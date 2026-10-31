#include "wav.h"

#include <QFile>
#include <QSaveFile>
#include <QtEndian>

#include <algorithm>
#include <cmath>

namespace leansip::wav {

namespace {

bool fail(QString *error, const QString &why)
{
    if (error)
        *error = why;
    return false;
}

quint32 le32(const char *p)
{
    return qFromLittleEndian<quint32>(p);
}

quint16 le16(const char *p)
{
    return qFromLittleEndian<quint16>(p);
}

} // namespace

double toDbfs(double linearRms)
{
    if (linearRms <= 0)
        return -120;
    return std::max(-120.0, 20.0 * std::log10(linearRms / 32768.0));
}

std::optional<Header> readHeader(QIODevice &device, qint64 fileSize, QString *error)
{
    const QByteArray riff = device.read(12);
    if (riff.size() < 12 || !riff.startsWith("RIFF") || riff.mid(8, 4) != "WAVE") {
        fail(error, QStringLiteral("not a WAV file"));
        return std::nullopt;
    }
    Header h;
    bool haveFmt = false;
    qint64 pos = 12;
    for (int guard = 0; guard < 64; ++guard) {
        const QByteArray chunk = device.read(8);
        if (chunk.size() < 8)
            break;
        const QByteArray id = chunk.left(4);
        const quint32 size = le32(chunk.constData() + 4);
        pos += 8;
        if (id == "fmt ") {
            const QByteArray fmt = device.read(size);
            if (fmt.size() < 16) {
                fail(error, QStringLiteral("truncated fmt chunk"));
                return std::nullopt;
            }
            h.formatTag = le16(fmt.constData());
            h.channels = le16(fmt.constData() + 2);
            h.sampleRate = static_cast<int>(le32(fmt.constData() + 4));
            h.bitsPerSample = le16(fmt.constData() + 14);
            haveFmt = true;
            pos += size;
            if (size % 2)
                device.read(1), ++pos;
            continue;
        }
        if (id == "data") {
            if (!haveFmt) {
                fail(error, QStringLiteral("data chunk before fmt chunk"));
                return std::nullopt;
            }
            h.dataOffset = pos;
            const qint64 onDisk = std::max<qint64>(0, fileSize - pos);
            // A recording in progress has a stale size (often 0); trust the file then.
            h.dataBytes = (size == 0 || size > onDisk) ? onDisk : size;
            return h;
        }
        if (!device.seek(pos + size + (size % 2)))
            break;
        pos += size + (size % 2);
    }
    fail(error, QStringLiteral("no data chunk"));
    return std::nullopt;
}

std::optional<Pcm> readFile(const QString &path, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        fail(error, QStringLiteral("cannot open %1: %2").arg(path, f.errorString()));
        return std::nullopt;
    }
    const auto h = readHeader(f, f.size(), error);
    if (!h)
        return std::nullopt;
    if (h->formatTag != 1 || h->bitsPerSample != 16 || h->channels < 1 || h->channels > 2 || h->sampleRate <= 0) {
        fail(error, QStringLiteral("%1: only 16-bit PCM mono/stereo WAV is supported (got format %2, %3 bit, %4 ch)")
                        .arg(path)
                        .arg(h->formatTag)
                        .arg(h->bitsPerSample)
                        .arg(h->channels));
        return std::nullopt;
    }
    f.seek(h->dataOffset);
    const QByteArray data = f.read(h->dataBytes - (h->dataBytes % (2 * h->channels)));
    return fromRaw(data, h->sampleRate, h->channels);
}

bool writeFile(const QString &path, const Pcm &pcm, QString *error)
{
    const QByteArray data = toRaw(pcm);
    QByteArray header(44, '\0');
    char *p = header.data();
    const auto put32 = [](char *at, quint32 v) { qToLittleEndian<quint32>(v, at); };
    const auto put16 = [](char *at, quint16 v) { qToLittleEndian<quint16>(v, at); };
    memcpy(p, "RIFF", 4);
    put32(p + 4, static_cast<quint32>(36 + data.size()));
    memcpy(p + 8, "WAVEfmt ", 8);
    put32(p + 16, 16);
    put16(p + 20, 1);
    put16(p + 22, static_cast<quint16>(pcm.channels));
    put32(p + 24, static_cast<quint32>(pcm.sampleRate));
    put32(p + 28, static_cast<quint32>(pcm.sampleRate * pcm.channels * 2));
    put16(p + 32, static_cast<quint16>(pcm.channels * 2));
    put16(p + 34, 16);
    memcpy(p + 36, "data", 4);
    put32(p + 40, static_cast<quint32>(data.size()));

    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return fail(error, QStringLiteral("cannot write %1: %2").arg(path, f.errorString()));
    f.write(header);
    f.write(data);
    if (!f.commit())
        return fail(error, QStringLiteral("cannot write %1: %2").arg(path, f.errorString()));
    return true;
}

Pcm fromRaw(const QByteArray &s16le, int sampleRate, int channels)
{
    Pcm pcm;
    pcm.sampleRate = sampleRate;
    pcm.channels = channels;
    const qsizetype n = s16le.size() / 2;
    pcm.samples.resize(n);
    for (qsizetype i = 0; i < n; ++i)
        pcm.samples[i] = qFromLittleEndian<qint16>(s16le.constData() + 2 * i);
    return pcm;
}

QByteArray toRaw(const Pcm &pcm)
{
    QByteArray out(pcm.samples.size() * 2, '\0');
    for (qsizetype i = 0; i < pcm.samples.size(); ++i)
        qToLittleEndian<qint16>(pcm.samples[i], out.data() + 2 * i);
    return out;
}

Pcm mixToMono(const Pcm &in)
{
    if (in.channels == 1)
        return in;
    Pcm out;
    out.sampleRate = in.sampleRate;
    out.channels = 1;
    const qint64 frames = in.frames();
    out.samples.resize(frames);
    for (qint64 f = 0; f < frames; ++f) {
        int sum = 0;
        for (int c = 0; c < in.channels; ++c)
            sum += in.samples[f * in.channels + c];
        out.samples[f] = static_cast<qint16>(sum / in.channels);
    }
    return out;
}

Pcm convert(const Pcm &in, int sampleRate, int channels)
{
    const Pcm mono = mixToMono(in);
    // Resample the mono signal linearly; speech and test tones do not need better than that.
    Pcm resampled;
    resampled.sampleRate = sampleRate;
    resampled.channels = 1;
    if (mono.sampleRate == sampleRate || mono.samples.isEmpty()) {
        resampled.samples = mono.samples;
    } else {
        const double ratio = static_cast<double>(mono.sampleRate) / sampleRate;
        const qint64 outFrames = static_cast<qint64>(std::floor(static_cast<double>(mono.frames()) / ratio));
        resampled.samples.resize(outFrames);
        for (qint64 i = 0; i < outFrames; ++i) {
            const double src = static_cast<double>(i) * ratio;
            const qint64 i0 = static_cast<qint64>(src);
            const qint64 i1 = std::min<qint64>(i0 + 1, mono.frames() - 1);
            const double t = src - static_cast<double>(i0);
            resampled.samples[i] = static_cast<qint16>(std::lround(mono.samples[i0] * (1 - t) + mono.samples[i1] * t));
        }
    }
    if (channels == 1)
        return resampled;
    Pcm out;
    out.sampleRate = sampleRate;
    out.channels = channels;
    out.samples.resize(resampled.samples.size() * channels);
    for (qsizetype i = 0; i < resampled.samples.size(); ++i) {
        for (int c = 0; c < channels; ++c)
            out.samples[i * channels + c] = resampled.samples[i];
    }
    return out;
}

void appendSilence(Pcm &pcm, double seconds)
{
    const qint64 frames = static_cast<qint64>(seconds * pcm.sampleRate);
    pcm.samples.resize(pcm.samples.size() + frames * pcm.channels, 0);
}

QVector<double> frameLevels(const Pcm &pcm, int frameMs)
{
    QVector<double> levels;
    if (pcm.sampleRate <= 0 || pcm.channels <= 0)
        return levels;
    const qint64 frameLen = std::max<qint64>(1, static_cast<qint64>(pcm.sampleRate) * frameMs / 1000);
    const qint64 frames = pcm.frames();
    for (qint64 start = 0; start + frameLen <= frames; start += frameLen) {
        double sum = 0;
        for (qint64 f = start; f < start + frameLen; ++f) {
            double v = 0;
            for (int c = 0; c < pcm.channels; ++c)
                v += pcm.samples[f * pcm.channels + c];
            v /= pcm.channels;
            sum += v * v;
        }
        levels.append(toDbfs(std::sqrt(sum / static_cast<double>(frameLen))));
    }
    return levels;
}

Levels measure(const Pcm &pcm, double silenceDbfs)
{
    Levels l;
    l.seconds = pcm.seconds();
    if (pcm.samples.isEmpty())
        return l;
    double sum = 0;
    int peak = 0;
    for (qint16 s : pcm.samples) {
        sum += static_cast<double>(s) * s;
        peak = std::max(peak, std::abs(static_cast<int>(s)));
    }
    l.rmsDbfs = toDbfs(std::sqrt(sum / static_cast<double>(pcm.samples.size())));
    l.peakDbfs = toDbfs(peak);
    const QVector<double> frames = frameLevels(pcm);
    if (!frames.isEmpty()) {
        const auto silent = std::count_if(frames.begin(), frames.end(), [&](double d) { return d < silenceDbfs; });
        l.silentFraction = static_cast<double>(silent) / static_cast<double>(frames.size());
    }
    return l;
}

} // namespace leansip::wav
