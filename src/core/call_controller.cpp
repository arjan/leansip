#include "call_controller.h"

#include "dtmf_tone.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QRegularExpression>

Q_LOGGING_CATEGORY(lcCall, "leansip.call")

namespace leansip {

CallController::CallController(BaresipClient *client, QObject *parent)
    : QObject(parent)
    , m_client(client)
{
    connect(m_client, &BaresipClient::connected, this, &CallController::onConnected);
    connect(m_client, &BaresipClient::eventReceived, this, &CallController::onEvent);
    connect(m_client, &BaresipClient::responseReceived, this, &CallController::onResponse);
    connect(m_client, &BaresipClient::disconnected, this, [this] {
        m_backendAvailable = false;
        resetCall();
        setRegistration(RegistrationState::Unknown, {});
        emit backendLost(QStringLiteral("connection to baresip lost"));
    });
    connect(m_client, &BaresipClient::connectionFailed, this, [this](const QString &err) {
        m_backendAvailable = false;
        emit backendLost(QStringLiteral("cannot reach baresip: %1").arg(err));
    });
}

bool CallController::isValidNumber(const QString &number)
{
    // Digits, DTMF symbols, '+', or a full sip: URI / user@host. No whitespace or shell-ish chars:
    // the string is passed verbatim to baresip's command parser.
    static const QRegularExpression re(QStringLiteral(R"(^(?:sips?:)?[A-Za-z0-9+*#._\-]+(?:@[A-Za-z0-9.\-]+(?::\d+)?)?$)"));
    return re.match(number).hasMatch();
}

QString CallController::failureSound(const QString &closeReason)
{
    // Reasons without a SIP status code ("Connection reset by user") are local hangups: silent.
    static const QRegularExpression code(QStringLiteral(R"(^\s*([1-6]\d\d)\b)"));
    const auto m = code.match(closeReason);
    if (!m.hasMatch())
        return {};
    switch (m.captured(1).toInt()) {
    case 486: // Busy Here
    case 600: // Busy Everywhere
    case 603: // Decline
        return QStringLiteral("busy.wav");
    case 487: // Request Terminated: we cancelled it ourselves
        return {};
    default:
        return dtmf::errorSoundFileName();
    }
}

void CallController::onConnected()
{
    m_backendAvailable = true;
    m_inflight.clear();
    m_sequenceTokens.clear();
    // Registration may already have completed before we connected, so seed the state from
    // `reginfo`; any REGISTER_* events that follow override it.
    m_seedToken = m_client->sendCommand(QStringLiteral("reginfo"));
    emit backendReady();
}

CallController::RegistrationState CallController::parseRegInfo(const QString &data, const QString &aor)
{
    if (aor.isEmpty())
        return RegistrationState::Unknown;
    // Lines look like: "0 - sip:user@host        \e[32mOK \e[;m kamailio (...) Expires 3706s"
    // baresip colours the status, so strip ANSI escapes before tokenising.
    static const QRegularExpression ansi(QStringLiteral("\x1b\\[[0-9;]*[A-Za-z]"));
    static const QRegularExpression ws(QStringLiteral("\\s+"));
    QString plain = data;
    plain.remove(ansi);
    const QStringList lines = plain.split(u'\n');
    for (const QString &line : lines) {
        if (!line.contains(aor))
            continue;
        const QStringList tokens = line.split(ws, Qt::SkipEmptyParts);
        if (tokens.contains(QLatin1String("OK")))
            return RegistrationState::Registered;
        if (tokens.contains(QLatin1String("ERR")) || tokens.contains(QLatin1String("FAIL")))
            return RegistrationState::Failed;
        return RegistrationState::Unknown;
    }
    return RegistrationState::Unknown;
}

void CallController::sendTracked(const QString &command, const QString &params)
{
    const QString token = m_client->sendCommand(command, params);
    if (token.isEmpty()) {
        emit commandFailed(command, QStringLiteral("not connected"));
        return;
    }
    m_inflight.insert(token, command);
}

void CallController::sendSequence(const QString &name, const QList<QPair<QString, QString>> &steps)
{
    QJsonArray array;
    for (const auto &step : steps)
        array.append(QJsonArray{step.first, step.second});
    const QString token = m_client->sendCommand(QStringLiteral("leansipd_seq"),
                                                QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact)));
    if (token.isEmpty()) {
        emit commandFailed(name, QStringLiteral("not connected"));
        return;
    }
    m_inflight.insert(token, name);
    m_sequenceTokens.insert(token);
}

void CallController::sendForCall(const QString &command, const QString &params)
{
    if (m_daemonMode && !m_callId.isEmpty())
        sendSequence(command, {{QStringLiteral("callfind"), m_callId}, {command, params}});
    else
        sendTracked(command, params);
}

bool CallController::isOurCall(const QString &eventCallId) const
{
    // Events without an id (older baresip, tests) and calls whose id we have not learnt yet
    // count as ours.
    return eventCallId.isEmpty() || m_callId.isEmpty() || eventCallId == m_callId;
}

void CallController::setOtherCall(const QString &id, bool active)
{
    if (id.isEmpty())
        return;
    const qsizetype before = m_otherCalls.size();
    if (active)
        m_otherCalls.insert(id);
    else
        m_otherCalls.remove(id);
    if (m_otherCalls.size() != before)
        emit otherCallsChanged(otherCallCount());
}

void CallController::setMuted(bool muted)
{
    if (m_muted == muted)
        return;
    m_muted = muted;
    emit muteChanged(muted);
}

void CallController::resetCall()
{
    setCallState(CallState::Idle);
    if (!m_otherCalls.isEmpty()) {
        m_otherCalls.clear();
        emit otherCallsChanged(0);
    }
}

void CallController::dial(const QString &number)
{
    const QString trimmed = number.trimmed();
    if (!isValidNumber(trimmed)) {
        playErrorTone();
        emit commandFailed(QStringLiteral("dial"), QStringLiteral("invalid number '%1'").arg(trimmed));
        return;
    }
    if (m_callState != CallState::Idle) {
        emit commandFailed(QStringLiteral("dial"), QStringLiteral("a call is already in progress"));
        return;
    }
    if (!m_backendAvailable || m_regState != RegistrationState::Registered) {
        qCInfo(lcCall) << "queueing dial until registered:" << trimmed;
        m_pendingDial = trimmed;
        return;
    }
    m_pendingDial.clear();
    setPeer(trimmed);
    setCallState(CallState::Outgoing);
    m_ownsCall = true;
    // Another client may have selected a different account (uafind); dial from ours.
    if (m_daemonMode && !m_accountAor.isEmpty())
        sendSequence(QStringLiteral("dial"), {{QStringLiteral("uafind"), m_accountAor}, {QStringLiteral("dial"), trimmed}});
    else
        sendTracked(QStringLiteral("dial"), trimmed);
}

void CallController::hangup()
{
    m_pendingDial.clear();
    if (m_callState == CallState::Idle)
        return;
    sendForCall(QStringLiteral("hangup"));
}

void CallController::answer()
{
    if (m_callState != CallState::Incoming)
        return;
    m_ownsCall = true;
    sendForCall(QStringLiteral("accept"));
}

void CallController::sendDtmf(QChar digit)
{
    if (m_callState != CallState::Active)
        return;
    static const QString allowed = QStringLiteral("0123456789*#ABCD");
    if (!allowed.contains(digit))
        return;
    sendForCall(QStringLiteral("sndcode"), QString(digit));
    // Local feedback tone via baresip's own player (debug_cmd `play`, resolved against audio_path).
    if (m_localTones)
        sendTracked(QStringLiteral("play"), dtmf::soundFileName(digit));
}

void CallController::toggleMute()
{
    if (m_callState != CallState::Active)
        return;
    sendForCall(QStringLiteral("mute"));
}

void CallController::onEvent(const BaresipEvent &ev)
{
    qCDebug(lcCall) << "event" << ev.klass << ev.type << ev.callId << ev.param;
    const QString &t = ev.type;
    const bool ours = isOurCall(ev.callId);

    if (t == QLatin1String("REGISTER_OK")) {
        setRegistration(RegistrationState::Registered, ev.param);
        if (!m_pendingDial.isEmpty()) {
            const QString number = m_pendingDial;
            m_pendingDial.clear();
            dial(number);
        }
    } else if (t == QLatin1String("REGISTER_FAIL")) {
        setRegistration(RegistrationState::Failed, ev.param);
    } else if (t == QLatin1String("UNREGISTERING")) {
        setRegistration(RegistrationState::Unregistered, ev.param);
    } else if (t == QLatin1String("CALL_INCOMING")) {
        if (m_callState != CallState::Idle) {
            // One call at a time here; baresip's own busy handling applies, we count it.
            qCInfo(lcCall) << "second incoming call from" << ev.peerUri;
            setOtherCall(ev.callId, true);
            return;
        }
        m_callId = ev.callId;
        setPeer(ev.peerDisplayName.isEmpty() ? ev.peerUri : ev.peerDisplayName);
        setCallState(CallState::Incoming);
    } else if (t == QLatin1String("CALL_RINGING") || t == QLatin1String("CALL_PROGRESS")) {
        if (m_callState == CallState::Idle) {
            // Placed by another client (sipctl): show who it is for.
            m_callId = ev.callId;
            setPeer(ev.peerUri);
            setCallState(CallState::Outgoing);
        } else if (ours) {
            if (m_callId.isEmpty())
                m_callId = ev.callId;
        } else {
            setOtherCall(ev.callId, true);
        }
    } else if (t == QLatin1String("CALL_ESTABLISHED")) {
        if (m_callState == CallState::Idle) {
            m_callId = ev.callId;
            setPeer(ev.peerDisplayName.isEmpty() ? ev.peerUri : ev.peerDisplayName);
        } else if (!ours) {
            setOtherCall(ev.callId, true);
            return;
        }
        if (m_callId.isEmpty())
            m_callId = ev.callId;
        setCallState(CallState::Active);
    } else if (t == QLatin1String("CALL_CLOSED")) {
        if (!ours || m_callState == CallState::Idle) {
            setOtherCall(ev.callId, false);
            return;
        }
        // baresip's menu module stays silent on a rejected call, so signal it ourselves.
        // Only for our own calls: an agent's failed test call should not beep at the user.
        const QString sound = m_callState == CallState::Outgoing && m_ownsCall ? failureSound(ev.param) : QString();
        setCallState(CallState::Idle);
        emit callEnded(ev.param.trimmed());
        if (!sound.isEmpty() && m_localTones && m_client->isConnected())
            sendTracked(QStringLiteral("play"), sound);
    } else if (ev.klass == QLatin1String("leansipd") && t == QLatin1String("BACKEND_DOWN")) {
        m_backendAvailable = false;
        resetCall();
        setRegistration(RegistrationState::Unknown, {});
        emit backendLost(ev.param.isEmpty() ? QStringLiteral("baresip went away") : ev.param);
    } else if (ev.klass == QLatin1String("leansipd") && t == QLatin1String("BACKEND_UP")) {
        onConnected(); // a fresh baresip behind the same connection: reseed registration
    } else if (t == QLatin1String("EXIT") || t == QLatin1String("SHUTDOWN")) {
        m_backendAvailable = false;
        resetCall();
        emit backendLost(QStringLiteral("baresip is shutting down"));
    }
}

void CallController::onResponse(const BaresipResponse &raw)
{
    if (!m_seedToken.isEmpty() && raw.token == m_seedToken) {
        m_seedToken.clear();
        qCDebug(lcCall) << "reginfo ok=" << raw.ok << "data=" << raw.data;
        if (!raw.ok) {
            qCInfo(lcCall) << "reginfo unavailable, waiting for register events:" << raw.data;
            return;
        }
        // Only ever upgrade from Unknown: a REGISTER_* event that arrived first is fresher.
        if (m_regState == RegistrationState::Unknown) {
            const auto seeded = parseRegInfo(raw.data, m_accountAor);
            if (seeded != RegistrationState::Unknown) {
                setRegistration(seeded, QStringLiteral("from reginfo"));
                if (seeded == RegistrationState::Registered && !m_pendingDial.isEmpty()) {
                    const QString number = m_pendingDial;
                    m_pendingDial.clear();
                    dial(number);
                }
            }
        }
        return;
    }

    const QString command = m_inflight.take(raw.token);
    if (command.isEmpty())
        return;
    // A leansipd_seq reply carries one {command, ok, data} per step; the last one that ran is
    // the answer to the command, or the reason it never ran.
    BaresipResponse resp = raw;
    if (m_sequenceTokens.remove(raw.token)) {
        const QJsonArray steps = QJsonDocument::fromJson(raw.data.toUtf8()).array();
        if (!steps.isEmpty())
            resp.data = steps.last().toObject().value(QStringLiteral("data")).toString();
    }
    // baresip terminates reply text with '\n'; a wrapped status label would show that as a blank line.
    const QString detail = resp.data.trimmed();

    if (resp.ok) {
        if (command == QLatin1String("dial") && m_callId.isEmpty()) {
            static const QRegularExpression idRe(QStringLiteral(R"(call id:\s*(\S+))"));
            if (const auto m = idRe.match(detail); m.hasMatch())
                m_callId = m.captured(1);
        } else if (command == QLatin1String("mute")) {
            // baresip says "call muted" / "call un-muted"; toggle when it says neither.
            const QString lower = detail.toLower();
            if (lower.contains(QLatin1String("un-mute")) || lower.contains(QLatin1String("unmute")))
                setMuted(false);
            else if (lower.contains(QLatin1String("mute")))
                setMuted(true);
            else
                setMuted(!m_muted);
        }
        return;
    }
    qCWarning(lcCall) << "command" << command << "failed:" << detail;
    if (command == QLatin1String("play"))
        return; // a missing tone file is cosmetic; don't bother the user
    if (command == QLatin1String("hangup") && m_callState != CallState::Idle) {
        // baresip has no call to hang up, so whatever we thought was in progress is gone
        // (e.g. a CALL_CLOSED we never saw). Resync rather than leave the UI stuck.
        qCInfo(lcCall) << "hangup rejected while" << m_callState << "- resetting to idle";
        setCallState(CallState::Idle);
        emit callEnded(detail.isEmpty() ? QStringLiteral("no active call") : detail);
        return;
    }
    if (command == QLatin1String("dial") && m_callState == CallState::Outgoing) {
        playErrorTone();
        setCallState(CallState::Idle);
        emit callEnded(detail.isEmpty() ? QStringLiteral("dial failed") : detail);
    }
    emit commandFailed(command, detail);
}

void CallController::playErrorTone()
{
    if (m_localTones && m_client->isConnected())
        sendTracked(QStringLiteral("play"), dtmf::errorSoundFileName());
}

void CallController::setCallState(CallState state)
{
    if (m_callState == state)
        return;
    m_callState = state;
    if (state == CallState::Idle) {
        m_ownsCall = false;
        m_callId.clear();
        setMuted(false);
    }
    emit callStateChanged(state);
}

void CallController::setRegistration(RegistrationState state, const QString &detail)
{
    if (m_regState == state && m_regDetail == detail)
        return;
    m_regState = state;
    m_regDetail = detail;
    qCInfo(lcCall) << "registration:" << state << detail;
    emit registrationStateChanged(state, detail);
}

void CallController::setPeer(const QString &peer)
{
    if (m_peer == peer)
        return;
    m_peer = peer;
    emit peerChanged(peer);
}

} // namespace leansip
