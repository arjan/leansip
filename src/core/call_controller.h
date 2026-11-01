#pragma once

#include "baresip_client.h"

#include <QObject>
#include <QSet>
#include <QString>

namespace leansip {

// Tracks registration and one call on top of BaresipClient. Through leansipd it also sees calls
// other clients place or answer, and the daemon's BACKEND_DOWN/BACKEND_UP events.
//
// The tracked call is identified by baresip's call id. Events about other calls only change
// otherCallCount(). In daemon mode, commands for the call (hangup, accept, DTMF, mute) select
// it by id first, inside one leansipd_seq, because baresip's "current call" is shared with
// every other client; dial selects our account the same way.
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

    // Connected to leansipd rather than directly to baresip: use leansipd_seq to target the
    // call and account.
    void setDaemonMode(bool enabled) { m_daemonMode = enabled; }

    // Lenient parse of baresip's `reginfo` text. Returns Unknown when the account's line
    // is absent or the format is not recognised; events will then settle the state.
    static RegistrationState parseRegInfo(const QString &data, const QString &aor);

    CallState callState() const { return m_callState; }
    RegistrationState registrationState() const { return m_regState; }
    QString registrationDetail() const { return m_regDetail; }
    QString peer() const { return m_peer; }
    bool backendAvailable() const { return m_backendAvailable; }
    bool hasPendingDial() const { return !m_pendingDial.isEmpty(); }
    // True when the current call was dialled or answered through this controller, as opposed
    // to one another leansipd client (leansipctl, an agent) is handling.
    bool ownsCall() const { return m_ownsCall; }
    QString callId() const { return m_callId; }
    bool isMuted() const { return m_muted; }
    // Calls in progress besides the tracked one (another client's, or a second incoming call).
    int otherCallCount() const { return static_cast<int>(m_otherCalls.size()); }

    static bool isValidNumber(const QString &number);

    // Sound to play when an outgoing call closes before being answered, from baresip's close
    // reason ("403 Forbidden", "486 Busy Here", ...). Empty when nothing should play.
    static QString failureSound(const QString &closeReason);

public slots:
    // Dials now when registered and idle; otherwise queues one number until registration succeeds.
    void dial(const QString &number);
    void hangup();
    void answer();
    void sendDtmf(QChar digit);
    // Toggles the microphone of the established call.
    void toggleMute();

signals:
    void callStateChanged(leansip::CallController::CallState state);
    void registrationStateChanged(leansip::CallController::RegistrationState state, const QString &detail);
    void peerChanged(const QString &peer);
    void callEnded(const QString &reason);
    void commandFailed(const QString &command, const QString &detail);
    void backendLost(const QString &reason);
    void backendReady();
    void muteChanged(bool muted);
    void otherCallsChanged(int count);

private:
    void onConnected();
    void onEvent(const BaresipEvent &event);
    void onResponse(const BaresipResponse &response);
    void setCallState(CallState state);
    void setRegistration(RegistrationState state, const QString &detail);
    void setPeer(const QString &peer);
    void sendTracked(const QString &command, const QString &params = {});
    // `command` for the tracked call: preceded by callfind in daemon mode.
    void sendForCall(const QString &command, const QString &params = {});
    void sendSequence(const QString &name, const QList<QPair<QString, QString>> &steps);
    bool isOurCall(const QString &eventCallId) const;
    void setOtherCall(const QString &id, bool active);
    void setMuted(bool muted);
    void resetCall();
    // "Invalid number" tone for failures baresip itself never signals (local rejection, dial error).
    void playErrorTone();

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
    bool m_ownsCall = false;
    bool m_daemonMode = false;
    bool m_muted = false;
    QString m_callId;
    QSet<QString> m_otherCalls;
    QSet<QString> m_sequenceTokens; // replies whose data is leansipd_seq's JSON array
    QHash<QString, QString> m_inflight; // token -> command name
};

} // namespace leansip
