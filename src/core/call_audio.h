#pragma once

#include "wav.h"

#include <QElapsedTimer>
#include <QFile>
#include <QObject>
#include <QTimer>
#include <QVector>

#include <memory>
#include <optional>

namespace leansip {

// Reads a 16-bit PCM WAV file while another process (baresip's sndfile) is still appending to
// it. Waits until the header is complete; afterwards each poll() returns the samples added since
// the previous one.
class WavTail {
public:
    // With `releaseRead`, file space already read is given back to the file system (a hole is
    // punched), so a long call in a tmpfs file does not keep growing in memory. Only for files
    // nobody keeps: the released audio reads back as silence.
    WavTail(const QString &path, bool releaseRead);

    // New interleaved samples, possibly none. When more than `maxBacklogMs` is waiting (the
    // reader started late or stalled), only the newest `maxBacklogMs` is returned. nullopt when
    // the file is unusable, with the reason in `error`.
    std::optional<QVector<qint16>> poll(int maxBacklogMs, QString *error);

    int sampleRate() const { return m_header ? m_header->sampleRate : 0; }
    int channels() const { return m_header ? m_header->channels : 0; }
    // Bytes from the start of the file that were read and released (always page aligned).
    qint64 releasedUpTo() const { return m_releasedTo; }

    static constexpr qint64 kReleaseChunk = 64 * 1024;

private:
    QFile m_file;
    bool m_release;
    std::optional<wav::Header> m_header;
    qint64 m_offset = 0;
    qint64 m_releasedTo = 0;
};

// Recent audio activity of a 16-bit PCM stream for a scrolling display: one point per
// kBucketMs, the last kLength of them, oldest first. A point is the slice's RMS level (channels
// mixed) mapped from kFloorDb..kCeilDb dBFS onto 0..1 and smoothed: it rises with a time
// constant of kAttackMs and falls with kReleaseMs, so it follows speech quickly without
// flickering.
class Waveform {
public:
    static constexpr int kBucketMs = 5;
    static constexpr int kLength = 400; // 2 s
    static constexpr double kFloorDb = -55.0;
    static constexpr double kCeilDb = -5.0;
    static constexpr double kAttackMs = 10.0;
    static constexpr double kReleaseMs = 70.0;

    Waveform();
    // Clears the history when the format changes. Nothing is taken in until this is called.
    void setFormat(int sampleRate, int channels);
    // Interleaved samples; `count` is the number of samples, not frames.
    void push(const qint16 *samples, qsizetype count);
    // `ms` of silence: keeps the display rolling while no audio arrives.
    void pushSilence(int ms);
    const QVector<float> &points() const { return m_points; }
    void reset();

    // The unsmoothed 0..1 level of a slice with this RMS (full scale 1.0).
    static double levelFor(double rms);

private:
    void close(double rms);

    int m_channels = 0;
    int m_bucketFrames = 0;
    int m_filled = 0;     // frames in the open slice
    double m_sumSq = 0;   // and their summed squares
    double m_level = 0;   // the smoothed level
    QVector<float> m_points;
};

// Follows the two recordings of a call (what we send, what we receive) and turns them into
// waveforms for the dialer's display.
class CallAudioMonitor : public QObject {
    Q_OBJECT
public:
    explicit CallAudioMonitor(QObject *parent = nullptr);
    ~CallAudioMonitor() override;

    // `releaseRead`: see WavTail; true only for files leansipd deletes after the call.
    void start(const QString &sentPath, const QString &receivedPath, bool releaseRead);
    void stop();
    bool isRunning() const { return m_timer.isActive(); }

    // Reads whatever arrived and emits waveformsChanged(); the timer calls this every kPollMs.
    void poll();

    static constexpr int kPollMs = 20;
    // A direction that stops delivering audio (hold, a stalled writer) falls back to silence
    // after this long.
    static constexpr int kSilenceAfterMs = 250;
    static constexpr int kMaxBacklogMs = 200;

signals:
    void waveformsChanged(const QVector<float> &received, const QVector<float> &sent);

private:
    struct Direction {
        std::unique_ptr<WavTail> tail;
        Waveform waveform;
        QElapsedTimer lastData;
        bool failed = false;
    };
    void pollDirection(Direction &d);

    Direction m_sent;
    Direction m_received;
    QTimer m_timer;
};

} // namespace leansip
