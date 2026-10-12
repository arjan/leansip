#include "dtmf_tone.h"

#include <QDir>
#include <QFile>
#include <QtEndian>

#include <cmath>
#include <numbers>

namespace leansip::dtmf {

namespace {

constexpr char16_t kKeys[] = u"0123456789*#ABCD";

template <typename T>
void appendLe(QByteArray &out, T value)
{
    T le = qToLittleEndian(value);
    out.append(reinterpret_cast<const char *>(&le), sizeof le);
}

} // namespace

std::optional<std::pair<int, int>> frequencies(QChar key)
{
    // Standard keypad: rows 697/770/852/941 Hz, columns 1209/1336/1477/1633 Hz.
    static constexpr int rows[] = {697, 770, 852, 941};
    static constexpr int cols[] = {1209, 1336, 1477, 1633};
    static constexpr char16_t layout[4][4] = {
        {u'1', u'2', u'3', u'A'},
        {u'4', u'5', u'6', u'B'},
        {u'7', u'8', u'9', u'C'},
        {u'*', u'0', u'#', u'D'},
    };
    const char16_t k = key.toUpper().unicode();
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            if (layout[r][c] == k)
                return std::make_pair(rows[r], cols[c]);
        }
    }
    return std::nullopt;
}

QByteArray samples(QChar key, int durationMs, int sampleRate)
{
    const auto freqs = frequencies(key);
    if (!freqs || durationMs <= 0 || sampleRate <= 0)
        return {};

    const int count = sampleRate * durationMs / 1000;
    const int ramp = std::min(count / 4, sampleRate * 5 / 1000); // 5 ms fade, shorter for tiny bursts
    constexpr double amplitude = 0.35 * 32767.0;                   // each tone at -9 dBFS: no clipping when summed

    QByteArray out;
    out.reserve(count * 2);
    const double twoPi = 2.0 * std::numbers::pi;
    for (int i = 0; i < count; ++i) {
        const double t = static_cast<double>(i) / sampleRate;
        double env = 1.0;
        if (ramp > 0 && i < ramp)
            env = static_cast<double>(i) / ramp;
        else if (ramp > 0 && i >= count - ramp)
            env = static_cast<double>(count - 1 - i) / ramp;
        const double v = env * amplitude * (std::sin(twoPi * freqs->first * t) + std::sin(twoPi * freqs->second * t)) / 2.0;
        appendLe<qint16>(out, static_cast<qint16>(std::lround(v)));
    }
    return out;
}

QByteArray sitSamples(int sampleRate)
{
    if (sampleRate <= 0)
        return {};
    constexpr int segmentMs = 330;
    constexpr int tailMs = 300;
    constexpr int tones[] = {950, 1400, 1800};
    constexpr double amplitude = 0.5 * 32767.0;
    const int perSegment = sampleRate * segmentMs / 1000;
    const int ramp = sampleRate * 5 / 1000;
    const double twoPi = 2.0 * std::numbers::pi;

    QByteArray out;
    out.reserve((3 * perSegment + sampleRate * tailMs / 1000) * 2);
    for (const int hz : tones) {
        for (int i = 0; i < perSegment; ++i) {
            double env = 1.0;
            if (i < ramp)
                env = static_cast<double>(i) / ramp;
            else if (i >= perSegment - ramp)
                env = static_cast<double>(perSegment - 1 - i) / ramp;
            const double v = env * amplitude * std::sin(twoPi * hz * i / sampleRate);
            appendLe<qint16>(out, static_cast<qint16>(std::lround(v)));
        }
    }
    for (int i = 0; i < sampleRate * tailMs / 1000; ++i)
        appendLe<qint16>(out, 0);
    return out;
}

QByteArray wav(const QByteArray &pcm, int sampleRate)
{
    constexpr quint16 channels = 1;
    constexpr quint16 bitsPerSample = 16;
    const quint32 byteRate = static_cast<quint32>(sampleRate) * channels * bitsPerSample / 8;

    QByteArray out;
    out.append("RIFF");
    appendLe<quint32>(out, static_cast<quint32>(36 + pcm.size()));
    out.append("WAVE");
    out.append("fmt ");
    appendLe<quint32>(out, 16);                       // PCM fmt chunk size
    appendLe<quint16>(out, 1);                        // PCM
    appendLe<quint16>(out, channels);
    appendLe<quint32>(out, static_cast<quint32>(sampleRate));
    appendLe<quint32>(out, byteRate);
    appendLe<quint16>(out, channels * bitsPerSample / 8); // block align
    appendLe<quint16>(out, bitsPerSample);
    out.append("data");
    appendLe<quint32>(out, static_cast<quint32>(pcm.size()));
    out.append(pcm);
    return out;
}

QString soundFileName(QChar key)
{
    if (!frequencies(key))
        return {};
    if (key == u'*')
        return QStringLiteral("dtmf-star.wav");
    if (key == u'#')
        return QStringLiteral("dtmf-hash.wav");
    return QStringLiteral("dtmf-%1.wav").arg(key.toUpper());
}

namespace {

// Replaces the target outright: writing through a symlink to baresip's stock file would fail.
bool writeSound(const QString &path, const QByteArray &data)
{
    if (QFile::exists(path) || QFileInfo(path).isSymLink())
        QFile::remove(path);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return file.write(data) == data.size();
}

} // namespace

bool installSoundBank(const QString &dir, int durationMs, int sampleRate)
{
    if (!QDir().mkpath(dir))
        return false;
    const QDir target(dir);
    for (const char16_t k : kKeys) {
        if (k == u'\0')
            break;
        const QChar key(k);
        if (!writeSound(target.filePath(soundFileName(key)), wav(samples(key, durationMs, sampleRate), sampleRate)))
            return false;
    }
    const QByteArray sit = wav(sitSamples(sampleRate), sampleRate);
    for (const char *name : {"error.wav", "notfound.wav"}) {
        if (!writeSound(target.filePath(QLatin1String(name)), sit))
            return false;
    }
    return true;
}

} // namespace leansip::dtmf
