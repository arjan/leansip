#include "call_audio.h"

#include <QLoggingCategory>

#include <cmath>

#include <fcntl.h>

namespace leansip {

Q_LOGGING_CATEGORY(lcCallAudio, "leansip.callaudio")

namespace {

constexpr qint64 kPage = 4096;

qint64 roundUp(qint64 value, qint64 to) { return (value + to - 1) / to * to; }

} // namespace

WavTail::WavTail(const QString &path, bool releaseRead)
    : m_file(path)
    , m_release(releaseRead)
{
}

std::optional<QVector<qint16>> WavTail::poll(int maxBacklogMs, QString *error)
{
    if (!m_file.isOpen()) {
        // Punching holes needs a descriptor open for writing; nothing is ever written through it.
        const auto mode = (m_release ? QIODevice::ReadWrite : QIODevice::ReadOnly) | QIODevice::Unbuffered;
        if (!m_file.exists())
            return QVector<qint16>(); // not created yet
        if (!m_file.open(mode)) {
            *error = QStringLiteral("cannot open %1: %2").arg(m_file.fileName(), m_file.errorString());
            return std::nullopt;
        }
    }
    const qint64 size = m_file.size();
    if (!m_header) {
        if (size < 44)
            return QVector<qint16>(); // the header is still being written
        m_file.seek(0);
        QString why;
        const auto header = wav::readHeader(m_file, size, &why);
        if (!header) {
            if (size < kPage)
                return QVector<qint16>();
            *error = QStringLiteral("%1: %2").arg(m_file.fileName(), why);
            return std::nullopt;
        }
        if (header->formatTag != 1 || header->bitsPerSample != 16 || header->channels < 1 || header->channels > 2
            || header->sampleRate <= 0) {
            *error = QStringLiteral("%1: not 16-bit PCM mono/stereo").arg(m_file.fileName());
            return std::nullopt;
        }
        m_header = header;
        m_offset = header->dataOffset;
        m_releasedTo = roundUp(header->dataOffset, kPage); // the header's page stays
    }

    const qint64 frameBytes = 2LL * m_header->channels;
    qint64 available = size - m_offset;
    available -= available % frameBytes;
    if (available <= 0)
        return QVector<qint16>();
    const qint64 maxBytes = static_cast<qint64>(m_header->sampleRate) * maxBacklogMs / 1000 * frameBytes;
    if (available > maxBytes) {
        m_offset += available - maxBytes;
        available = maxBytes;
    }
    if (!m_file.seek(m_offset)) {
        *error = QStringLiteral("%1: seek failed").arg(m_file.fileName());
        return std::nullopt;
    }
    QVector<qint16> samples(static_cast<qsizetype>(available / 2));
    const qint64 got = m_file.read(reinterpret_cast<char *>(samples.data()), available);
    if (got < 0) {
        *error = QStringLiteral("%1: %2").arg(m_file.fileName(), m_file.errorString());
        return std::nullopt;
    }
    const qint64 whole = got - got % frameBytes;
    samples.resize(static_cast<qsizetype>(whole / 2));
    m_offset += whole;

    if (m_release) {
        const qint64 to = m_offset / kPage * kPage;
        if (to - m_releasedTo >= kReleaseChunk) {
            if (fallocate(m_file.handle(), FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, m_releasedTo, to - m_releasedTo) == 0)
                m_releasedTo = to;
            else
                m_release = false; // not supported here; keep reading without it
        }
    }
    return samples;
}

Waveform::Waveform()
    : m_points(kLength, 0.0f)
{
}

void Waveform::setFormat(int sampleRate, int channels)
{
    const int bucketFrames = sampleRate > 0 ? std::max(1, sampleRate * kBucketMs / 1000) : 0;
    if (bucketFrames == m_bucketFrames && channels == m_channels)
        return;
    m_bucketFrames = bucketFrames;
    m_channels = channels > 0 ? channels : 0;
    reset();
}

void Waveform::reset()
{
    m_points.fill(0.0f);
    m_filled = 0;
    m_sumSq = 0;
    m_level = 0;
}

double Waveform::levelFor(double rms)
{
    if (rms <= 0)
        return 0;
    return std::clamp((20.0 * std::log10(rms) - kFloorDb) / (kCeilDb - kFloorDb), 0.0, 1.0);
}

void Waveform::close(double rms)
{
    const double target = levelFor(rms);
    const double tau = target > m_level ? kAttackMs : kReleaseMs;
    m_level += (target - m_level) * (1.0 - std::exp(-kBucketMs / tau));
    m_points.removeFirst();
    m_points.append(static_cast<float>(m_level));
}

void Waveform::push(const qint16 *samples, qsizetype count)
{
    if (m_bucketFrames == 0 || m_channels == 0)
        return;
    const qsizetype frames = count / m_channels;
    for (qsizetype f = 0; f < frames; ++f) {
        int sum = 0;
        for (int c = 0; c < m_channels; ++c)
            sum += samples[f * m_channels + c];
        const double v = static_cast<double>(sum) / (32768.0 * m_channels);
        m_sumSq += v * v;
        if (++m_filled == m_bucketFrames) {
            close(std::sqrt(m_sumSq / m_bucketFrames));
            m_filled = 0;
            m_sumSq = 0;
        }
    }
}

void Waveform::pushSilence(int ms)
{
    if (m_bucketFrames == 0)
        return;
    for (int i = 0; i < ms / kBucketMs; ++i)
        close(0.0);
}

CallAudioMonitor::CallAudioMonitor(QObject *parent)
    : QObject(parent)
{
    m_timer.setInterval(kPollMs);
    connect(&m_timer, &QTimer::timeout, this, &CallAudioMonitor::poll);
}

CallAudioMonitor::~CallAudioMonitor() = default;

void CallAudioMonitor::start(const QString &sentPath, const QString &receivedPath, bool releaseRead)
{
    stop();
    m_sent.tail = sentPath.isEmpty() ? nullptr : std::make_unique<WavTail>(sentPath, releaseRead);
    m_received.tail = receivedPath.isEmpty() ? nullptr : std::make_unique<WavTail>(receivedPath, releaseRead);
    m_timer.start();
}

void CallAudioMonitor::stop()
{
    m_timer.stop();
    for (Direction *d : {&m_sent, &m_received}) {
        d->tail.reset();
        d->waveform.reset();
        d->lastData.invalidate();
        d->failed = false;
    }
    emit waveformsChanged(m_received.waveform.points(), m_sent.waveform.points());
}

void CallAudioMonitor::pollDirection(Direction &d)
{
    if (!d.tail || d.failed)
        return;
    QString error;
    const auto samples = d.tail->poll(kMaxBacklogMs, &error);
    if (!samples) {
        qCWarning(lcCallAudio) << error;
        d.failed = true;
        d.waveform.reset();
        return;
    }
    d.waveform.setFormat(d.tail->sampleRate(), d.tail->channels());
    if (!samples->isEmpty()) {
        d.waveform.push(samples->constData(), samples->size());
        d.lastData.start();
    } else if (d.lastData.isValid() && d.lastData.elapsed() > kSilenceAfterMs && d.tail->sampleRate() > 0) {
        // Keep rolling with a flat line instead of freezing.
        d.waveform.pushSilence(kPollMs);
    }
}

void CallAudioMonitor::poll()
{
    pollDirection(m_sent);
    pollDirection(m_received);
    emit waveformsChanged(m_received.waveform.points(), m_sent.waveform.points());
}

} // namespace leansip
