#include "call_analysis.h"

#include <cmath>

namespace leansip::analysis {

namespace {

const QString kId = QStringLiteral("id");
const QString kType = QStringLiteral("type");
const QString kTs = QStringLiteral("ts");

double round1(double v)
{
    return std::round(v * 10.0) / 10.0;
}

} // namespace

QString lastCallId(const QJsonArray &events)
{
    for (qsizetype i = events.size() - 1; i >= 0; --i) {
        const QJsonObject ev = events.at(i).toObject();
        if (ev.value(QStringLiteral("class")).toString() == QLatin1String("call") && !ev.value(kId).toString().isEmpty())
            return ev.value(kId).toString();
    }
    return {};
}

QJsonObject levelsToJson(const wav::Levels &l)
{
    return {{QStringLiteral("seconds"), round1(l.seconds)},
            {QStringLiteral("rmsDbfs"), round1(l.rmsDbfs)},
            {QStringLiteral("peakDbfs"), round1(l.peakDbfs)},
            {QStringLiteral("silentPercent"), round1(l.silentFraction * 100.0)}};
}

QJsonObject analyzeCall(const QJsonArray &events, const QString &callId, const std::optional<wav::Levels> &sent,
                        const std::optional<wav::Levels> &received)
{
    QJsonObject out{{kId, callId}};
    qint64 start = -1, ringing = -1, established = -1, closed = -1;
    QString dtmf;
    QJsonObject lastRtcp;
    double maxRxJitter = 0, maxTxJitter = 0;
    int rtcpReports = 0;
    QJsonArray warnings;

    for (const QJsonValue &v : events) {
        const QJsonObject ev = v.toObject();
        if (ev.value(kId).toString() != callId)
            continue;
        const QString type = ev.value(kType).toString();
        const qint64 ts = ev.value(kTs).toInteger();
        if (start < 0) {
            start = ts;
            out[QStringLiteral("direction")] = ev.value(QStringLiteral("direction"));
            out[QStringLiteral("account")] = ev.value(QStringLiteral("accountaor"));
            out[QStringLiteral("peer")] = ev.value(QStringLiteral("peeruri"));
            out[QStringLiteral("startTs")] = ts;
        }
        if ((type == QLatin1String("CALL_RINGING") || type == QLatin1String("CALL_PROGRESS")) && ringing < 0)
            ringing = ts;
        else if (type == QLatin1String("CALL_ESTABLISHED") && established < 0)
            established = ts;
        else if (type == QLatin1String("CALL_CLOSED")) {
            closed = ts;
            out[QStringLiteral("closeReason")] = ev.value(QStringLiteral("param")).toString().trimmed();
        } else if (type == QLatin1String("CALL_DTMF_START"))
            dtmf += ev.value(QStringLiteral("param")).toString();
        else if (type == QLatin1String("CALL_RTCP") && ev.contains(QStringLiteral("rtcp_stats"))) {
            lastRtcp = ev.value(QStringLiteral("rtcp_stats")).toObject();
            ++rtcpReports;
            maxRxJitter = std::max(maxRxJitter, lastRtcp.value(QStringLiteral("rx")).toObject().value(QStringLiteral("jit")).toDouble());
            maxTxJitter = std::max(maxTxJitter, lastRtcp.value(QStringLiteral("tx")).toObject().value(QStringLiteral("jit")).toDouble());
        } else if (type == QLatin1String("CALL_AUDIO")) {
            out[QStringLiteral("audio")] = QJsonObject{{QStringLiteral("codec"), ev.value(QStringLiteral("codec"))},
                                                       {QStringLiteral("sampleRate"), ev.value(QStringLiteral("srate"))},
                                                       {QStringLiteral("channels"), ev.value(QStringLiteral("channels"))}};
        }
    }
    if (start < 0) {
        out[QStringLiteral("found")] = false;
        return out;
    }
    out[QStringLiteral("found")] = true;
    QJsonObject timing;
    if (ringing >= 0)
        timing[QStringLiteral("ringingAfterMs")] = ringing - start;
    if (established >= 0)
        timing[QStringLiteral("answeredAfterMs")] = established - start;
    if (closed >= 0 && established >= 0)
        timing[QStringLiteral("talkMs")] = closed - established;
    if (closed >= 0)
        timing[QStringLiteral("totalMs")] = closed - start;
    out[QStringLiteral("timing")] = timing;
    out[QStringLiteral("state")] = closed >= 0 ? QStringLiteral("closed")
                                   : established >= 0 ? QStringLiteral("established")
                                                      : QStringLiteral("setup");
    out[QStringLiteral("dtmfReceived")] = dtmf;
    if (rtcpReports > 0) {
        QJsonObject rtcp = lastRtcp;
        rtcp[QStringLiteral("reports")] = rtcpReports;
        rtcp[QStringLiteral("maxRxJitter")] = maxRxJitter;
        rtcp[QStringLiteral("maxTxJitter")] = maxTxJitter;
        out[QStringLiteral("rtcp")] = rtcp;
        if (lastRtcp.value(QStringLiteral("rx")).toObject().value(QStringLiteral("lost")).toInteger() > 0
            || lastRtcp.value(QStringLiteral("tx")).toObject().value(QStringLiteral("lost")).toInteger() > 0)
            warnings.append(QStringLiteral("packet loss reported by RTCP"));
    } else if (established >= 0) {
        warnings.append(QStringLiteral("no RTCP reports"));
    }
    if (established < 0 && closed >= 0)
        warnings.append(QStringLiteral("call was never answered"));

    if (sent || received) {
        QJsonObject audio = out.value(QStringLiteral("levels")).toObject();
        if (sent)
            audio[QStringLiteral("sent")] = levelsToJson(*sent);
        if (received)
            audio[QStringLiteral("received")] = levelsToJson(*received);
        out[QStringLiteral("levels")] = audio;
        const bool sentAudio = sent && sent->silentFraction < 0.98;
        const bool receivedAudio = received && received->silentFraction < 0.98;
        if (established >= 0 && received && !receivedAudio)
            warnings.append(sentAudio ? QStringLiteral("one-way audio: nothing received while sending")
                                      : QStringLiteral("no audio in either direction"));
        else if (established >= 0 && sent && !sentAudio && receivedAudio)
            warnings.append(QStringLiteral("nothing sent: the far end heard silence"));
    }
    out[QStringLiteral("warnings")] = warnings;
    return out;
}

} // namespace leansip::analysis
