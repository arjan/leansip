#include "core/baresip_client.h"
#include "core/call_controller.h"
#include "core/daemon_client.h"
#include "core/profile.h"
#include "core/settings.h"
#include "core/single_instance.h"
#include "core/unix_signals.h"
#include "ui/main_window.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QMessageBox>
#include <QTimer>

#include <csignal>
#include <memory>
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

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("leansip"));
    QApplication::setApplicationVersion(QStringLiteral(LEANSIP_VERSION));
    QApplication::setDesktopFileName(QStringLiteral("leansip")); // Wayland app id: icon and .desktop match
    QApplication::setQuitOnLastWindowClosed(true);

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Minimal Qt dialer frontend for baresip"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("number"), QStringLiteral("Number or SIP URI to dial immediately"));
    const QCommandLineOption connectOpt({QStringLiteral("c"), QStringLiteral("connect")},
                                        QStringLiteral("Attach to a running baresip ctrl_tcp at host:port instead of spawning one"),
                                        QStringLiteral("host:port"));
    const QCommandLineOption profileOpt({QStringLiteral("p"), QStringLiteral("profile")},
                                        QStringLiteral("Profile in the config file (default: $LEANSIP_PROFILE, "
                                                       "then its default_profile)"),
                                        QStringLiteral("name"));
    const QCommandLineOption configOpt(QStringLiteral("config"),
                                       QStringLiteral("Config file (default: $LEANSIP_CONFIG or ~/.config/leansip/leansip.toml)"),
                                       QStringLiteral("file"), settings::defaultPath());
    const QCommandLineOption recordOpt(QStringLiteral("record"),
                                       QStringLiteral("Record calls (both directions) when this window starts leansipd"));
    parser.addOption(connectOpt);
    parser.addOption(profileOpt);
    parser.addOption(configOpt);
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

    const QString configPath = QDir(parser.value(configOpt)).absolutePath();
    QString loadError;
    const auto config = settings::load(configPath, &loadError);
    const auto selected = config ? settings::select(*config, parser.value(profileOpt), &loadError) : std::nullopt;
    if (!selected) {
        std::fprintf(stderr, "leansip: %s\n", qPrintable(loadError));
        QMessageBox::critical(nullptr, QStringLiteral("leansip"), loadError);
        return 1;
    }
    // The profile this window serves; Ctrl+<digit> can switch it.
    settings::Profile current = *selected;

    // One dialer per profile: another profile runs as its own window.
    auto instanceName = [](const QString &profileName) { return SingleInstance::defaultName() + u'-' + profileName; };
    auto instance = std::make_unique<SingleInstance>(instanceName(current.name));
    if (instance->tryForward(number))
        return 0;
    if (!instance->listen())
        std::fputs("leansip: single-instance socket unavailable; continuing without it\n", stderr);

    const bool spawn = !parser.isSet(connectOpt);

    BaresipClient client;
    CallController controller(&client);
    controller.setAccountAor(current.aor);
    MainWindow window(current.info(), &controller);

    // Ctrl-C / SIGTERM must still run aboutToQuit so the child baresip is stopped cleanly.
    UnixSignalBridge signalBridge;
    if (signalBridge.install({SIGINT, SIGTERM, SIGHUP}))
        QObject::connect(&signalBridge, &UnixSignalBridge::signalReceived, &app, [] { QApplication::quit(); });

    bool quitting = false;
    int reattachAttempts = 0; // outlives the lambdas below, unlike a block-scoped local
    int generation = 0;       // bumped on a profile switch: reconnects for the old one are void
    QStringList daemonArgs{QStringLiteral("--ui-owned")};
    bool switching = false;   // a profile switch is waiting for its daemon
    if (!spawn) {
        const auto endpoint = parseEndpoint(parser.value(connectOpt));
        if (!endpoint) {
            std::fputs("leansip: --connect expects host:port\n", stderr);
            return 2;
        }
        client.connectToHost(endpoint->host, endpoint->port);
    } else {
        // baresip belongs to leansipd, shared with leansipctl/agents. A daemon we start is ours: we
        // bind to it, and it stops when this window closes. One that leansipctl started is left alone.
        if (parser.isSet(recordOpt))
            daemonArgs << QStringLiteral("--record");
        controller.setDaemonMode(true);
        QString error;
        if (!daemon::ensureRunning(current.name, configPath, daemonArgs, &error)) {
            QMessageBox::critical(nullptr, QStringLiteral("leansip"), QStringLiteral("Could not start leansipd:\n%1").arg(error));
            return 1;
        }

        // If the daemon goes away under us (crash, `leansipctl stop`), bring one back.
        QObject::connect(&client, &BaresipClient::disconnected, &app, [&] {
            if (quitting || reattachAttempts >= 3)
                return;
            ++reattachAttempts;
            QTimer::singleShot(300 * reattachAttempts, &app, [&, gen = generation] {
                if (quitting || gen != generation || client.isConnected())
                    return;
                QString err;
                if (daemon::ensureRunning(current.name, configPath, daemonArgs, &err))
                    client.connectToServer(profile::socketPath(current.name));
                else
                    window.statusLabel()->setText(err);
            });
        });
        QObject::connect(&client, &BaresipClient::connected, &app, [&] {
            reattachAttempts = 0;
            // We are the dialer: our calls have no time limit. Binding is refused (harmlessly)
            // by a daemon we did not start. Neither reply is tracked.
            client.sendCommand(QStringLiteral("leansipd_hello"), QStringLiteral("ui"));
            client.sendCommand(QStringLiteral("leansipd_bind"));
        });
        // Only now: a Unix socket may connect synchronously, inside this call.
        client.connectToServer(profile::socketPath(current.name));
    }

    auto wireInstance = [&] {
        QObject::connect(instance.get(), &SingleInstance::numberReceived, &window,
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
    };
    wireInstance();

    // Alt+<digit>: the current profile's phonebook entry, from the file as it is now.
    QObject::connect(&window, &MainWindow::phonebookEntry, &app, [&](int digit) {
        QString err;
        const auto file = settings::load(configPath, &err);
        if (!file) {
            window.statusLabel()->setText(err);
            return;
        }
        const QString entry = file->profiles.value(current.name).phonebook.value(digit);
        if (entry.isEmpty()) {
            window.statusLabel()->setText(QStringLiteral("No phonebook entry for Alt+%1").arg(digit));
            return;
        }
        window.fillNumber(entry);
    });
    QObject::connect(&window, &MainWindow::phonebookSave, &app,
                     [&](int digit, const QString &entry, bool profileOnly) {
        const auto saved = settings::savePhonebookEntry(configPath, profileOnly ? current.name : QString(), digit, entry);
        if (!saved.ok) {
            window.statusLabel()->setText(saved.error);
            return;
        }
        QString text = profileOnly ? QStringLiteral("Saved as Alt+%1 for this profile").arg(digit)
                                   : QStringLiteral("Saved as Alt+%1 for all profiles").arg(digit);
        // A profile's own entry wins over the shared one, so say when that hides the new number.
        QString err;
        if (const auto file = settings::load(configPath, &err);
            !profileOnly && file && file->profiles.value(current.name).phonebook.value(digit) != entry) {
            text = QStringLiteral("Saved for all profiles, but this profile's own Alt+%1 wins").arg(digit);
        }
        window.statusLabel()->setText(text);
    });

    // Ctrl+<digit>: serve the profile with that shortcut in this window. The old profile's
    // daemon stops with the connection if this window started it (and unregisters), as when
    // the window closes; one leansipctl started keeps running.
    QObject::connect(&window, &MainWindow::profileShortcut, &app, [&](int digit) {
        auto say = [&](const QString &text) { window.statusLabel()->setText(text); };
        if (!spawn) {
            say(QStringLiteral("Profiles cannot be switched with --connect"));
            return;
        }
        QString err;
        const auto file = settings::load(configPath, &err); // the file as it is now
        if (!file) {
            say(err);
            return;
        }
        const auto target = settings::byShortcut(*file, digit);
        if (!target) {
            say(QStringLiteral("No profile has shortcut = %1").arg(digit));
            return;
        }
        if (target->name == current.name || switching)
            return;
        if (controller.callState() != CallController::CallState::Idle) {
            say(QStringLiteral("Hang up before switching profiles"));
            return;
        }
        // Another window already serves it: bring that one up instead.
        if (SingleInstance(instanceName(target->name)).tryForward({}))
            return;

        ++generation;
        reattachAttempts = 0;
        client.disconnectFromHost();
        current = *target;
        instance = std::make_unique<SingleInstance>(instanceName(current.name));
        if (!instance->listen())
            std::fputs("leansip: single-instance socket unavailable; continuing without it\n", stderr);
        wireInstance();
        controller.setAccountAor(current.aor);
        window.setAccount(current.info());
        say(QStringLiteral("Starting %1…").arg(current.info().label()));
        switching = true;
        QCoreApplication::processEvents(); // show it: password_cmd may take a while
        const bool started = daemon::ensureRunning(current.name, configPath, daemonArgs, &err);
        switching = false;
        if (!started) {
            say(err.simplified());
            return;
        }
        client.connectToServer(profile::socketPath(current.name));
        window.statusLabel()->clear();
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

    return app.exec();
}
