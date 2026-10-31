#include "core/elevenlabs.h"
#include "core/wav.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

using namespace leansip;

// Just enough HTTP/1.1 to answer one request per connection with a canned reply.
class FakeHttp : public QObject {
    Q_OBJECT
public:
    struct Request {
        QByteArray method;
        QByteArray target;
        QHash<QByteArray, QByteArray> headers; // lower-case names
        QByteArray body;
    };

    FakeHttp()
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            QTcpSocket *s = m_server.nextPendingConnection();
            auto *buffer = new QByteArray;
            connect(s, &QTcpSocket::disconnected, s, [s, buffer] {
                delete buffer;
                s->deleteLater();
            });
            connect(s, &QTcpSocket::readyRead, this, [this, s, buffer] {
                buffer->append(s->readAll());
                const qsizetype end = buffer->indexOf("\r\n\r\n");
                if (end < 0)
                    return;
                Request r;
                const QList<QByteArray> lines = buffer->left(end).split('\n');
                const QList<QByteArray> first = lines.first().trimmed().split(' ');
                r.method = first.value(0);
                r.target = first.value(1);
                for (qsizetype i = 1; i < lines.size(); ++i) {
                    const qsizetype colon = lines[i].indexOf(':');
                    if (colon > 0)
                        r.headers.insert(lines[i].left(colon).trimmed().toLower(), lines[i].mid(colon + 1).trimmed());
                }
                const qsizetype length = r.headers.value("content-length").toLongLong();
                if (buffer->size() < end + 4 + length)
                    return;
                r.body = buffer->mid(end + 4, length);
                requests.append(r);
                QByteArray reply = "HTTP/1.1 " + QByteArray::number(status) + " X\r\nContent-Type: " + contentType
                                   + "\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
                s->write(reply);
                s->disconnectFromHost();
            });
        });
    }
    bool listen() { return m_server.listen(QHostAddress::LocalHost, 0); }
    QString url() const { return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort()); }

    int status = 200;
    QByteArray contentType = "application/json";
    QByteArray body;
    QList<Request> requests;

private:
    QTcpServer m_server;
};

class TestElevenLabs : public QObject {
    Q_OBJECT
private slots:
    void init()
    {
        for (const char *v : {"ELEVENLABS_API_KEY", "ELEVENLABS_BASE_URL", "ELEVENLABS_STT_MODEL", "ELEVENLABS_TTS_MODEL",
                              "ELEVENLABS_VOICE_ID"})
            qunsetenv(v);
    }

    void configNeedsAKeyAndASafeUrl()
    {
        QString error;
        QVERIFY(!elevenlabs::Config::fromEnvironment(&error));
        QVERIFY(error.contains("ELEVENLABS_API_KEY"));

        qputenv("ELEVENLABS_API_KEY", "k");
        auto c = elevenlabs::Config::fromEnvironment(&error);
        QVERIFY(c);
        QCOMPARE(c->baseUrl, "https://api.elevenlabs.io");
        QCOMPARE(c->sttModel, "scribe_v2");
        QCOMPARE(c->ttsModel, "eleven_multilingual_v2");

        // The key must never go out over plain http to another machine.
        qputenv("ELEVENLABS_BASE_URL", "http://api.example.com");
        QVERIFY(!elevenlabs::Config::fromEnvironment(&error));
        QVERIFY(error.contains("https"));
        qputenv("ELEVENLABS_BASE_URL", "http://127.0.0.1:9/");
        c = elevenlabs::Config::fromEnvironment(&error);
        QVERIFY(c);
        QCOMPARE(c->baseUrl, "http://127.0.0.1:9");

        qputenv("ELEVENLABS_VOICE_ID", "voice1");
        qputenv("ELEVENLABS_STT_MODEL", "scribe_v1");
        c = elevenlabs::Config::fromEnvironment(&error);
        QCOMPARE(c->voiceId, "voice1");
        QCOMPARE(c->sttModel, "scribe_v1");
    }

    void synthesizeAsksForPcm16k()
    {
        FakeHttp http;
        QVERIFY(http.listen());
        http.contentType = "application/octet-stream";
        http.body = QByteArray(3200, '\x01'); // 100 ms of 16 kHz mono
        elevenlabs::Config c;
        c.apiKey = "secret";
        c.baseUrl = http.url();
        c.voiceId = "v/1";
        QString error;
        const auto pcm = elevenlabs::synthesize(c, "Hello there", &error, 5000);
        QVERIFY2(pcm, qPrintable(error));
        QCOMPARE(pcm->size(), 3200);
        QCOMPARE(http.requests.size(), 1);
        const auto &r = http.requests.first();
        QCOMPARE(r.method, "POST");
        QCOMPARE(r.target, "/v1/text-to-speech/v%2F1?output_format=pcm_16000");
        QCOMPARE(r.headers.value("xi-api-key"), "secret");
        const QJsonObject body = QJsonDocument::fromJson(r.body).object();
        QCOMPARE(body["text"].toString(), "Hello there");
        QCOMPARE(body["model_id"].toString(), "eleven_multilingual_v2");
    }

    void transcribeUploadsMultipart()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("a.wav");
        QString error;
        QVERIFY(wav::writeFile(path, wav::fromRaw(QByteArray(320, '\0'), 16000, 1), &error));
        FakeHttp http;
        QVERIFY(http.listen());
        http.body = R"({"language_code":"nld","language_probability":0.98,"text":" Goedemiddag ","words":[{"text":"Goedemiddag","start":0.1,"end":0.6}]})";
        elevenlabs::Config c;
        c.apiKey = "secret";
        c.baseUrl = http.url();
        const auto t = elevenlabs::transcribe(c, path, &error, "nl", 5000);
        QVERIFY2(t, qPrintable(error));
        QCOMPARE(t->text, "Goedemiddag");
        QCOMPARE(t->languageCode, "nld");
        QCOMPARE(t->words.size(), 1);
        const auto &r = http.requests.first();
        QCOMPARE(r.target, "/v1/speech-to-text");
        QVERIFY(r.headers.value("content-type").startsWith("multipart/form-data"));
        QVERIFY(r.body.contains("name=\"model_id\"\r\n\r\nscribe_v2"));
        QVERIFY(r.body.contains("name=\"language_code\"\r\n\r\nnl"));
        QVERIFY(r.body.contains("name=\"file\"; filename=\"a.wav\""));
        QVERIFY(r.body.contains("RIFF"));
    }

    void errorsCarryStatusAndMessage()
    {
        FakeHttp http;
        QVERIFY(http.listen());
        http.status = 401;
        http.body = R"({"detail":{"status":"invalid_api_key","message":"Invalid API key"}})";
        elevenlabs::Config c;
        c.apiKey = "wrong";
        c.baseUrl = http.url();
        QString error;
        QVERIFY(!elevenlabs::synthesize(c, "x", &error, 5000));
        QVERIFY(error.contains("401"));
        QVERIFY(error.contains("Invalid API key"));
        QVERIFY(!error.contains("wrong")); // never echo the key
    }

    void unreachableServerFails()
    {
        elevenlabs::Config c;
        c.apiKey = "k";
        c.baseUrl = "http://127.0.0.1:1";
        QString error;
        QVERIFY(!elevenlabs::synthesize(c, "x", &error, 3000));
        QVERIFY(!error.isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestElevenLabs)
#include "tst_elevenlabs.moc"
