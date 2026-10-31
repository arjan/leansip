#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QString>

#include <optional>

// ElevenLabs speech-to-text and text-to-speech, the only speech services leansip uses.
// Blocking calls (they run a local event loop); meant for sipctl, not for the GUI thread.
namespace leansip::elevenlabs {

struct Config {
    QString apiKey;  // ELEVENLABS_API_KEY (required)
    QString baseUrl; // ELEVENLABS_BASE_URL, default https://api.elevenlabs.io
    QString sttModel = QStringLiteral("scribe_v2");             // ELEVENLABS_STT_MODEL
    QString ttsModel = QStringLiteral("eleven_multilingual_v2"); // ELEVENLABS_TTS_MODEL
    QString voiceId = QStringLiteral("JBFqnCBsd6RMkjVDRZzb");   // ELEVENLABS_VOICE_ID

    // Fails without an API key. The key is only ever sent to baseUrl, which must be https
    // unless it points at the loopback interface (tests).
    static std::optional<Config> fromEnvironment(QString *error);
};

struct Transcript {
    QString text;
    QString languageCode;
    double languageProbability = 0;
    QJsonArray words;
};

std::optional<Transcript> transcribe(const Config &config, const QString &wavPath, QString *error,
                                     const QString &languageCode = {}, int timeoutMs = 120000);

// Speech as 16 kHz mono 16-bit PCM (ElevenLabs output_format pcm_16000).
constexpr int kTtsSampleRate = 16000;
std::optional<QByteArray> synthesize(const Config &config, const QString &text, QString *error,
                                     int timeoutMs = 120000);

} // namespace leansip::elevenlabs
