#include "core/dtmf_tone.h"

#include <QtTest>

#include <cmath>
#include <numbers>

using namespace leansip;

class TestDtmfTone : public QObject {
    Q_OBJECT
private slots:
    void frequencyTable()
    {
        QCOMPARE(dtmf::frequencies('1'), std::make_pair(697, 1209));
        QCOMPARE(dtmf::frequencies('5'), std::make_pair(770, 1336));
        QCOMPARE(dtmf::frequencies('9'), std::make_pair(852, 1477));
        QCOMPARE(dtmf::frequencies('0'), std::make_pair(941, 1336));
        QCOMPARE(dtmf::frequencies('*'), std::make_pair(941, 1209));
        QCOMPARE(dtmf::frequencies('#'), std::make_pair(941, 1477));
        QCOMPARE(dtmf::frequencies('D'), std::make_pair(941, 1633));
        QCOMPARE(dtmf::frequencies('d'), std::make_pair(941, 1633));
        QVERIFY(!dtmf::frequencies('x').has_value());
        QVERIFY(!dtmf::frequencies('+').has_value());
    }

    void sampleLengthAndEnvelope()
    {
        const QByteArray pcm = dtmf::samples('5', 100, 8000);
        QCOMPARE(pcm.size(), 800 * 2);
        const auto *s = reinterpret_cast<const qint16 *>(pcm.constData());
        QCOMPARE(s[0], 0);     // fade-in starts silent
        QCOMPARE(s[799], 0);   // fade-out ends silent
        qint16 peak = 0;
        for (int i = 0; i < 800; ++i)
            peak = std::max<qint16>(peak, static_cast<qint16>(std::abs(s[i])));
        QVERIFY(peak > 4000);  // audible
        QVERIFY(peak < 12000); // and well below clipping
        QVERIFY(dtmf::samples('x', 100, 8000).isEmpty());
        QVERIFY(dtmf::samples('1', 0, 8000).isEmpty());
    }

    void containsBothFrequencies()
    {
        // Goertzel power at the two expected tones must dwarf a nearby unrelated frequency.
        const int rate = 8000;
        const QByteArray pcm = dtmf::samples('5', 200, rate);
        const auto *s = reinterpret_cast<const qint16 *>(pcm.constData());
        const int n = static_cast<int>(pcm.size() / 2);
        auto power = [&](int hz) {
            const double k = 2.0 * std::cos(2.0 * std::numbers::pi * hz / rate);
            double q0 = 0, q1 = 0, q2 = 0;
            for (int i = 0; i < n; ++i) {
                q0 = k * q1 - q2 + s[i];
                q2 = q1;
                q1 = q0;
            }
            return q1 * q1 + q2 * q2 - k * q1 * q2;
        };
        QVERIFY(power(770) > 100 * power(1000));
        QVERIFY(power(1336) > 100 * power(1000));
        QVERIFY(power(697) < power(770) / 100);
    }

    void wavHeader()
    {
        const QByteArray pcm = dtmf::samples('1', 50, 8000);
        const QByteArray w = dtmf::wav(pcm, 8000);
        QCOMPARE(w.size(), 44 + pcm.size());
        QCOMPARE(w.left(4), QByteArray("RIFF"));
        QCOMPARE(w.mid(8, 4), QByteArray("WAVE"));
        QCOMPARE(w.mid(12, 4), QByteArray("fmt "));
        QCOMPARE(qFromLittleEndian<quint16>(w.constData() + 20), quint16(1));  // PCM
        QCOMPARE(qFromLittleEndian<quint16>(w.constData() + 22), quint16(1));  // mono
        QCOMPARE(qFromLittleEndian<quint32>(w.constData() + 24), quint32(8000));
        QCOMPARE(qFromLittleEndian<quint16>(w.constData() + 34), quint16(16)); // bits
        QCOMPARE(w.mid(36, 4), QByteArray("data"));
        QCOMPARE(qFromLittleEndian<quint32>(w.constData() + 40), quint32(pcm.size()));
        QCOMPARE(w.mid(44), pcm);
    }

    void soundFileNames()
    {
        QCOMPARE(dtmf::soundFileName('5'), "dtmf-5.wav");
        QCOMPARE(dtmf::soundFileName('*'), "dtmf-star.wav");
        QCOMPARE(dtmf::soundFileName('#'), "dtmf-hash.wav");
        QCOMPARE(dtmf::soundFileName('a'), "dtmf-A.wav");
        QCOMPARE(dtmf::soundFileName('x'), QString());
    }

    void installsSixteenFiles()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString sounds = dir.filePath("sounds");
        QVERIFY(dtmf::installSoundBank(sounds));
        const QStringList files = QDir(sounds).entryList({"*.wav"}, QDir::Files);
        QCOMPARE(files.size(), 16);
        QVERIFY(files.contains("dtmf-0.wav"));
        QVERIFY(files.contains("dtmf-star.wav"));
        QVERIFY(files.contains("dtmf-hash.wav"));
        QVERIFY(files.contains("dtmf-D.wav"));
        QFile f(sounds + "/dtmf-7.wav");
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.read(4), QByteArray("RIFF"));
        QCOMPARE(f.size(), 44 + 8000 * 120 / 1000 * 2);
    }

    void installFailsOnUnwritablePath()
    {
        QVERIFY(!dtmf::installSoundBank("/proc/leansip-cannot-write"));
    }
};

QTEST_APPLESS_MAIN(TestDtmfTone)
#include "tst_dtmf_tone.moc"
