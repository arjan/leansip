#pragma once

#include <QLocalServer>
#include <QObject>
#include <QString>

namespace leansip {

// Ensures one running dialer per user. A second invocation hands its number (or an
// empty "raise" request) to the first and exits.
class SingleInstance : public QObject {
    Q_OBJECT
public:
    // `name` must be unique per user; see defaultName().
    explicit SingleInstance(const QString &name, QObject *parent = nullptr);

    static QString defaultName();

    // Returns true if another instance accepted the message (caller should exit).
    bool tryForward(const QString &number, int timeoutMs = 1000);

    // Becomes the primary instance. Returns false if the socket could not be created.
    bool listen();

signals:
    // Empty number means "raise the window".
    void numberReceived(const QString &number);

private:
    void onNewConnection();

    QString m_name;
    QLocalServer m_server;
};

} // namespace leansip
