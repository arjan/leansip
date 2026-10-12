#include "baresip_client.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcClient, "leansip.client")

namespace leansip {

namespace {
// Short so we attach almost as soon as baresip binds, before its first REGISTER completes.
constexpr int kRetryIntervalMs = 50;
}

BaresipClient::BaresipClient(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<BaresipEvent>();
    qRegisterMetaType<BaresipResponse>();

    m_retryTimer.setSingleShot(true);
    connect(&m_retryTimer, &QTimer::timeout, this, &BaresipClient::attemptConnect);

    connect(&m_socket, &QTcpSocket::connected, this, [this] {
        m_connecting = false;
        m_wasConnected = true;
        m_decoder.reset();
        qCInfo(lcClient) << "connected to" << m_host << m_port;
        emit connected();
    });
    connect(&m_socket, &QTcpSocket::disconnected, this, [this] {
        if (m_wasConnected) {
            m_wasConnected = false;
            qCInfo(lcClient) << "disconnected";
            emit disconnected();
        }
    });
    connect(&m_socket, &QTcpSocket::errorOccurred, this, &BaresipClient::onSocketError);
    connect(&m_socket, &QTcpSocket::readyRead, this, &BaresipClient::onReadyRead);
}

void BaresipClient::connectToHost(const QString &host, quint16 port, int timeoutMs)
{
    m_host = host;
    m_port = port;
    m_timeoutMs = timeoutMs;
    m_connecting = true;
    m_connectClock.start();
    attemptConnect();
}

void BaresipClient::attemptConnect()
{
    if (!m_connecting)
        return;
    m_socket.abort();
    m_socket.connectToHost(m_host, m_port);
}

void BaresipClient::onSocketError(QAbstractSocket::SocketError error)
{
    if (m_connecting) {
        if (m_connectClock.elapsed() < m_timeoutMs) {
            m_retryTimer.start(kRetryIntervalMs);
            return;
        }
        m_connecting = false;
        qCWarning(lcClient) << "connection failed:" << m_socket.errorString();
        emit connectionFailed(m_socket.errorString());
        return;
    }
    if (error == QAbstractSocket::RemoteHostClosedError)
        return; // `disconnected` follows
    if (m_wasConnected) {
        m_wasConnected = false;
        emit disconnected();
    }
}

void BaresipClient::disconnectFromHost()
{
    m_connecting = false;
    m_retryTimer.stop();
    m_socket.disconnectFromHost();
}

bool BaresipClient::isConnected() const
{
    return m_socket.state() == QAbstractSocket::ConnectedState;
}

QString BaresipClient::sendCommand(const QString &command, const QString &params)
{
    if (!isConnected())
        return {};
    const QString token = QString::number(m_nextToken++);
    QJsonObject obj;
    obj[QStringLiteral("command")] = command;
    obj[QStringLiteral("params")] = params;
    obj[QStringLiteral("token")] = token;
    const QByteArray frame = netstring::encode(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    m_socket.write(frame);
    return token;
}

void BaresipClient::onReadyRead()
{
    const QList<QByteArray> frames = m_decoder.feed(m_socket.readAll());
    for (const QByteArray &frame : frames)
        handleFrame(frame);
    if (m_decoder.failed()) {
        qCWarning(lcClient) << "framing error:" << m_decoder.lastError();
        emit protocolError(m_decoder.lastError());
        m_socket.abort();
    }
}

void BaresipClient::handleFrame(const QByteArray &payload)
{
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(payload, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        qCWarning(lcClient) << "bad JSON frame:" << err.errorString();
        emit protocolError(QStringLiteral("bad JSON: %1").arg(err.errorString()));
        return;
    }
    const QJsonObject obj = doc.object();
    if (auto ev = BaresipEvent::fromJson(obj)) {
        emit eventReceived(*ev);
        return;
    }
    if (auto resp = BaresipResponse::fromJson(obj)) {
        emit responseReceived(*resp);
        return;
    }
    qCWarning(lcClient) << "unrecognised frame:" << payload.left(200);
}

} // namespace leansip
