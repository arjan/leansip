#pragma once

#include <QString>

namespace leansip {

// Produces the private baresip config directory for the child process: the profile's config
// with console/GTK UI modules removed, ctrl_tcp bound to loopback and audio_path pointing at our
// own sounds directory (baresip's stock WAVs symlinked in, our generated DTMF tones added), and
// the profile's account.
namespace config_deriver {

struct Options {
    // Non-empty: load sndfile.so and record every call, both directions, into this directory.
    QString recordDir;
};

// Pure transformation of the config file text. `listen` is e.g. "127.0.0.1:41234",
// `audioPath` the directory baresip should resolve `play <file>` and ring tones against.
// aufile.so is always loaded: leansipd plays clips into calls with `ausrc aufile,<wav>`.
QString deriveConfigText(const QString &userConfig, const QString &listen, const QString &audioPath,
                         const Options &options = {});

// The audio_path the user configured, or baresip's default when the line is absent/commented.
QString userAudioPath(const QString &userConfig);

struct Result {
    bool ok = false;
    QString dir;       // the directory to pass as `baresip -f <dir>`
    QString soundsDir; // where DTMF WAVs should be written (== audio_path in the derived config)
    QString error;
};

// What baresip runs with: the config text (before deriveConfigText), the `accounts` line, and
// optionally the instance id for the `uuid` file.
struct Setup {
    QString config;
    QString accountLine;
    QString uuid;
};

// Empty when `setup` is safe to run; otherwise why not. An account that uses TLS (sips: or
// transport=tls, also on its outbound proxy) needs an existing sip_cafile or sip_capath:
// without one baresip accepts any server certificate.
QString tlsProblem(const Setup &setup);

// Fails with tlsProblem() when there is one. Writes config, accounts (0600) and uuid into `targetDir` (created 0700 if missing) and links
// baresip's stock sounds into its sounds directory.
Result writeDirectory(const Setup &setup, const QString &targetDir, const QString &listen, const Options &options = {});

} // namespace config_deriver
} // namespace leansip
