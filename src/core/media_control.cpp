#include "media_control.h"

#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QRegularExpression>

namespace leansip {

std::optional<StreamFormat> parseAudioDebug(const QString &text)
{
    // " tx:   encode: opus 48000Hz/2ch ptime=20ms S16LE"
    // "       source: ausine,440 S16LE"
    static const QRegularExpression encode(QStringLiteral(R"(encode:\s+(\S+)\s+(\d+)Hz/(\d+)ch)"));
    static const QRegularExpression source(QStringLiteral(R"(^\s*source:\s+(\S+))"),
                                           QRegularExpression::MultilineOption);
    const auto e = encode.match(text);
    if (!e.hasMatch())
        return std::nullopt;
    StreamFormat f;
    f.codec = e.captured(1);
    f.sampleRate = e.captured(2).toInt();
    f.channels = e.captured(3).toInt();
    const auto s = source.match(text);
    if (s.hasMatch())
        f.source = s.captured(1);
    if (f.sampleRate <= 0 || f.channels <= 0)
        return std::nullopt;
    return f;
}

RecordingTracker::RecordingTracker(QString incomingDir, QString recordingsDir)
    : m_incoming(std::move(incomingDir))
    , m_recordings(std::move(recordingsDir))
{
}

std::optional<RecordingTracker::Files> RecordingTracker::onCallEvent(const QString &callId)
{
    if (callId.isEmpty() || m_byCall.contains(callId) || m_incoming.isEmpty())
        return std::nullopt;
    // sndfile names files "dump-<yyyy-mm-dd-hh-mm-ss>-enc" / "-dec" (with or without .wav).
    static const QRegularExpression dumpName(QStringLiteral(R"(^dump-(.+)-(enc|dec)(?:\.wav)?$)"));
    QMap<QString, Files> pairs; // keyed by timestamp, so iteration is oldest first
    const QStringList names = QDir(m_incoming).entryList(QDir::Files, QDir::Time | QDir::Reversed);
    for (const QString &name : names) {
        const auto m = dumpName.match(name);
        if (!m.hasMatch())
            continue;
        Files &f = pairs[m.captured(1)];
        (m.captured(2) == QLatin1String("enc") ? f.sent : f.received) = QDir(m_incoming).filePath(name);
    }
    // The oldest complete pair belongs to the oldest call still waiting for one: calls start
    // their audio in the order their events arrive.
    for (auto it = pairs.cbegin(); it != pairs.cend(); ++it) {
        if (it->sent.isEmpty() || it->received.isEmpty())
            continue;
        QDir().mkpath(m_recordings);
        Files moved{QDir(m_recordings).filePath(callId + QStringLiteral("-sent.wav")),
                    QDir(m_recordings).filePath(callId + QStringLiteral("-received.wav"))};
        if (!QFile::rename(it->sent, moved.sent) || !QFile::rename(it->received, moved.received))
            return std::nullopt;
        m_byCall.insert(callId, moved);
        return moved;
    }
    return std::nullopt;
}

std::optional<RecordingTracker::Files> RecordingTracker::filesFor(const QString &callId) const
{
    const auto it = m_byCall.constFind(callId);
    if (it == m_byCall.constEnd())
        return std::nullopt;
    return *it;
}

std::optional<RecordingTracker::Files> RecordingTracker::forget(const QString &callId)
{
    const auto it = m_byCall.find(callId);
    if (it == m_byCall.end())
        return std::nullopt;
    Files files = *it;
    m_byCall.erase(it);
    return files;
}

QJsonObject RecordingTracker::toJson() const
{
    QJsonObject out;
    for (auto it = m_byCall.cbegin(); it != m_byCall.cend(); ++it)
        out[it.key()] = QJsonObject{{QStringLiteral("sent"), it->sent}, {QStringLiteral("received"), it->received}};
    return out;
}

} // namespace leansip
