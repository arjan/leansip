#pragma once

#include "baresip_client.h"

#include <QObject>
#include <QString>

namespace leansip {

// Tracks registration and a single call on top of BaresipClient.
// UI-agnostic: emits signals, never touches widgets.
class CallController : public QObject {
    Q_OBJECT
public:
    enum class CallState { Idle, Outgoing, Incoming, Active };
    Q_ENUM(CallState)
    enum class RegistrationState { Unknown, Registered, Failed, Unregistered };
    Q_ENUM(RegistrationState)

    explicit CallController(BaresipClient *client, QObject *parent = nullptr);

    // AOR of the account we track (used to pick our line out of `reginfo` output).
    void setAccountAor(const QString &aor) { m_accountAor = aor; }

    // Ask baresip to play a local DTMF feedback tone for each digit sent (default on).
    void setLocalTones(bool enabled) { m_localTones = enabled; }

    // Lenient parse of baresip's `reginfo` text. Returns Unknown when the account's line
    // is absent or the format is not recognised; events will then settle the state.
    static RegistrationState parseRegInfo(const QString &data, const QString &aor);

    CallState callState() const { return m_callState; }
    RegistrationState registrationState() const { return m_regState; }
    QString registrationDetail() const { return m_regDetail; }
    QString peer() const { return m_peer; }
    bool backendAvailable() const { return m_backendAvailable; }
    bool hasPendingDial() const { return !m_pendingDial.isEmpty(); }

    static bool isValidNumber(const QString &number);

public slots:
    // Dials now when registered and idle; otherwise queues one number until registration succeeds.
    void dial(const QString &number);
    void hangup();
    void answer();
    void sendDtmf(QChar digit);

signals:
    void callStateChanged(leansip::CallController::CallState state);
    void registrationStateChanged(leansip::CallController::RegistrationState state, const QString &detail);
    void peerChanged(const QString &peer);
    void callEnded(const QString &reason);
    void commandFailed(const QString &command, const QString &detail);
    void backendLost(const QString &reason);
    void backendReady();

private:
    void onConnected();
    void onEvent(const BaresipEvent &event);
    void onResponse(const BaresipResponse &response);
    void setCallState(CallState state);
    void setRegistration(RegistrationState state, const QString &detail);
    void setPeer(const QString &peer);
    void sendTracked(const QString &command, const QString &params = {});

    BaresipClient *m_client;
    CallState m_callState = CallState::Idle;
    RegistrationState m_regState = RegistrationState::Unknown;
    QString m_regDetail;
    QString m_peer;
    QString m_pendingDial;
    QString m_accountAor;
    QString m_seedToken; // in-flight `reginfo` issued on connect; failures are not user-facing
    bool m_backendAvailable = false;
    bool m_localTones = true;
    QHash<QString, QString> m_inflight; // token -> command name
};

} // namespace leansip
