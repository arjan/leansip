#include "elevenlabs.h"

#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QHttpMultiPart>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTimer>
#include <QUrlQuery>

#include <memory>

namespace leansip::elevenlabs {

namespace {

QString env(const char *name)
{
    return qEnvironmentVariable(name).trimmed();
}

bool isLoopbackHost(const QString &host)
{
    if (host == QLatin1String("localhost"))
        return true;
    QHostAddress address;
    return address.setAddress(host) && address.isLoopback();
}

struct HttpResult {
    int status = 0;
    QByteArray body;
    QString networkError;
};

HttpResult run(QNetworkReply *reply, int timeoutMs)
{
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, reply, &QNetworkReply::abort);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(timeoutMs);
    loop.exec();
    HttpResult r;
    r.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    r.body = reply->readAll();
    if (reply->error() != QNetworkReply::NoError && r.status == 0)
        r.networkError = timer.isActive() ? reply->errorString() : QStringLiteral("timed out");
    reply->deleteLater();
    return r;
}

QString describe(const HttpResult &r, const char *what)
{
    if (!r.networkError.isEmpty())
        return QStringLiteral("ElevenLabs %1: %2").arg(QLatin1String(what), r.networkError);
    // Error bodies are JSON like {"detail":{"status":"...","message":"..."}}.
    const QJsonObject detail = QJsonDocument::fromJson(r.body).object().value(QStringLiteral("detail")).toObject();
    const QString message = detail.value(QStringLiteral("message")).toString();
    return QStringLiteral("ElevenLabs %1 failed: HTTP %2%3")
        .arg(QLatin1String(what))
        .arg(r.status)
        .arg(message.isEmpty() ? QStringLiteral(" ") + QString::fromUtf8(r.body.left(300)) : QStringLiteral(": ") + message);
}

QNetworkRequest request(const Config &config, const QString &path)
{
    QNetworkRequest req(QUrl(config.baseUrl + path));
    req.setRawHeader("xi-api-key", config.apiKey.toUtf8());
    req.setTransferTimeout(0);
    return req;
}

} // namespace

std::optional<Config> Config::fromEnvironment(QString *error)
{
    Config c;
    c.apiKey = env("ELEVENLABS_API_KEY");
    if (c.apiKey.isEmpty()) {
        if (error)
            *error = QStringLiteral("ELEVENLABS_API_KEY is not set; speech-to-text and text-to-speech need it");
        return std::nullopt;
    }
    c.baseUrl = env("ELEVENLABS_BASE_URL");
    if (c.baseUrl.isEmpty())
        c.baseUrl = QStringLiteral("https://api.elevenlabs.io");
    while (c.baseUrl.endsWith(u'/'))
        c.baseUrl.chop(1);
    const QUrl url(c.baseUrl);
    if (!url.isValid() || (url.scheme() != QLatin1String("https") && !(url.scheme() == QLatin1String("http") && isLoopbackHost(url.host())))) {
        if (error)
            *error = QStringLiteral("ELEVENLABS_BASE_URL must be an https URL (http only for loopback)");
        return std::nullopt;
    }
    if (const QString v = env("ELEVENLABS_STT_MODEL"); !v.isEmpty())
        c.sttModel = v;
    if (const QString v = env("ELEVENLABS_TTS_MODEL"); !v.isEmpty())
        c.ttsModel = v;
    if (const QString v = env("ELEVENLABS_VOICE_ID"); !v.isEmpty())
        c.voiceId = v;
    return c;
}

std::optional<Transcript> transcribe(const Config &config, const QString &wavPath, QString *error,
                                     const QString &languageCode, int timeoutMs)
{
    auto file = std::make_unique<QFile>(wavPath);
    if (!file->open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("cannot open %1: %2").arg(wavPath, file->errorString());
        return std::nullopt;
    }
    auto *multi = new QHttpMultiPart(QHttpMultiPart::FormDataType);
    auto field = [multi](const QString &name, const QString &value) {
        QHttpPart part;
        part.setHeader(QNetworkRequest::ContentDispositionHeader, QStringLiteral("form-data; name=\"%1\"").arg(name));
        part.setBody(value.toUtf8());
        multi->append(part);
    };
    field(QStringLiteral("model_id"), config.sttModel);
    if (!languageCode.isEmpty())
        field(QStringLiteral("language_code"), languageCode);
    QHttpPart audio;
    audio.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("audio/wav"));
    audio.setHeader(QNetworkRequest::ContentDispositionHeader,
                    QStringLiteral("form-data; name=\"file\"; filename=\"%1\"").arg(QFileInfo(wavPath).fileName()));
    audio.setBodyDevice(file.get());
    file.release()->setParent(multi);
    multi->append(audio);

    QNetworkAccessManager nam;
    QNetworkReply *reply = nam.post(request(config, QStringLiteral("/v1/speech-to-text")), multi);
    multi->setParent(reply);
    const HttpResult r = run(reply, timeoutMs);
    if (r.status != 200) {
        if (error)
            *error = describe(r, "speech-to-text");
        return std::nullopt;
    }
    const QJsonObject obj = QJsonDocument::fromJson(r.body).object();
    if (!obj.contains(QStringLiteral("text"))) {
        if (error)
            *error = QStringLiteral("ElevenLabs speech-to-text: unexpected reply %1").arg(QString::fromUtf8(r.body.left(200)));
        return std::nullopt;
    }
    Transcript t;
    t.text = obj.value(QStringLiteral("text")).toString().trimmed();
    t.languageCode = obj.value(QStringLiteral("language_code")).toString();
    t.languageProbability = obj.value(QStringLiteral("language_probability")).toDouble();
    t.words = obj.value(QStringLiteral("words")).toArray();
    return t;
}

std::optional<QByteArray> synthesize(const Config &config, const QString &text, QString *error, int timeoutMs)
{
    if (text.trimmed().isEmpty()) {
        if (error)
            *error = QStringLiteral("no text to speak");
        return std::nullopt;
    }
    QNetworkRequest req = request(config, QStringLiteral("/v1/text-to-speech/%1").arg(QString::fromUtf8(QUrl::toPercentEncoding(config.voiceId))));
    QUrl url = req.url();
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("output_format"), QStringLiteral("pcm_%1").arg(kTtsSampleRate));
    url.setQuery(query);
    req.setUrl(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    const QJsonObject body{{QStringLiteral("text"), text}, {QStringLiteral("model_id"), config.ttsModel}};

    QNetworkAccessManager nam;
    const HttpResult r = run(nam.post(req, QJsonDocument(body).toJson(QJsonDocument::Compact)), timeoutMs);
    if (r.status != 200) {
        if (error)
            *error = describe(r, "text-to-speech");
        return std::nullopt;
    }
    if (r.body.isEmpty() || r.body.size() % 2) {
        if (error)
            *error = QStringLiteral("ElevenLabs text-to-speech: unexpected audio (%1 bytes)").arg(r.body.size());
        return std::nullopt;
    }
    return r.body;
}

} // namespace leansip::elevenlabs
