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

    connect(&m_socket, &QTcpSocket::connected, this, &BaresipClient::onConnected);
    connect(&m_socket, &QTcpSocket::disconnected, this, &BaresipClient::onDisconnected);
    connect(&m_socket, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError error) {
        if (m_transport == Transport::Tcp)
            handleError(error == QAbstractSocket::RemoteHostClosedError, m_socket.errorString());
    });
    connect(&m_socket, &QTcpSocket::readyRead, this, &BaresipClient::onReadyRead);

    connect(&m_local, &QLocalSocket::connected, this, &BaresipClient::onConnected);
    connect(&m_local, &QLocalSocket::disconnected, this, &BaresipClient::onDisconnected);
    connect(&m_local, &QLocalSocket::errorOccurred, this, [this](QLocalSocket::LocalSocketError error) {
        if (m_transport == Transport::Local)
            handleError(error == QLocalSocket::PeerClosedError, m_local.errorString());
    });
    connect(&m_local, &QLocalSocket::readyRead, this, &BaresipClient::onReadyRead);
}

void BaresipClient::onConnected()
{
    m_connecting = false;
    m_wasConnected = true;
    m_decoder.reset();
    qCInfo(lcClient) << "connected to" << (m_transport == Transport::Local ? m_path : m_host) << m_port;
    emit connected();
}

void BaresipClient::onDisconnected()
{
    if (m_wasConnected) {
        m_wasConnected = false;
        qCInfo(lcClient) << "disconnected";
        emit disconnected();
    }
}

QIODevice *BaresipClient::device()
{
    return m_transport == Transport::Local ? static_cast<QIODevice *>(&m_local) : &m_socket;
}

void BaresipClient::connectToHost(const QString &host, quint16 port, int timeoutMs)
{
    m_local.abort();
    m_transport = Transport::Tcp;
    m_host = host;
    m_port = port;
    m_timeoutMs = timeoutMs;
    m_connecting = true;
    m_connectClock.start();
    attemptConnect();
}

void BaresipClient::connectToServer(const QString &socketPath, int timeoutMs)
{
    m_socket.abort();
    m_transport = Transport::Local;
    m_path = socketPath;
    m_host.clear();
    m_port = 0;
    m_timeoutMs = timeoutMs;
    m_connecting = true;
    m_connectClock.start();
    attemptConnect();
}

void BaresipClient::attemptConnect()
{
    if (!m_connecting)
        return;
    if (m_transport == Transport::Local) {
        m_local.abort();
        m_local.connectToServer(m_path);
    } else {
        m_socket.abort();
        m_socket.connectToHost(m_host, m_port);
    }
}

void BaresipClient::handleError(bool peerClosed, const QString &errorString)
{
    if (m_connecting) {
        if (m_connectClock.elapsed() < m_timeoutMs) {
            m_retryTimer.start(kRetryIntervalMs);
            return;
        }
        m_connecting = false;
        qCWarning(lcClient) << "connection failed:" << errorString;
        emit connectionFailed(errorString);
        return;
    }
    if (peerClosed)
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
    // A command sent right before disconnecting (hangup on quit) must still reach the peer.
    if (isConnected() && device()->bytesToWrite() > 0) {
        if (m_transport == Transport::Local)
            m_local.waitForBytesWritten(500);
        else
            m_socket.waitForBytesWritten(500);
    }
    if (m_transport == Transport::Local)
        m_local.disconnectFromServer();
    else
        m_socket.disconnectFromHost();
}

bool BaresipClient::isConnected() const
{
    if (m_transport == Transport::Local)
        return m_local.state() == QLocalSocket::ConnectedState;
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
    qCDebug(lcClient) << "send" << command << params << "token" << token;
    device()->write(frame);
    return token;
}

void BaresipClient::onReadyRead()
{
    const QList<QByteArray> frames = m_decoder.feed(device()->readAll());
    for (const QByteArray &frame : frames)
        handleFrame(frame);
    if (m_decoder.failed()) {
        qCWarning(lcClient) << "framing error:" << m_decoder.lastError();
        emit protocolError(m_decoder.lastError());
        if (m_transport == Transport::Local)
            m_local.abort();
        else
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
        emit rawEventReceived(obj);
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
