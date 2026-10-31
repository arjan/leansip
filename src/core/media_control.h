#pragma once

#include <QDir>
#include <QHash>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>

#include <optional>

namespace leansip {

// What baresip's `audio_debug` says about the current call's audio stream.
struct StreamFormat {
    QString codec;      // "opus", "G722", "PCMA", ...
    int sampleRate = 0; // of the encoder: what a source must deliver
    int channels = 0;
    QString source;     // current audio source, "module,device" as `ausrc` takes it
};
std::optional<StreamFormat> parseAudioDebug(const QString &text);

// Which destinations a non-dialer client (sipctl, an agent) may call or transfer to.
// Loopback destinations are always allowed. Other ones must match a pattern from
// `<baresip dir>/sipctl-allow`: one per line, `#` comments, `*` matches any run of characters,
// compared against the number/user part and against the whole target without its scheme.
class CallPolicy {
public:
    static CallPolicy load(const QString &allowFile);
    void setPatterns(const QStringList &patterns);
    QStringList patterns() const { return m_patterns; }

    // Empty when allowed, otherwise the reason.
    QString check(const QString &target) const;
    // Commands that place or redirect a call, and the target taken from their params.
    static std::optional<QString> dialTarget(const QString &command, const QString &params);
    static bool isLoopbackTarget(const QString &target);

private:
    QStringList m_patterns;
    QString m_file;
};

// Maps sndfile's per-call dumps ("dump-<time>-enc"/"-dec" in the incoming directory) to call
// ids and moves them to "<recordings>/<call id>-sent.wav" and "-received.wav". baresip keeps
// writing through its open file descriptors after the rename.
class RecordingTracker {
public:
    RecordingTracker(QString incomingDir, QString recordingsDir);

    struct Files {
        QString sent;
        QString received;
    };
    // Called for each call event; claims a new pair of dumps for a call that has none yet.
    // Returns the files when this call just got them.
    std::optional<Files> onCallEvent(const QString &callId);
    std::optional<Files> filesFor(const QString &callId) const;
    QJsonObject toJson() const;
    QString incomingDir() const { return m_incoming; }
    QString recordingsDir() const { return m_recordings; }

private:
    QString m_incoming;
    QString m_recordings;
    QHash<QString, Files> m_byCall;
};

} // namespace leansip
