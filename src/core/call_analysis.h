#pragma once

#include "wav.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <optional>

namespace leansip::analysis {

// The id of the most recent call in a journal ("" when there is none).
QString lastCallId(const QJsonArray &events);

// Summary of one call from leansipd's event journal: timing (from the first event of the call
// to ringing, answer and close), close reason, DTMF received, RTCP statistics, the stream
// format leansipd recorded (CALL_AUDIO) and, when given, the levels of both recordings.
// `warnings` lists likely problems such as one-way audio.
QJsonObject analyzeCall(const QJsonArray &events, const QString &callId,
                        const std::optional<wav::Levels> &sent = std::nullopt,
                        const std::optional<wav::Levels> &received = std::nullopt);

QJsonObject levelsToJson(const wav::Levels &levels);

} // namespace leansip::analysis
