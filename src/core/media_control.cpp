#include "media_control.h"

#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QRegularExpression>
#include <QTextStream>

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

CallPolicy CallPolicy::load(const QString &allowFile)
{
    CallPolicy p;
    p.m_file = allowFile;
    QFile f(allowFile);
    QStringList patterns;
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&f);
        while (!in.atEnd()) {
            const QString line = in.readLine().section(u'#', 0, 0).trimmed();
            if (!line.isEmpty())
                patterns.append(line);
        }
    }
    p.setPatterns(patterns);
    return p;
}

void CallPolicy::setPatterns(const QStringList &patterns)
{
    m_patterns = patterns;
}

namespace {

QString stripScheme(QString target)
{
    target = target.trimmed();
    for (const char *scheme : {"sips:", "sip:", "tel:"}) {
        if (target.startsWith(QLatin1String(scheme), Qt::CaseInsensitive))
            return target.mid(static_cast<qsizetype>(qstrlen(scheme)));
    }
    return target;
}

QString hostOf(const QString &bare)
{
    const qsizetype at = bare.indexOf(u'@');
    if (at < 0)
        return {};
    QString host = bare.mid(at + 1).section(u';', 0, 0).section(u'>', 0, 0);
    if (host.startsWith(u'[')) // [::1]:5060
        return host.mid(1).section(u']', 0, 0);
    if (host.count(u':') == 1)
        host = host.section(u':', 0, 0);
    return host;
}

bool globMatch(const QString &pattern, const QString &text)
{
    const QRegularExpression re(QRegularExpression::anchoredPattern(
        QRegularExpression::escape(pattern).replace(QStringLiteral("\\*"), QStringLiteral(".*"))),
        QRegularExpression::CaseInsensitiveOption);
    return re.match(text).hasMatch();
}

} // namespace

bool CallPolicy::isLoopbackTarget(const QString &target)
{
    const QString host = hostOf(stripScheme(target)).toLower();
    return host == QLatin1String("localhost") || host == QLatin1String("::1") || host.startsWith(QLatin1String("127."));
}

QString CallPolicy::check(const QString &target) const
{
    const QString bare = stripScheme(target);
    if (bare.isEmpty())
        return QStringLiteral("no destination");
    if (isLoopbackTarget(target))
        return {};
    const QString user = bare.section(u'@', 0, 0);
    for (const QString &p : m_patterns) {
        if (globMatch(p, user) || globMatch(p, bare))
            return {};
    }
    return QStringLiteral("destination '%1' is not allowed for sipctl: add a pattern to %2 "
                          "(loopback destinations are always allowed)")
        .arg(bare, m_file.isEmpty() ? QStringLiteral("sipctl-allow") : m_file);
}

std::optional<QString> CallPolicy::dialTarget(const QString &command, const QString &params)
{
    const QString c = command.trimmed().toLower();
    if (c == QLatin1String("dial") || c == QLatin1String("dialdir") || c == QLatin1String("transfer"))
        return params.trimmed().section(u' ', 0, 0);
    if (c == QLatin1String("dialcontact"))
        return QStringLiteral("(current contact)");
    return std::nullopt;
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

QJsonObject RecordingTracker::toJson() const
{
    QJsonObject out;
    for (auto it = m_byCall.cbegin(); it != m_byCall.cend(); ++it)
        out[it.key()] = QJsonObject{{QStringLiteral("sent"), it->sent}, {QStringLiteral("received"), it->received}};
    return out;
}

} // namespace leansip
