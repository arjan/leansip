#pragma once

#include <QString>

namespace leansip {

// Produces a private baresip config directory for the child process:
// the user's `config` with console/GTK UI modules removed, ctrl_tcp bound to loopback and
// audio_path pointing at our own sounds directory (baresip's stock WAVs symlinked in, our
// generated DTMF tones added), plus symlinks to the user's accounts/contacts/uuid so
// credentials stay in one place.
namespace config_deriver {

// Pure transformation of the config file text. `listen` is e.g. "127.0.0.1:41234",
// `audioPath` the directory baresip should resolve `play <file>` and ring tones against.
QString deriveConfigText(const QString &userConfig, const QString &listen, const QString &audioPath);

// The audio_path the user configured, or baresip's default when the line is absent/commented.
QString userAudioPath(const QString &userConfig);

struct Result {
    bool ok = false;
    QString dir;       // the derived directory to pass as `baresip -f <dir>`
    QString soundsDir; // where DTMF WAVs should be written (== audio_path in the derived config)
    QString error;
};

// Writes the derived config into `targetDir` (created 0700 if missing).
// `userDir` is typically ~/.baresip; its `config` must exist (baresip creates it on first run).
// With `linkAccounts == false` an existing `accounts` file in `targetDir` is left alone (used
// while a candidate accounts file is being validated).
Result deriveDirectory(const QString &userDir, const QString &targetDir, const QString &listen,
                       bool linkAccounts = true);

} // namespace config_deriver
} // namespace leansip
