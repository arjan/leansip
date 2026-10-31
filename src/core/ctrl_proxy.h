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
// stop baresip for every other client. Built in:
//   leansipd_events <since>   journalled events after <since>, as a JSON array
//   leansipd_seq <json>       [["callfind","<id>"],["hangup",""]]: run the commands back to back,
//                             with no other client's command in between, stopping at the first
//                             failure; replies with a JSON array of {command, ok, data}
//   leansipd_hello ui         marks the connection as the dialer (exempt from the command filter)
//   leansipd_bind             see below
//
// baresip has one "current call" and one "current account" for all clients, which is why
// targeting a call (callfind) or an account (uafind) must be part of the same sequence as the
// command it is for.
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
    using Reply = std::function<void(bool ok, const QString &data)>;
    using AsyncHandler = std::function<void(const QString &params, Reply reply)>;

    struct Step {
        QString command;
        QString params;
    };
    struct StepResult {
        QString command;
        bool ok = false;
        QString data;
    };
    using SequenceDone = std::function<void(const QList<StepResult> &results)>;

    // Refusal reason for a command from a non-dialer client, or empty to let it through.
    using CommandFilter = std::function<QString(const QString &command, const QString &params)>;
    // Sees every upstream reply to a client's command (also sequence steps).
    using ResponseObserver = std::function<void(bool fromUi, const QString &command, const QString &params,
                                                bool ok, const QString &data)>;

    explicit CtrlProxy(BaresipClient *upstream, QObject *parent = nullptr);
    ~CtrlProxy() override;

    // `path` is an absolute socket path; its directory should already be private (0700).
    bool listen(const QString &path, QString *error);
    void close();

    void addCommand(const QString &name, Handler handler);
    void addAsyncCommand(const QString &name, AsyncHandler handler);
    void setCommandFilter(CommandFilter filter) { m_filter = std::move(filter); }
    void setResponseObserver(ResponseObserver observer) { m_observer = std::move(observer); }

    // Runs `steps` upstream back to back; client commands arriving meanwhile wait. `done` gets
    // one result per step that ran (fewer when one failed).
    void runSequence(QList<Step> steps, SequenceDone done);

    // Journals and broadcasts an event of the daemon's own (seq and ts are added).
    void publishEvent(const QJsonObject &event);

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
    struct ClientState {
        netstring::Decoder decoder;
        bool ui = false;
    };
    struct Pending {
        QPointer<QLocalSocket> client;
        QString token;
        qint64 seq = 0;
        QString command;
        QString params;
        bool fromUi = false;
    };
    // A client request, or a sequence (isSequence) of steps with a completion callback.
    struct Job {
        QPointer<QLocalSocket> client;
        QJsonObject request;
        QList<Step> steps;
        SequenceDone done;
        bool isSequence = false;
    };

    void onNewConnection();
    void onClientData(QLocalSocket *client);
    void onClientGone(QLocalSocket *client);
    void enqueue(Job job);
    void pump();
    void handleRequest(QLocalSocket *client, const QJsonObject &request);
    void startSequence(Job job);
    void sendNextStep();
    void finishSequence();
    void reply(QLocalSocket *client, const QString &token, bool ok, const QString &data, qint64 seq);
    void onUpstreamResponse(const BaresipResponse &response);
    void publish(QJsonObject event);
    void failPending(const QString &why);
    bool isUi(QLocalSocket *client) const;
    static void send(QLocalSocket *client, const QJsonObject &obj);

    BaresipClient *m_upstream;
    QLocalServer m_server;
    std::map<QLocalSocket *, ClientState> m_clients;
    QHash<QString, Pending> m_pending; // upstream token -> origin
    QHash<QString, Handler> m_commands;
    QHash<QString, AsyncHandler> m_asyncCommands;
    CommandFilter m_filter;
    ResponseObserver m_observer;
    std::deque<Job> m_jobs;
    bool m_inSequence = false; // the running sequence:
    Job m_sequence;
    QList<StepResult> m_sequenceResults;
    QString m_sequenceToken;
    std::deque<QJsonObject> m_journal;
    qint64 m_seq = 0;
    bool m_backendUp = false;
    bool m_bindingAllowed = false;
    QPointer<QLocalSocket> m_owner;
};

} // namespace leansip
