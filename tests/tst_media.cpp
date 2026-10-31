#include "core/call_analysis.h"
#include "core/log_file.h"
#include "core/media_control.h"
#include "core/speech_segmenter.h"
#include "core/wav.h"

#include <QFile>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QtTest>

#include <cmath>

using namespace leansip;

namespace {

wav::Pcm tone(int rate, int channels, double seconds, double hz, double amplitude = 0.5)
{
    wav::Pcm p;
    p.sampleRate = rate;
    p.channels = channels;
    const qint64 frames = static_cast<qint64>(seconds * rate);
    for (qint64 i = 0; i < frames; ++i) {
        const auto v = static_cast<qint16>(std::lround(amplitude * 32767 * std::sin(2 * M_PI * hz * static_cast<double>(i) / rate)));
        for (int c = 0; c < channels; ++c)
            p.samples.append(v);
    }
    return p;
}

// Rough frequency from zero crossings: enough to tell 440 Hz from 1200 Hz.
double zeroCrossingHz(const wav::Pcm &p)
{
    const wav::Pcm mono = wav::mixToMono(p);
    int crossings = 0;
    for (qsizetype i = 1; i < mono.samples.size(); ++i)
        crossings += (mono.samples[i - 1] < 0) != (mono.samples[i] < 0);
    return crossings / 2.0 / mono.seconds();
}

QJsonObject ev(const QString &type, const QString &id, qint64 ts, const QJsonObject &extra = {})
{
    QJsonObject o = extra;
    o["event"] = true;
    o["class"] = extra.value("class").toString("call");
    o["type"] = type;
    o["id"] = id;
    o["ts"] = ts;
    return o;
}

} // namespace

class TestMedia : public QObject {
    Q_OBJECT
private slots:
    void wavRoundTripAndHeader()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("t.wav");
        const wav::Pcm in = tone(16000, 1, 0.5, 440);
        QString error;
        QVERIFY2(wav::writeFile(path, in, &error), qPrintable(error));
        const auto out = wav::readFile(path, &error);
        QVERIFY2(out, qPrintable(error));
        QCOMPARE(out->sampleRate, 16000);
        QCOMPARE(out->channels, 1);
        QCOMPARE(out->samples, in.samples);
        QCOMPARE(QFileInfo(path).size(), 44 + in.samples.size() * 2);
    }

    void headerOfAGrowingFileTrustsTheFileSize()
    {
        // A recording in progress: the data chunk still says 0 bytes.
        QTemporaryDir dir;
        const QString path = dir.filePath("g.wav");
        QString error;
        QVERIFY(wav::writeFile(path, tone(8000, 2, 0.1, 300), &error));
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadWrite));
        f.seek(40);
        f.write(QByteArray(4, '\0'));
        f.seek(f.size());
        f.write(QByteArray(400, '\1'));
        f.flush();
        f.seek(0);
        const auto h = wav::readHeader(f, f.size(), &error);
        QVERIFY2(h, qPrintable(error));
        QCOMPARE(h->dataOffset, 44);
        QCOMPARE(h->dataBytes, f.size() - 44);
        QCOMPARE(h->channels, 2);
    }

    void rejectsNonPcm()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("x.wav");
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("not a wav at all");
        f.close();
        QString error;
        QVERIFY(!wav::readFile(path, &error));
        QVERIFY(error.contains("not a WAV"));
    }

    void convertResamplesAndMapsChannels()
    {
        const wav::Pcm in = tone(16000, 1, 1.0, 1200);
        const wav::Pcm out = wav::convert(in, 48000, 2);
        QCOMPARE(out.sampleRate, 48000);
        QCOMPARE(out.channels, 2);
        QVERIFY(std::abs(out.seconds() - 1.0) < 0.01);
        QVERIFY(std::abs(zeroCrossingHz(out) - 1200) < 30); // pitch survives the rate change
        QCOMPARE(out.samples[0], out.samples[1]);            // both channels carry the signal

        const wav::Pcm back = wav::convert(out, 8000, 1);
        QCOMPARE(back.channels, 1);
        QVERIFY(std::abs(zeroCrossingHz(back) - 1200) < 30);
    }

    void levelsAndSilence()
    {
        wav::Pcm p = tone(8000, 1, 1.0, 440, 0.5);
        wav::appendSilence(p, 1.0);
        QCOMPARE(p.seconds(), 2.0);
        const wav::Levels l = wav::measure(p);
        QVERIFY(l.peakDbfs > -7 && l.peakDbfs < -5); // 0.5 full scale = -6 dBFS
        QVERIFY(std::abs(l.silentFraction - 0.5) < 0.02);
        const wav::Levels quiet = wav::measure(wav::fromRaw(QByteArray(1600, '\0'), 8000, 1));
        QCOMPARE(quiet.silentFraction, 1.0);
        QCOMPARE(quiet.rmsDbfs, -120.0);
    }

    void segmenterFindsOneUtterance()
    {
        SpeechSegmenter::Params p;
        p.thresholdDbfs = -40;
        p.startMs = 100;
        p.silenceMs = 400;
        p.prerollMs = 100;
        SpeechSegmenter seg(p);
        auto feed = [&](double db, int frames) {
            for (int i = 0; i < frames; ++i)
                seg.feed(db);
        };
        feed(-80, 50);              // 1 s of quiet
        feed(-20, 2);               // a click is not speech
        feed(-80, 10);
        QCOMPARE(seg.state(), SpeechSegmenter::State::Waiting);
        feed(-20, 60);              // 1.2 s of speech starting at frame 62
        QCOMPARE(seg.state(), SpeechSegmenter::State::Speaking);
        QCOMPARE(seg.startFrame(), 62 - 5); // 100 ms preroll
        feed(-80, 10);              // a short pause keeps the utterance going
        feed(-20, 5);
        feed(-80, 20);              // 400 ms quiet ends it
        QCOMPARE(seg.state(), SpeechSegmenter::State::Done);
        QVERIFY(!seg.truncated());
        QCOMPARE(seg.endFrame(), 62 + 60 + 10 + 5 + 5); // speech end + 100 ms tail
    }

    void segmenterStopsAtMax()
    {
        SpeechSegmenter::Params p;
        p.maxMs = 1000;
        SpeechSegmenter seg(p);
        for (int i = 0; i < 200 && seg.state() != SpeechSegmenter::State::Done; ++i)
            seg.feed(-10);
        QCOMPARE(seg.state(), SpeechSegmenter::State::Done);
        QVERIFY(seg.truncated());
        QVERIFY(seg.endFrame() - seg.startFrame() <= 50);
    }

    void parsesAudioDebug()
    {
        const QString text = "--- Audio stream ---\n"
                             " tx:   encode: opus 48000Hz/2ch ptime=20ms S16LE\n"
                             "       source: alsa,default S16LE\n"
                             " rx:   decode: opus 48000Hz/2ch S16LE\n";
        const auto f = parseAudioDebug(text);
        QVERIFY(f);
        QCOMPARE(f->codec, "opus");
        QCOMPARE(f->sampleRate, 48000);
        QCOMPARE(f->channels, 2);
        QCOMPARE(f->source, "alsa,default");
        QVERIFY(!parseAudioDebug("--- Audio stream ---\n (no call)"));
        QCOMPARE(parseAudioDebug(" tx:   encode: G722 16000Hz/1ch ptime=20ms\n")->channels, 1);
    }

    void callPolicy()
    {
        QTemporaryDir dir;
        const QString allow = dir.filePath("sipctl-allow");
        CallPolicy none = CallPolicy::load(allow); // missing file: loopback only
        QVERIFY(none.check("sip:bob@127.0.0.1:15061").isEmpty());
        QVERIFY(none.check("bob@localhost").isEmpty());
        QVERIFY(none.check("sip:x@[::1]:5060").isEmpty());
        QVERIFY(none.check("940").contains("not allowed"));
        QVERIFY(none.check("sip:940@voiceworks.com").contains(allow));

        QFile f(allow);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("# local bots\n940\n*@lab.example.com  # whole lab\n+3120*\n");
        f.close();
        const CallPolicy p = CallPolicy::load(allow);
        QCOMPARE(p.patterns(), (QStringList{"940", "*@lab.example.com", "+3120*"}));
        QVERIFY(p.check("940").isEmpty());
        QVERIFY(p.check("sip:940@voiceworks.com").isEmpty()); // the user part matches
        QVERIFY(p.check("tel:+31201234567").isEmpty());
        QVERIFY(p.check("sip:anyone@lab.example.com").isEmpty());
        QVERIFY(!p.check("9400").isEmpty());
        QVERIFY(!p.check("+31612345678").isEmpty());
        QVERIFY(!p.check("sip:anyone@lab.example.com.evil.org").isEmpty());

        QCOMPARE(*CallPolicy::dialTarget("dial", "940"), "940");
        QCOMPARE(*CallPolicy::dialTarget("dialdir", "sip:a@b audio=sendonly"), "sip:a@b");
        QCOMPARE(*CallPolicy::dialTarget("transfer", " 123 "), "123");
        QVERIFY(CallPolicy::dialTarget("dialcontact", {}));
        QVERIFY(!CallPolicy::dialTarget("hangup", {}));
    }

    void recordingTrackerClaimsPairsOldestFirst()
    {
        QTemporaryDir dir;
        const QString incoming = dir.filePath("in");
        const QString recordings = dir.filePath("rec");
        QDir().mkpath(incoming);
        auto touch = [&](const QString &name) {
            QFile f(QDir(incoming).filePath(name));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("RIFF");
        };
        RecordingTracker t(incoming, recordings);
        QVERIFY(!t.onCallEvent("c1")); // nothing yet
        touch("dump-2026-10-31-17-51-55-enc");
        QVERIFY(!t.onCallEvent("c1")); // half a pair
        touch("dump-2026-10-31-17-51-55-dec");
        touch("dump-2026-10-31-17-52-10-enc.wav");
        touch("dump-2026-10-31-17-52-10-dec.wav");
        const auto f1 = t.onCallEvent("c1");
        QVERIFY(f1);
        QCOMPARE(f1->sent, QDir(recordings).filePath("c1-sent.wav"));
        QVERIFY(QFile::exists(f1->received));
        QVERIFY(!QFile::exists(QDir(incoming).filePath("dump-2026-10-31-17-51-55-enc")));
        QVERIFY(!t.onCallEvent("c1")); // already has its files
        const auto f2 = t.onCallEvent("c2");
        QVERIFY(f2);
        QCOMPARE(QFileInfo(f2->received).fileName(), "c2-received.wav");
        QCOMPARE(t.filesFor("c2")->sent, f2->sent);
        QCOMPARE(t.toJson().keys(), (QStringList{"c1", "c2"}));
    }

    void analyzeCallSummary()
    {
        QJsonArray events;
        events.append(ev("CALL_LOCAL_SDP", "c1", 1000, {{"direction", "outgoing"}, {"accountaor", "sip:a@x"}, {"peeruri", "sip:940@x"}}));
        events.append(ev("CALL_RINGING", "c1", 1300));
        events.append(ev("CALL_INCOMING", "other", 1350));
        events.append(ev("CALL_ESTABLISHED", "c1", 4000));
        events.append(ev("CALL_AUDIO", "c1", 4010, {{"codec", "G722"}, {"srate", 16000}, {"channels", 1}}));
        events.append(ev("CALL_DTMF_START", "c1", 5000, {{"param", "1"}}));
        events.append(ev("CALL_DTMF_START", "c1", 5500, {{"param", "#"}}));
        QJsonObject rtcp{{"rtt", 3}, {"rx", QJsonObject{{"jit", 400}, {"lost", 0}, {"sent", 100}}},
                         {"tx", QJsonObject{{"jit", 900}, {"lost", 2}, {"sent", 100}}}};
        events.append(ev("CALL_RTCP", "c1", 9000, {{"rtcp_stats", rtcp}}));
        events.append(ev("CALL_CLOSED", "c1", 14000, {{"param", "Connection reset by peer\n"}}));

        QCOMPARE(analysis::lastCallId(events), "c1");
        wav::Levels sent;
        sent.silentFraction = 0.2;
        wav::Levels received; // all silent
        const QJsonObject a = analysis::analyzeCall(events, "c1", sent, received);
        QVERIFY(a["found"].toBool());
        QCOMPARE(a["direction"].toString(), "outgoing");
        QCOMPARE(a["peer"].toString(), "sip:940@x");
        QCOMPARE(a["timing"].toObject()["ringingAfterMs"].toInt(), 300);
        QCOMPARE(a["timing"].toObject()["answeredAfterMs"].toInt(), 3000);
        QCOMPARE(a["timing"].toObject()["talkMs"].toInt(), 10000);
        QCOMPARE(a["closeReason"].toString(), "Connection reset by peer");
        QCOMPARE(a["dtmfReceived"].toString(), "1#");
        QCOMPARE(a["audio"].toObject()["codec"].toString(), "G722");
        QCOMPARE(a["rtcp"].toObject()["maxTxJitter"].toDouble(), 900.0);
        const QJsonArray warnings = a["warnings"].toArray();
        QVERIFY(warnings.contains(QJsonValue("packet loss reported by RTCP")));
        QVERIFY(warnings.contains(QJsonValue("one-way audio: nothing received while sending")));

        QJsonArray rejected;
        rejected.append(ev("CALL_LOCAL_SDP", "c2", 0));
        rejected.append(ev("CALL_CLOSED", "c2", 500, {{"param", "486 Busy Here"}}));
        const QJsonObject r = analysis::analyzeCall(rejected, "c2");
        QCOMPARE(r["state"].toString(), "closed");
        QVERIFY(r["warnings"].toArray().contains(QJsonValue("call was never answered")));
        QVERIFY(!analysis::analyzeCall(rejected, "nope")["found"].toBool());
    }

    void logFileRotates()
    {
        QTemporaryDir dir;
        LogFile log(dir.filePath("d.log"), 1000);
        QString error;
        QVERIFY(log.open(&error));
        for (int i = 0; i < 30; ++i)
            log.writeLine(QString(50, 'x'));
        QVERIFY(QFile::exists(dir.filePath("d.log.1")));
        QVERIFY(QFileInfo(dir.filePath("d.log")).size() < 1000);
        QVERIFY(QFileInfo(dir.filePath("d.log.1")).size() >= 1000);
        QCOMPARE(QFileInfo(dir.filePath("d.log")).permissions() & (QFileDevice::ReadOther | QFileDevice::ReadGroup), QFileDevice::Permissions{});
    }
};

QTEST_GUILESS_MAIN(TestMedia)
#include "tst_media.moc"
