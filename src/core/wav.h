#pragma once

#include <QByteArray>
#include <QIODevice>
#include <QList>
#include <QString>
#include <QVector>

#include <optional>

namespace leansip::wav {

// 16-bit PCM audio, samples interleaved by channel.
struct Pcm {
    int sampleRate = 0;
    int channels = 0;
    QVector<qint16> samples;

    qint64 frames() const { return channels > 0 ? samples.size() / channels : 0; }
    double seconds() const { return sampleRate > 0 ? static_cast<double>(frames()) / sampleRate : 0.0; }
};

struct Header {
    int sampleRate = 0;
    int channels = 0;
    int bitsPerSample = 0;
    int formatTag = 0;    // 1 = PCM
    qint64 dataOffset = 0;
    qint64 dataBytes = 0; // from the file size when the header's value is stale (a file being written)
};

// Parses RIFF/WAVE chunks up to "data". `fileSize` lets a still-growing recording (whose
// header sizes are not final yet) report all the data actually on disk.
std::optional<Header> readHeader(QIODevice &device, qint64 fileSize, QString *error);

std::optional<Pcm> readFile(const QString &path, QString *error);
bool writeFile(const QString &path, const Pcm &pcm, QString *error);

Pcm fromRaw(const QByteArray &s16le, int sampleRate, int channels);
QByteArray toRaw(const Pcm &pcm);

// Channel mapping (mono <-> stereo by duplicating/averaging) plus linear resampling.
Pcm convert(const Pcm &in, int sampleRate, int channels);
Pcm mixToMono(const Pcm &in);
void appendSilence(Pcm &pcm, double seconds);

// Loudness per frame in dBFS (mono mix), the basis for level and speech detection.
QVector<double> frameLevels(const Pcm &pcm, int frameMs = 20);

struct Levels {
    double seconds = 0;
    double rmsDbfs = -120;
    double peakDbfs = -120;
    double silentFraction = 1; // share of 20 ms frames below the silence threshold
};
Levels measure(const Pcm &pcm, double silenceDbfs = -50);

double toDbfs(double linearRms);

} // namespace leansip::wav
