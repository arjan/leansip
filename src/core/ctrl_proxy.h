#pragma once

#include "baresip_client.h"
#include "netstring.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QObject>
#include <QPointer>

#include <deque>
#include <functional>
#include <map>

namespace leansip {

// Shares one baresip among any number of clients (the dialer, sipctl, an agent).
//
// Clients speak baresip's own ctrl_tcp protocol (netstring-framed JSON) over a Unix socket that
// only this user can open. Commands are forwarded upstream with a fresh token, and the response
// is routed back to the client that asked, under its own token. Every baresip event is
// broadcast to every client. Each event and response carries "seq", a running event counter, and
// events also carry "ts" (ms since epoch); events are journalled so a client can ask for what it
// missed ("leansipd_events <since-seq>").
//
// Commands starting with "leansipd_" are handled locally; `quit` is refused, because it would
// stop baresip for every other client.
//
// Owner binding: when allowed (a daemon the dialer started), the first client to send
// `leansipd_bind` becomes the owner, and ownerGone() fires when that connection closes, also
// when the owner crashes. Other clients cannot bind.
class CtrlProxy : public QObject {
    Q_OBJECT
public:
    struct Result {
        bool ok = false;
        QString data;
    };
    using Handler = std::function<Result(const QString &params)>;

    explicit CtrlProxy(BaresipClient *upstream, QObject *parent = nullptr);
    ~CtrlProxy() override;

    // `path` is an absolute socket path; its directory should already be private (0700).
    bool listen(const QString &path, QString *error);
    void close();

    void addCommand(const QString &name, Handler handler);

    // Tells clients that baresip went away or came back ("leansipd" BACKEND_DOWN/BACKEND_UP
    // events). Upstream connect/disconnect is tracked automatically; call this to give a reason.
    void setBackendUp(bool up, const QString &reason = {});
    bool backendUp() const { return m_backendUp; }

    void setOwnerBindingAllowed(bool allowed) { m_bindingAllowed = allowed; }
    bool hasOwner() const { return !m_owner.isNull(); }

    int clientCount() const { return static_cast<int>(m_clients.size()); }
    qint64 lastSeq() const { return m_seq; }
    // Journalled events with seq > `since`, oldest first.
    QJsonArray eventsSince(qint64 since) const;

    static constexpr int kJournalSize = 2000;
    static constexpr int kMaxClients = 32;
    static constexpr qint64 kMaxClientBacklog = 4 << 20;

signals:
    void clientCountChanged(int count);
    void ownerBound();
    void ownerGone();

private:
    struct Pending {
        QPointer<QLocalSocket> client;
        QString token;
        qint64 seq = 0;
    };

    void onNewConnection();
    void onClientData(QLocalSocket *client);
    void onClientGone(QLocalSocket *client);
    void handleRequest(QLocalSocket *client, const QJsonObject &request);
    void reply(QLocalSocket *client, const QString &token, bool ok, const QString &data, qint64 seq);
    void onUpstreamResponse(const BaresipResponse &response);
    void publish(QJsonObject event);
    void failPending(const QString &why);
    static void send(QLocalSocket *client, const QJsonObject &obj);

    BaresipClient *m_upstream;
    QLocalServer m_server;
    std::map<QLocalSocket *, netstring::Decoder> m_clients;
    QHash<QString, Pending> m_pending; // upstream token -> origin
    QHash<QString, Handler> m_commands;
    std::deque<QJsonObject> m_journal;
    qint64 m_seq = 0;
    bool m_backendUp = false;
    bool m_bindingAllowed = false;
    QPointer<QLocalSocket> m_owner;
};

} // namespace leansip
