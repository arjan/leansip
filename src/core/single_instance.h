#pragma once

#include <QLocalServer>
#include <QObject>
#include <QString>

namespace leansip {

// Ensures one running dialer per user and profile (the name carries the profile). A second invocation hands its number (or an
// empty "raise" request) to the first and exits.
//
// Wayland compositors only let a window take focus with an activation token, which the
// launcher gives the *new* process in $XDG_ACTIVATION_TOKEN. The token is therefore forwarded
// alongside the number so the primary can use it when raising its window.
class SingleInstance : public QObject {
    Q_OBJECT
public:
    // `name` must be unique per user; see defaultName().
    explicit SingleInstance(const QString &name, QObject *parent = nullptr);

    static QString defaultName();

    // Returns true if another instance accepted the message (caller should exit).
    // Forwards this process's $XDG_ACTIVATION_TOKEN, if any.
    bool tryForward(const QString &number, int timeoutMs = 1000);

    // Becomes the primary instance. Returns false if the socket could not be created.
    bool listen();

    // Wire format helpers, exposed for tests: "<number>\t<token>\n".
    static QByteArray encodeMessage(const QString &number, const QString &activationToken);
    static void decodeMessage(const QByteArray &line, QString &number, QString &activationToken);

signals:
    // Empty number means "raise the window". `activationToken` may be empty.
    void numberReceived(const QString &number, const QString &activationToken);

private:
    void onNewConnection();

    QString m_name;
    QLocalServer m_server;
};

} // namespace leansip
