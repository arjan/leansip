#include "core/call_audio.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <cmath>
#include <numbers>

#include <sys/stat.h>

using namespace leansip;

namespace {

QVector<qint16> sine(double hz, double amplitude, int sampleRate, int frames, int channels = 1)
{
    QVector<qint16> out;
    out.reserve(frames * channels);
    for (int i = 0; i < frames; ++i) {
        const auto v = static_cast<qint16>(amplitude * 32767.0 * std::sin(2.0 * std::numbers::pi * hz * i / sampleRate));
        for (int c = 0; c < channels; ++c)
            out.append(v);
    }
    return out;
}

// A WAV header the way a recorder leaves it while still writing: sizes 0.
QByteArray openHeader(int sampleRate, int channels)
{
    QByteArray h;
    auto u32 = [&h](quint32 v) { h.append(reinterpret_cast<const char *>(&v), 4); };
    auto u16 = [&h](quint16 v) { h.append(reinterpret_cast<const char *>(&v), 2); };
    h.append("RIFF");
    u32(0);
    h.append("WAVEfmt ");
    u32(16);
    u16(1);
    u16(static_cast<quint16>(channels));
    u32(static_cast<quint32>(sampleRate));
    u32(static_cast<quint32>(sampleRate * channels * 2));
    u16(static_cast<quint16>(channels * 2));
    u16(16);
    h.append("data");
    u32(0);
    return h;
}

void append(const QString &path, const QByteArray &bytes)
{
    QFile f(path);
    QVERIFY(f.open(QIODevice::Append));
    QCOMPARE(f.write(bytes), bytes.size());
}

QByteArray raw(const QVector<qint16> &samples)
{
    return QByteArray(reinterpret_cast<const char *>(samples.constData()), samples.size() * 2);
}

} // namespace

class TestCallAudio : public QObject {
    Q_OBJECT
private slots:
    void levelsMapDecibelsOntoTheDisplay()
    {
        QCOMPARE(Waveform::levelFor(0.0), 0.0);
        QCOMPARE(Waveform::levelFor(0.001), 0.0);                 // -60 dBFS: below the floor
        QVERIFY(qFuzzyCompare(Waveform::levelFor(0.01), 0.3));     // -40 dBFS
        QVERIFY(qAbs(Waveform::levelFor(0.562341) - 1.0) < 1e-5);  // -5 dBFS: the ceiling
        QCOMPARE(Waveform::levelFor(0.7), 1.0);                    // above it: clamped
        QCOMPARE(Waveform::levelFor(1.0), 1.0);
    }

    void activityRisesFastAndFallsSmoothly()
    {
        Waveform w;
        QCOMPARE(w.points().size(), Waveform::kLength);
        const QVector<qint16> ignored(80, 1000);
        w.push(ignored.constData(), ignored.size()); // no format yet: ignored
        QCOMPARE(w.points().last(), 0.0f);

        w.setFormat(8000, 1); // 40 frames per 5 ms point
        const auto loud = sine(440, 0.5, 8000, 8000);
        w.push(loud.constData(), 30); // not a whole slice yet
        QCOMPARE(w.points().last(), 0.0f);
        w.push(loud.constData() + 30, 8 * 30 - 30); // 30 ms of a -9 dBFS tone
        QVERIFY2(w.points().last() > 0.8f, qPrintable(QString::number(w.points().last())));
        // Not in one jump: the first point after silence is only part of the way there.
        QVERIFY(w.points().at(Waveform::kLength - 6) < 0.5f);
        w.push(loud.constData(), 8 * 200);
        const float steady = w.points().last();
        QVERIFY(qAbs(steady - 0.92f) < 0.01f); // -9 dBFS

        // Silence: down within a few hundred ms, but gradually.
        w.pushSilence(20);
        QVERIFY(w.points().last() > 0.5f * steady);
        w.pushSilence(500);
        QVERIFY(w.points().last() < 0.02f);
        for (qsizetype i = Waveform::kLength - 104; i < Waveform::kLength; ++i)
            QVERIFY(w.points()[i] <= w.points()[i - 1]); // never bounces back

        // Channels are mixed: opposite channels cancel out.
        Waveform st;
        st.setFormat(8000, 2);
        QVector<qint16> frames(80 * 2);
        for (qsizetype i = 0; i < frames.size(); i += 2) {
            frames[i] = 10000;
            frames[i + 1] = -10000;
        }
        st.push(frames.constData(), frames.size());
        QCOMPARE(st.points().last(), 0.0f);

        // A new format starts over.
        w.setFormat(48000, 2);
        QVERIFY(std::all_of(w.points().cbegin(), w.points().cend(), [](float v) { return v == 0.0f; }));
    }

    void tailFollowsAGrowingFile()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("dump.wav");
        WavTail tail(path, false);
        QString error;
        auto got = tail.poll(1000, &error);
        QVERIFY(got && got->isEmpty()); // not created yet

        append(path, openHeader(8000, 2).left(20)); // header half written
        got = tail.poll(1000, &error);
        QVERIFY(got && got->isEmpty());
        append(path, openHeader(8000, 2).mid(20)); // and the rest
        got = tail.poll(1000, &error);
        QVERIFY2(got, qPrintable(error));
        QVERIFY(got->isEmpty());
        QCOMPARE(tail.sampleRate(), 8000);
        QCOMPARE(tail.channels(), 2);

        const QVector<qint16> first{1, 2, 3, 4, 5, 6};
        append(path, raw(first) + QByteArray("\x07", 1)); // and half a sample of the next frame
        got = tail.poll(1000, &error);
        QVERIFY(got);
        QCOMPARE(*got, first);
        append(path, QByteArray("\x00\x08", 2)); // sample 7 complete, 8 still half
        got = tail.poll(1000, &error);
        QVERIFY(got && got->isEmpty()); // no whole frame yet
        append(path, QByteArray("\x00", 1));
        got = tail.poll(1000, &error);
        QVERIFY(got);
        QCOMPARE(*got, (QVector<qint16>{7, 8}));

        // A reader that falls behind gets only the newest part.
        append(path, raw(QVector<qint16>(8000 * 2, 0)) + raw(QVector<qint16>(800 * 2, 5)));
        got = tail.poll(100, &error); // 100 ms = 800 frames
        QVERIFY(got);
        QCOMPARE(got->size(), 1600);
        QVERIFY(std::all_of(got->cbegin(), got->cend(), [](qint16 v) { return v == 5; }));
    }

    void tailRejectsOtherFormats()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("float.wav");
        QByteArray header = openHeader(8000, 1);
        header[20] = 3; // IEEE float
        append(path, header);
        WavTail tail(path, false);
        QString error;
        QVERIFY(!tail.poll(1000, &error));
        QVERIFY(error.contains("16-bit"));
    }

    void tailReleasesWhatItReadOnlyWhenAllowed()
    {
        QTemporaryDir dir;
        for (const bool release : {false, true}) {
            const QString path = dir.filePath(release ? "live.wav" : "kept.wav");
            append(path, openHeader(8000, 1));
            WavTail tail(path, release);
            QString error;
            QVERIFY(tail.poll(10000, &error));
            const QByteArray audio = raw(QVector<qint16>(80000, 1000)); // 160 kB
            append(path, audio);
            const auto got = tail.poll(10000, &error);
            QVERIFY2(got, qPrintable(error));
            QCOMPARE(got->size(), 80000);

            struct stat st {};
            QCOMPARE(stat(QFile::encodeName(path).constData(), &st), 0);
            const qint64 allocated = static_cast<qint64>(st.st_blocks) * 512;
            QFile f(path);
            QVERIFY(f.open(QIODevice::ReadOnly));
            const QByteArray content = f.readAll();
            QCOMPARE(content.size(), 44 + audio.size()); // the size never changes
            QCOMPARE(content.left(4), QByteArray("RIFF")); // nor the header
            if (release) {
                QCOMPARE(tail.releasedUpTo(), (44 + audio.size()) / 4096 * 4096);
                QVERIFY2(allocated <= 2 * 4096, qPrintable(QString::number(allocated)));
                QCOMPARE(content.mid(4096, 2), QByteArray(2, '\0')); // released audio reads as silence
            } else {
                QCOMPARE(tail.releasedUpTo(), 4096);
                QCOMPARE(content.mid(4096, 2), audio.mid(4096 - 44, 2));
                QVERIFY(allocated >= audio.size());
            }
        }
    }

    void monitorEmitsWaveformsAndRollsOnInSilence()
    {
        QTemporaryDir dir;
        const QString sent = dir.filePath("sent.wav");
        const QString received = dir.filePath("received.wav");
        append(sent, openHeader(8000, 1));
        append(received, openHeader(8000, 1));

        CallAudioMonitor monitor;
        QSignalSpy waves(&monitor, &CallAudioMonitor::waveformsChanged);
        monitor.start(sent, received, true);
        QVERIFY(monitor.isRunning());
        monitor.poll(); // reads the headers

        append(received, raw(sine(440, 0.5, 8000, 800))); // 100 ms
        monitor.poll();
        auto receivedWave = waves.last().at(0).value<QVector<float>>();
        auto sentWave = waves.last().at(1).value<QVector<float>>();
        QVERIFY(receivedWave.last() > 0.8f);
        QCOMPARE(sentWave.last(), 0.0f);

        // No more audio: after kSilenceAfterMs it keeps rolling and falls to nothing.
        QTest::qWait(CallAudioMonitor::kSilenceAfterMs + 50);
        for (int i = 0; i < 40; ++i)
            monitor.poll();
        receivedWave = waves.last().at(0).value<QVector<float>>();
        QVERIFY(receivedWave.last() < 0.02f);
        QVERIFY(std::any_of(receivedWave.cbegin(), receivedWave.cend(), [](float v) { return v > 0.8f; }));

        monitor.stop();
        QVERIFY(!monitor.isRunning());
        receivedWave = waves.last().at(0).value<QVector<float>>();
        QVERIFY(std::all_of(receivedWave.cbegin(), receivedWave.cend(), [](float v) { return v == 0.0f; }));
    }
};

QTEST_GUILESS_MAIN(TestCallAudio)
#include "tst_call_audio.moc"
