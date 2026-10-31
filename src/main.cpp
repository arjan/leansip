#include "core/account_validator.h"
#include "core/accounts_parser.h"
#include "core/baresip_client.h"
#include "core/baresip_process.h"
#include "core/call_controller.h"
#include "core/daemon_client.h"
#include "core/profile.h"
#include "core/single_instance.h"
#include "core/unix_signals.h"
#include "ui/accounts_dialog.h"
#include "ui/main_window.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QMessageBox>
#include <QPointer>
#include <QTimer>
#include <QUrl>

#include <csignal>
#include <cstdio>

using namespace leansip;

namespace {

// Accepts "0612345678", "tel:+31612345678", "sip:alice@example.com", "sips:…".
QString normaliseNumber(QString arg)
{
    arg = arg.trimmed();
    if (arg.startsWith(QLatin1String("tel:"), Qt::CaseInsensitive))
        arg = arg.mid(4);
    return arg;
}

struct Endpoint {
    QString host;
    quint16 port = 0;
};

std::optional<Endpoint> parseEndpoint(const QString &spec)
{
    const qsizetype colon = spec.lastIndexOf(u':');
    if (colon <= 0)
        return std::nullopt;
    bool ok = false;
    const uint port = spec.mid(colon + 1).toUInt(&ok);
    if (!ok || port == 0 || port > 65535)
        return std::nullopt;
    return Endpoint{spec.left(colon), static_cast<quint16>(port)};
}

AccountInfo firstAccountOrPlaceholder(const AccountsFile &file)
{
    if (!file.accounts.isEmpty())
        return file.accounts.first().info();
    AccountInfo none;
    none.displayName = QObject::tr("no account — press F2");
    return none;
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("leansip"));
    QApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QApplication::setQuitOnLastWindowClosed(true);

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Minimal Qt dialer frontend for baresip"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("number"), QStringLiteral("Number or SIP URI to dial immediately"));
    const QCommandLineOption connectOpt({QStringLiteral("c"), QStringLiteral("connect")},
                                        QStringLiteral("Attach to a running baresip ctrl_tcp at host:port instead of spawning one"),
                                        QStringLiteral("host:port"));
    const QCommandLineOption dirOpt({QStringLiteral("d"), QStringLiteral("baresip-dir")},
                                    QStringLiteral("baresip config directory"), QStringLiteral("dir"),
                                    QDir::home().filePath(QStringLiteral(".baresip")));
    const QCommandLineOption recordOpt(QStringLiteral("record"),
                                       QStringLiteral("Record calls (both directions) when this window starts leansipd"));
    parser.addOption(connectOpt);
    parser.addOption(dirOpt);
    parser.addOption(recordOpt);
    parser.process(app);

    const QStringList positional = parser.positionalArguments();
    if (positional.size() > 1) {
        std::fputs("leansip: at most one number argument is accepted\n", stderr);
        return 2;
    }
    const QString number = positional.isEmpty() ? QString() : normaliseNumber(positional.first());
    if (!number.isEmpty() && !CallController::isValidNumber(number)) {
        std::fprintf(stderr, "leansip: invalid number '%s'\n", qPrintable(number));
        return 2;
    }

    // One dialer per baresip directory: a second account runs as its own window.
    const QString baresipDir = parser.value(dirOpt);
    const QString profile = SingleInstance::profileSuffix(baresipDir);
    SingleInstance instance(SingleInstance::defaultName() + profile);
    if (instance.tryForward(number))
        return 0;
    if (!instance.listen())
        std::fputs("leansip: single-instance socket unavailable; continuing without it\n", stderr);

    const bool spawn = !parser.isSet(connectOpt);
    if (spawn) {
        // First run on a fresh machine: let baresip lay down its template config.
        QString error;
        if (!BaresipProcess::ensureUserConfig(baresipDir, &error)) {
            QMessageBox::critical(nullptr, QStringLiteral("leansip"), error);
            return 1;
        }
    }

    const QString accountsPath = QDir(baresipDir).filePath(QStringLiteral("accounts"));
    AccountsFile accountsFile;
    QString loadError;
    if (!accounts::readFile(accountsPath, &accountsFile, &loadError)) {
        QMessageBox::critical(nullptr, QStringLiteral("leansip"), loadError);
        return 1;
    }
    const AccountInfo initialAccount = firstAccountOrPlaceholder(accountsFile);

    BaresipClient client;
    CallController controller(&client);
    controller.setAccountAor(accountsFile.accounts.isEmpty() ? QString() : initialAccount.aor);
    MainWindow window(initialAccount, &controller);

    // Ctrl-C / SIGTERM must still run aboutToQuit so the child baresip is stopped cleanly.
    UnixSignalBridge signalBridge;
    if (signalBridge.install({SIGINT, SIGTERM, SIGHUP}))
        QObject::connect(&signalBridge, &UnixSignalBridge::signalReceived, &app, [] { QApplication::quit(); });

    std::unique_ptr<daemon::DaemonBackend> backend;
    std::unique_ptr<AccountValidator> validator;
    bool quitting = false;
    int reattachAttempts = 0; // outlives the lambdas below, unlike a block-scoped local
    if (!spawn) {
        const auto endpoint = parseEndpoint(parser.value(connectOpt));
        if (!endpoint) {
            std::fputs("leansip: --connect expects host:port\n", stderr);
            return 2;
        }
        client.connectToHost(endpoint->host, endpoint->port);
    } else {
        // baresip belongs to leansipd, shared with sipctl/agents. A daemon we start is ours: we
        // bind to it, and it stops when this window closes. One that sipctl started is left alone.
        QStringList daemonArgs{QStringLiteral("--ui-owned")};
        if (parser.isSet(recordOpt))
            daemonArgs << QStringLiteral("--record");
        controller.setDaemonMode(true);
        QString error;
        if (!daemon::ensureRunning(baresipDir, daemonArgs, &error)) {
            QMessageBox::critical(nullptr, QStringLiteral("leansip"), QStringLiteral("Could not start leansipd:\n%1").arg(error));
            return 1;
        }
        const QString socketPath = profile::socketPath(baresipDir);
        backend = std::make_unique<daemon::DaemonBackend>(baresipDir);
        validator = std::make_unique<AccountValidator>(backend.get(), &client, accountsPath);

        // If the daemon goes away under us (crash, `sipctl stop`), bring one back.
        auto reattach = [&, daemonArgs, socketPath] {
            if (quitting)
                return;
            QString err;
            if (daemon::ensureRunning(baresipDir, daemonArgs, &err))
                client.connectToServer(socketPath);
            else
                window.statusLabel()->setText(err);
        };
        QObject::connect(&client, &BaresipClient::disconnected, &app, [&, reattach] {
            if (quitting || reattachAttempts >= 3)
                return;
            ++reattachAttempts;
            QTimer::singleShot(300 * reattachAttempts, &app, reattach);
        });
        QObject::connect(&client, &BaresipClient::connected, &app, [&] {
            reattachAttempts = 0;
            // We are the dialer: our calls have no time limit. Binding is refused (harmlessly)
            // by a daemon we did not start. Neither reply is tracked.
            client.sendCommand(QStringLiteral("leansipd_hello"), QStringLiteral("ui"));
            client.sendCommand(QStringLiteral("leansipd_bind"));
        });
        // Only now: a Unix socket may connect synchronously, inside this call.
        client.connectToServer(socketPath);
    }

    // F2: the accounts editor. Only one at a time; it needs the spawned baresip to validate.
    QPointer<AccountsDialog> dialog;
    auto openAccounts = [&] {
        if (dialog) {
            dialog->raise();
            dialog->activateWindow();
            return;
        }
        if (!validator) {
            window.statusLabel()->setText(QStringLiteral("account editing needs a spawned baresip (not --connect)"));
            return;
        }
        AccountsFile current;
        QString error;
        if (!accounts::readFile(accountsPath, &current, &error)) {
            window.statusLabel()->setText(error);
            return;
        }
        dialog = new AccountsDialog(current, validator.get(), &window);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setModal(true);
        dialog->setCallActive(controller.callState() != CallController::CallState::Idle);
        QObject::connect(dialog, &AccountsDialog::saved, &window, [&](const AccountsFile &saved) {
            const AccountInfo active = firstAccountOrPlaceholder(saved);
            window.setAccount(active);
            controller.setAccountAor(saved.accounts.isEmpty() ? QString() : active.aor);
        });
        dialog->show();
    };
    QObject::connect(&window, &MainWindow::configureRequested, &window, openAccounts);
    QObject::connect(&controller, &CallController::callStateChanged, &window, [&](CallController::CallState state) {
        if (dialog)
            dialog->setCallActive(state != CallController::CallState::Idle);
    });

    QObject::connect(&instance, &SingleInstance::numberReceived, &window,
                     [&](const QString &n, const QString &activationToken) {
        // QtWayland consumes XDG_ACTIVATION_TOKEN from the environment on activateWindow();
        // without a token the compositor refuses focus to an already-running app.
        if (!activationToken.isEmpty())
            qputenv("XDG_ACTIVATION_TOKEN", activationToken.toUtf8());
        if (n.isEmpty())
            window.raiseWindow();
        else
            window.dialNumber(n);
    });

    QObject::connect(&app, &QApplication::aboutToQuit, &app, [&] {
        quitting = true;
        // Closing the dialer ends the user's own call, not one an agent is running.
        if (controller.ownsCall())
            controller.hangup();
        if (!spawn && client.isConnected())
            client.sendCommand(QStringLiteral("quit")); // --connect: we own nothing else to stop
        client.disconnectFromHost();
    });

    window.raiseWindow(); // show + request focus (uses the launcher's activation token, if any)
    if (!number.isEmpty())
        window.dialNumber(number);
    if (accountsFile.accounts.isEmpty() && validator)
        QTimer::singleShot(0, &window, openAccounts); // nothing to dial with yet: set up an account first

    return app.exec();
}
