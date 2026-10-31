// leansipd: owns one baresip for one baresip directory and shares it, through CtrlProxy, with
// the dialer and with command-line/agent clients (sipctl).

#include "core/baresip_client.h"
#include "core/baresip_process.h"
#include "core/ctrl_proxy.h"
#include "core/daemon_client.h"
#include "core/profile.h"
#include "core/unix_signals.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

#include <csignal>
#include <cstdio>

using namespace leansip;

namespace {
// A daemon the dialer started gives up if the dialer never binds to it (it died while starting).
constexpr int kBindGraceMs = 10000;
} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("leansipd"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Shares one baresip between the leansip dialer and sipctl"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption dirOpt({QStringLiteral("d"), QStringLiteral("baresip-dir")},
                                    QStringLiteral("baresip config directory"), QStringLiteral("dir"),
                                    profile::defaultBaresipDir());
    const QCommandLineOption uiOpt(QStringLiteral("ui-owned"),
                                   QStringLiteral("Started by the dialer: quit (stopping baresip) when the client that "
                                                  "sends leansipd_bind disconnects, even if other clients remain"));
    const QCommandLineOption traceOpt(QStringLiteral("sip-trace"),
                                      QStringLiteral("Run baresip with -s: SIP messages go to the log"));
    parser.addOption(dirOpt);
    parser.addOption(uiOpt);
    parser.addOption(traceOpt);
    parser.process(app);

    const QString baresipDir = QDir(parser.value(dirOpt)).absolutePath();
    const QString runtimeDir = profile::runtimeDir(baresipDir);
    const QString socketPath = profile::socketPath(baresipDir);
    const bool uiOwned = parser.isSet(uiOpt);

    if (!QDir().mkpath(runtimeDir)) {
        std::fprintf(stderr, "leansipd: cannot create %s\n", qPrintable(runtimeDir));
        return 1;
    }
    QFile::setPermissions(runtimeDir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);

    // Exactly one daemon per baresip directory. A socket nobody answers on is stale.
    if (daemon::isRunning(socketPath, 500)) {
        std::fprintf(stderr, "leansipd: already running for %s\n", qPrintable(baresipDir));
        return 0;
    }
    QLocalServer::removeServer(socketPath);

    QString error;
    if (!BaresipProcess::ensureUserConfig(baresipDir, &error)) {
        std::fprintf(stderr, "leansipd: %s\n", qPrintable(error));
        return 1;
    }

    BaresipClient upstream;
    CtrlProxy proxy(&upstream);
    if (!proxy.listen(socketPath, &error)) {
        std::fprintf(stderr, "leansipd: %s\n", qPrintable(error));
        return 1;
    }

    BaresipProcess process;
    process.setDerivedDir(runtimeDir);
    if (parser.isSet(traceOpt))
        process.setExtraArgs({QStringLiteral("-s")});
    if (!process.start(baresipDir)) {
        std::fprintf(stderr, "leansipd: cannot start baresip: %s\n", qPrintable(process.lastError()));
        return 1;
    }
    upstream.connectToHost(process.host(), process.port());
    QObject::connect(&process, &BaresipProcess::finished, &app, [&](int code, QProcess::ExitStatus) {
        proxy.setBackendUp(false, QStringLiteral("baresip exited (code %1)").arg(code));
    });

    proxy.addCommand(QStringLiteral("leansipd_status"), [&](const QString &) {
        QJsonObject s;
        s[QStringLiteral("baresipDir")] = baresipDir;
        s[QStringLiteral("runtimeDir")] = runtimeDir;
        s[QStringLiteral("socket")] = socketPath;
        s[QStringLiteral("log")] = profile::logPath(baresipDir);
        s[QStringLiteral("pid")] = QCoreApplication::applicationPid();
        s[QStringLiteral("baresipPid")] = process.pid();
        s[QStringLiteral("baresipUp")] = proxy.backendUp();
        s[QStringLiteral("clients")] = proxy.clientCount();
        s[QStringLiteral("uiOwned")] = uiOwned;
        s[QStringLiteral("uiAttached")] = proxy.hasOwner();
        s[QStringLiteral("sipTrace")] = parser.isSet(traceOpt);
        s[QStringLiteral("seq")] = proxy.lastSeq();
        return CtrlProxy::Result{true, QString::fromUtf8(QJsonDocument(s).toJson(QJsonDocument::Compact))};
    });
    // The dialer's account editor: run baresip on a candidate accounts file ("override") or on
    // the user's own file again (empty params).
    proxy.addCommand(QStringLiteral("leansipd_restart"), [&](const QString &params) {
        proxy.setBackendUp(false, QStringLiteral("restarting baresip"));
        upstream.disconnectFromHost();
        process.setAccountsOverride(params.trimmed() == QLatin1String("override"));
        if (!process.restart())
            return CtrlProxy::Result{false, process.lastError()};
        upstream.connectToHost(process.host(), process.port());
        return CtrlProxy::Result{true, QStringLiteral("restarted")};
    });
    proxy.addCommand(QStringLiteral("leansipd_shutdown"), [&](const QString &) {
        QTimer::singleShot(0, &app, &QCoreApplication::quit); // reply first
        return CtrlProxy::Result{true, QStringLiteral("shutting down")};
    });

    // The dialer that started us owns us: when its connection closes (quit or crash), so do we,
    // even if sipctl/agent clients are still connected.
    proxy.setOwnerBindingAllowed(uiOwned);
    QObject::connect(&proxy, &CtrlProxy::ownerGone, &app, [] {
        std::fputs("leansipd: the dialer that started us closed, exiting\n", stderr);
        QCoreApplication::quit();
    });
    if (uiOwned) {
        QTimer::singleShot(kBindGraceMs, &app, [&] {
            if (!proxy.hasOwner()) {
                std::fputs("leansipd: the dialer never attached, exiting\n", stderr);
                QCoreApplication::quit();
            }
        });
    }

    // A daemon outlives its terminal: ignore SIGHUP, stop cleanly on SIGINT/SIGTERM.
    std::signal(SIGHUP, SIG_IGN);
    UnixSignalBridge signalBridge;
    if (signalBridge.install({SIGINT, SIGTERM}))
        QObject::connect(&signalBridge, &UnixSignalBridge::signalReceived, &app, [] { QCoreApplication::quit(); });

    QObject::connect(&app, &QCoreApplication::aboutToQuit, &app, [&] {
        proxy.close();
        if (upstream.isConnected()) {
            upstream.sendCommand(QStringLiteral("quit"));
            upstream.disconnectFromHost();
        }
        process.stop();
    });

    std::fprintf(stderr, "leansipd: serving %s on %s\n", qPrintable(baresipDir), qPrintable(socketPath));
    return app.exec();
}
