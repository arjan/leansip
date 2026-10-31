#include "core/account_validator.h"
#include "core/accounts_parser.h"
#include "core/baresip_client.h"
#include "core/baresip_process.h"
#include "core/call_controller.h"
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
    parser.addOption(connectOpt);
    parser.addOption(dirOpt);
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

    BaresipProcess process;
    process.setDerivedDir(BaresipProcess::defaultDerivedDir() + profile);
    std::unique_ptr<AccountValidator> validator;
    if (!spawn) {
        const auto endpoint = parseEndpoint(parser.value(connectOpt));
        if (!endpoint) {
            std::fputs("leansip: --connect expects host:port\n", stderr);
            return 2;
        }
        client.connectToHost(endpoint->host, endpoint->port);
    } else {
        if (!process.start(baresipDir)) {
            QMessageBox::critical(nullptr, QStringLiteral("leansip"),
                                  QStringLiteral("Could not start baresip:\n%1").arg(process.lastError()));
            return 1;
        }
        QObject::connect(&process, &BaresipProcess::finished, &app, [&](int code, QProcess::ExitStatus) {
            if (window.isVisible())
                window.statusLabel()->setText(QStringLiteral("baresip exited (code %1)").arg(code));
        });
        client.connectToHost(process.host(), process.port());
        validator = std::make_unique<AccountValidator>(&process, &client, accountsPath);
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
        controller.hangup();
        if (client.isConnected()) {
            client.sendCommand(QStringLiteral("quit"));
            client.disconnectFromHost();
        }
        process.stop();
    });

    window.raiseWindow(); // show + request focus (uses the launcher's activation token, if any)
    if (!number.isEmpty())
        window.dialNumber(number);
    if (accountsFile.accounts.isEmpty() && validator)
        QTimer::singleShot(0, &window, openAccounts); // nothing to dial with yet: set up an account first

    return app.exec();
}
