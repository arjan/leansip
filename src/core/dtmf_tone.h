#pragma once

#include <QByteArray>
#include <QChar>
#include <QString>
#include <optional>
#include <utility>

namespace leansip::dtmf {

// Row/column frequencies (Hz) for a DTMF key, or nullopt for a non-DTMF character.
std::optional<std::pair<int, int>> frequencies(QChar key);

// Signed 16-bit little-endian mono PCM of the dual tone, with a short fade in/out to avoid clicks.
// Returns an empty array for a non-DTMF key.
QByteArray samples(QChar key, int durationMs, int sampleRate);

// Minimal RIFF/WAVE container around 16-bit mono PCM.
QByteArray wav(const QByteArray &pcm, int sampleRate);

// File name baresip is asked to play for a key, relative to its audio_path: "dtmf-5.wav",
// "dtmf-star.wav", "dtmf-hash.wav". Empty for a non-DTMF key.
QString soundFileName(QChar key);

// Writes one WAV per DTMF key into `dir` (created if needed). Returns false on the first I/O error.
bool installSoundBank(const QString &dir, int durationMs = 400, int sampleRate = 8000);

} // namespace leansip::dtmf
