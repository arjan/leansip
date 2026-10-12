#include "call_controller.h"

#include "dtmf_tone.h"

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
        setCallState(CallState::Idle);
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

void CallController::onConnected()
{
    m_backendAvailable = true;
    m_inflight.clear();
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
    sendTracked(QStringLiteral("dial"), trimmed);
}

void CallController::hangup()
{
    m_pendingDial.clear();
    if (m_callState == CallState::Idle)
        return;
    sendTracked(QStringLiteral("hangup"));
}

void CallController::answer()
{
    if (m_callState != CallState::Incoming)
        return;
    sendTracked(QStringLiteral("accept"));
}

void CallController::sendDtmf(QChar digit)
{
    if (m_callState != CallState::Active)
        return;
    static const QString allowed = QStringLiteral("0123456789*#ABCD");
    if (!allowed.contains(digit))
        return;
    sendTracked(QStringLiteral("sndcode"), QString(digit));
    // Local feedback tone via baresip's own player (debug_cmd `play`, resolved against audio_path).
    if (m_localTones)
        sendTracked(QStringLiteral("play"), dtmf::soundFileName(digit));
}

void CallController::onEvent(const BaresipEvent &ev)
{
    qCDebug(lcCall) << "event" << ev.klass << ev.type << ev.param;
    const QString &t = ev.type;

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
            // Single-call UI: let baresip's own busy handling apply; we just ignore it here.
            qCInfo(lcCall) << "ignoring second incoming call from" << ev.peerUri;
            return;
        }
        setPeer(ev.peerDisplayName.isEmpty() ? ev.peerUri : ev.peerDisplayName);
        setCallState(CallState::Incoming);
    } else if (t == QLatin1String("CALL_RINGING") || t == QLatin1String("CALL_PROGRESS")) {
        if (m_callState == CallState::Idle)
            setCallState(CallState::Outgoing);
    } else if (t == QLatin1String("CALL_ESTABLISHED")) {
        setCallState(CallState::Active);
    } else if (t == QLatin1String("CALL_CLOSED")) {
        if (m_callState != CallState::Idle) {
            setCallState(CallState::Idle);
            emit callEnded(ev.param);
        }
    } else if (t == QLatin1String("EXIT") || t == QLatin1String("SHUTDOWN")) {
        m_backendAvailable = false;
        setCallState(CallState::Idle);
        emit backendLost(QStringLiteral("baresip is shutting down"));
    }
}

void CallController::onResponse(const BaresipResponse &resp)
{
    if (!m_seedToken.isEmpty() && resp.token == m_seedToken) {
        m_seedToken.clear();
        qCDebug(lcCall) << "reginfo ok=" << resp.ok << "data=" << resp.data;
        if (!resp.ok) {
            qCInfo(lcCall) << "reginfo unavailable, waiting for register events:" << resp.data;
            return;
        }
        // Only ever upgrade from Unknown: a REGISTER_* event that arrived first is fresher.
        if (m_regState == RegistrationState::Unknown) {
            const auto seeded = parseRegInfo(resp.data, m_accountAor);
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

    const QString command = m_inflight.take(resp.token);
    if (command.isEmpty())
        return;
    if (resp.ok)
        return;
    qCWarning(lcCall) << "command" << command << "failed:" << resp.data;
    if (command == QLatin1String("play"))
        return; // a missing tone file is cosmetic; don't bother the user
    if (command == QLatin1String("dial") && m_callState == CallState::Outgoing) {
        playErrorTone();
        setCallState(CallState::Idle);
        emit callEnded(resp.data.isEmpty() ? QStringLiteral("dial failed") : resp.data);
    }
    emit commandFailed(command, resp.data);
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
