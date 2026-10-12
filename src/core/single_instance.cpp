#include "single_instance.h"

#include <QLocalSocket>
#include <QLoggingCategory>

#include <unistd.h>

Q_LOGGING_CATEGORY(lcInstance, "leansip.instance")

namespace leansip {

namespace {
constexpr qint64 kMaxMessage = 512;
}

SingleInstance::SingleInstance(const QString &name, QObject *parent)
    : QObject(parent)
    , m_name(name)
{
    // Only this user may connect; the socket lives in the abstract/runtime namespace.
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    connect(&m_server, &QLocalServer::newConnection, this, &SingleInstance::onNewConnection);
}

QString SingleInstance::defaultName()
{
    return QStringLiteral("leansip-%1").arg(::getuid());
}

bool SingleInstance::tryForward(const QString &number, int timeoutMs)
{
    QLocalSocket socket;
    socket.connectToServer(m_name);
    if (!socket.waitForConnected(timeoutMs))
        return false;
    socket.write(number.toUtf8() + '\n');
    socket.flush();
    socket.waitForBytesWritten(timeoutMs);
    // Wait for the ack so the primary has definitely read the number before we exit.
    socket.waitForReadyRead(timeoutMs);
    const bool acked = socket.readAll().startsWith("ok");
    socket.disconnectFromServer();
    return acked;
}

bool SingleInstance::listen()
{
    if (m_server.listen(m_name))
        return true;
    // A stale socket file from a crashed instance: remove and retry once. Only safe because
    // tryForward() just failed to connect to it.
    if (m_server.serverError() == QAbstractSocket::AddressInUseError) {
        QLocalServer::removeServer(m_name);
        if (m_server.listen(m_name))
            return true;
    }
    qCWarning(lcInstance) << "cannot listen on" << m_name << m_server.errorString();
    return false;
}

void SingleInstance::onNewConnection()
{
    while (QLocalSocket *conn = m_server.nextPendingConnection()) {
        connect(conn, &QLocalSocket::readyRead, this, [this, conn] {
            if (!conn->canReadLine()) {
                if (conn->bytesAvailable() > kMaxMessage)
                    conn->abort();
                return;
            }
            const QString number = QString::fromUtf8(conn->readLine(kMaxMessage)).trimmed();
            conn->write("ok\n");
            conn->flush();
            conn->disconnectFromServer();
            emit numberReceived(number);
        });
        connect(conn, &QLocalSocket::disconnected, conn, &QObject::deleteLater);
    }
}

} // namespace leansip
