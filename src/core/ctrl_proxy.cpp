#include "ctrl_proxy.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcProxy, "leansip.proxy")

namespace leansip {

namespace {
const QString kPrefix = QStringLiteral("leansipd_");
}

CtrlProxy::CtrlProxy(BaresipClient *upstream, QObject *parent)
    : QObject(parent)
    , m_upstream(upstream)
{
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    connect(&m_server, &QLocalServer::newConnection, this, &CtrlProxy::onNewConnection);

    connect(m_upstream, &BaresipClient::responseReceived, this, &CtrlProxy::onUpstreamResponse);
    connect(m_upstream, &BaresipClient::rawEventReceived, this, [this](const QJsonObject &ev) { publish(ev); });
    connect(m_upstream, &BaresipClient::connected, this, [this] { setBackendUp(true); });
    connect(m_upstream, &BaresipClient::disconnected, this,
            [this] { setBackendUp(false, QStringLiteral("connection to baresip lost")); });
    connect(m_upstream, &BaresipClient::connectionFailed, this,
            [this](const QString &err) { setBackendUp(false, QStringLiteral("cannot reach baresip: %1").arg(err)); });

    addCommand(QStringLiteral("leansipd_events"), [this](const QString &params) {
        bool ok = false;
        const qint64 since = params.trimmed().isEmpty() ? 0 : params.trimmed().toLongLong(&ok);
        if (!params.trimmed().isEmpty() && !ok)
            return Result{false, QStringLiteral("expected a sequence number")};
        return Result{true, QString::fromUtf8(QJsonDocument(eventsSince(since)).toJson(QJsonDocument::Compact))};
    });
}

CtrlProxy::~CtrlProxy()
{
    close();
}

bool CtrlProxy::listen(const QString &path, QString *error)
{
    if (m_server.listen(path))
        return true;
    if (error)
        *error = QStringLiteral("cannot listen on %1: %2").arg(path, m_server.errorString());
    return false;
}

void CtrlProxy::close()
{
    // Copy: aborting a socket can re-enter onClientGone and mutate the map.
    std::vector<QLocalSocket *> clients;
    for (const auto &entry : m_clients)
        clients.push_back(entry.first);
    for (QLocalSocket *c : clients) {
        // Deliver what is queued (e.g. the reply to leansipd_shutdown) before hanging up.
        if (c->state() == QLocalSocket::ConnectedState && c->bytesToWrite() > 0)
            c->waitForBytesWritten(200);
        c->abort();
    }
    m_clients.clear();
    m_server.close();
}

void CtrlProxy::addCommand(const QString &name, Handler handler)
{
    m_commands.insert(name, std::move(handler));
}

void CtrlProxy::setBackendUp(bool up, const QString &reason)
{
    if (m_backendUp == up)
        return;
    m_backendUp = up;
    if (!up)
        failPending(reason.isEmpty() ? QStringLiteral("baresip went away") : reason);
    QJsonObject ev;
    ev[QStringLiteral("event")] = true;
    ev[QStringLiteral("class")] = QStringLiteral("leansipd");
    ev[QStringLiteral("type")] = up ? QStringLiteral("BACKEND_UP") : QStringLiteral("BACKEND_DOWN");
    ev[QStringLiteral("param")] = reason;
    qCInfo(lcProxy) << (up ? "backend up" : "backend down") << reason;
    publish(ev);
}

QJsonArray CtrlProxy::eventsSince(qint64 since) const
{
    QJsonArray out;
    for (const QJsonObject &ev : m_journal) {
        if (ev.value(QStringLiteral("seq")).toInteger() > since)
            out.append(ev);
    }
    return out;
}

void CtrlProxy::onNewConnection()
{
    while (QLocalSocket *client = m_server.nextPendingConnection()) {
        if (clientCount() >= kMaxClients) {
            qCWarning(lcProxy) << "too many clients, refusing one";
            client->abort();
            client->deleteLater();
            continue;
        }
        m_clients.emplace(client, netstring::Decoder{});
        connect(client, &QLocalSocket::readyRead, this, [this, client] { onClientData(client); });
        connect(client, &QLocalSocket::disconnected, this, [this, client] { onClientGone(client); });
        qCInfo(lcProxy) << "client connected," << clientCount() << "total";
        emit clientCountChanged(clientCount());
        // Data may have arrived together with the connection.
        if (client->bytesAvailable() > 0)
            onClientData(client);
    }
}

void CtrlProxy::onClientGone(QLocalSocket *client)
{
    if (m_clients.erase(client) == 0)
        return;
    client->deleteLater();
    if (client == m_owner) {
        m_owner = nullptr;
        qCInfo(lcProxy) << "owner disconnected";
        emit ownerGone();
    }
    qCInfo(lcProxy) << "client disconnected," << clientCount() << "left";
    emit clientCountChanged(clientCount());
}

void CtrlProxy::onClientData(QLocalSocket *client)
{
    auto it = m_clients.find(client);
    if (it == m_clients.end())
        return;
    const QList<QByteArray> frames = it->second.feed(client->readAll());
    const bool failed = it->second.failed();
    for (const QByteArray &frame : frames) {
        const QJsonDocument doc = QJsonDocument::fromJson(frame);
        if (!doc.isObject()) {
            reply(client, {}, false, QStringLiteral("bad JSON"), m_seq);
            continue;
        }
        handleRequest(client, doc.object());
    }
    if (failed) {
        qCWarning(lcProxy) << "client framing error, dropping it";
        client->abort();
    }
}

void CtrlProxy::handleRequest(QLocalSocket *client, const QJsonObject &request)
{
    const QString command = request.value(QStringLiteral("command")).toString();
    const QString params = request.value(QStringLiteral("params")).toString();
    const QJsonValue tokenValue = request.value(QStringLiteral("token"));
    const QString token = tokenValue.isString() ? tokenValue.toString()
                                                : (tokenValue.isDouble() ? QString::number(tokenValue.toInteger()) : QString());
    const qint64 seq = m_seq;

    if (command.isEmpty()) {
        reply(client, token, false, QStringLiteral("missing command"), seq);
        return;
    }
    if (command == QLatin1String("leansipd_bind")) {
        if (!m_bindingAllowed)
            reply(client, token, false, QStringLiteral("not bindable: this daemon was not started by the dialer"), seq);
        else if (m_owner && m_owner != client)
            reply(client, token, false, QStringLiteral("already bound to another client"), seq);
        else {
            m_owner = client;
            reply(client, token, true, QStringLiteral("bound: leansipd stops when this connection closes"), seq);
            emit ownerBound();
        }
        return;
    }
    if (command.startsWith(kPrefix)) {
        const auto handler = m_commands.constFind(command);
        if (handler == m_commands.constEnd()) {
            reply(client, token, false, QStringLiteral("unknown command %1").arg(command), seq);
            return;
        }
        const Result result = (*handler)(params);
        reply(client, token, result.ok, result.data, seq);
        return;
    }
    if (command == QLatin1String("quit")) {
        reply(client, token, false,
              QStringLiteral("refused: quit would stop baresip for every client; use leansipd_shutdown"), seq);
        return;
    }
    if (!m_backendUp) {
        reply(client, token, false, QStringLiteral("baresip is not connected"), seq);
        return;
    }
    const QString upstreamToken = m_upstream->sendCommand(command, params);
    if (upstreamToken.isEmpty()) {
        reply(client, token, false, QStringLiteral("baresip is not connected"), seq);
        return;
    }
    m_pending.insert(upstreamToken, Pending{client, token, seq});
}

void CtrlProxy::onUpstreamResponse(const BaresipResponse &response)
{
    const auto it = m_pending.constFind(response.token);
    if (it == m_pending.constEnd())
        return;
    const Pending pending = *it;
    m_pending.erase(it);
    if (pending.client)
        reply(pending.client, pending.token, response.ok, response.data, pending.seq);
}

void CtrlProxy::failPending(const QString &why)
{
    const auto pending = m_pending;
    m_pending.clear();
    for (const Pending &p : pending) {
        if (p.client)
            reply(p.client, p.token, false, why, p.seq);
    }
}

void CtrlProxy::publish(QJsonObject event)
{
    event[QStringLiteral("seq")] = ++m_seq;
    event[QStringLiteral("ts")] = QDateTime::currentMSecsSinceEpoch();
    m_journal.push_back(event);
    while (m_journal.size() > static_cast<size_t>(kJournalSize))
        m_journal.pop_front();
    for (const auto &entry : m_clients)
        send(entry.first, event);
}

void CtrlProxy::reply(QLocalSocket *client, const QString &token, bool ok, const QString &data, qint64 seq)
{
    QJsonObject obj;
    obj[QStringLiteral("response")] = true;
    obj[QStringLiteral("ok")] = ok;
    obj[QStringLiteral("data")] = data;
    obj[QStringLiteral("token")] = token;
    obj[QStringLiteral("seq")] = seq;
    send(client, obj);
}

void CtrlProxy::send(QLocalSocket *client, const QJsonObject &obj)
{
    if (client->state() != QLocalSocket::ConnectedState)
        return;
    // A client that stopped reading must not make us buffer events forever.
    if (client->bytesToWrite() > kMaxClientBacklog) {
        qCWarning(lcProxy) << "client is not reading, dropping it";
        client->abort();
        return;
    }
    client->write(netstring::encode(QJsonDocument(obj).toJson(QJsonDocument::Compact)));
}

} // namespace leansip
