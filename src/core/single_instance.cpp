#include "single_instance.h"

#include <QLocalSocket>
#include <QLoggingCategory>

#include <unistd.h>

Q_LOGGING_CATEGORY(lcInstance, "leansip.instance")

namespace leansip {

namespace {
constexpr qint64 kMaxMessage = 1024;
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

QByteArray SingleInstance::encodeMessage(const QString &number, const QString &activationToken)
{
    // Neither field may legitimately contain a tab or newline; strip them so the frame stays sane.
    auto clean = [](QString s) {
        s.remove(u'\t');
        s.remove(u'\n');
        return s.toUtf8();
    };
    return clean(number) + '\t' + clean(activationToken) + '\n';
}

void SingleInstance::decodeMessage(const QByteArray &line, QString &number, QString &activationToken)
{
    const QByteArray trimmed = line.trimmed();
    const qsizetype tab = trimmed.indexOf('\t');
    number = QString::fromUtf8(tab < 0 ? trimmed : trimmed.left(tab)).trimmed();
    activationToken = tab < 0 ? QString() : QString::fromUtf8(trimmed.mid(tab + 1)).trimmed();
}

bool SingleInstance::tryForward(const QString &number, int timeoutMs)
{
    QLocalSocket socket;
    socket.connectToServer(m_name);
    if (!socket.waitForConnected(timeoutMs))
        return false;
    socket.write(encodeMessage(number, qEnvironmentVariable("XDG_ACTIVATION_TOKEN")));
    socket.flush();
    socket.waitForBytesWritten(timeoutMs);
    // Wait for the ack so the primary has definitely read the message before we exit.
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
            QString number;
            QString token;
            decodeMessage(conn->readLine(kMaxMessage), number, token);
            conn->write("ok\n");
            conn->flush();
            conn->disconnectFromServer();
            emit numberReceived(number, token);
        });
        connect(conn, &QLocalSocket::disconnected, conn, &QObject::deleteLater);
    }
}

} // namespace leansip
