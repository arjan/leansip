#pragma once

#include <QString>

namespace leansip::baresip_binary {

// The baresip leansipd runs, and the directory with its stock sounds (ring.wav, busy.wav, ...).
struct Location {
    QString program;   // absolute path, or a name to look up in PATH
    QString soundsDir; // empty: the default, /usr/share/baresip
    QString error;     // non-empty: no usable baresip
};

// Where the embedded copy lives in the executable's Qt resources: `baresip` and `sounds/*.wav`.
// Only leansipd carries it, and only in a build configured with LEANSIP_EMBED_BARESIP_DIR.
inline constexpr char kEmbeddedPrefix[] = ":/leansip/baresip";

// In this order:
//   1. $LEANSIP_BARESIP, a path or a name in PATH;
//   2. the copy embedded in this executable, extracted on first use (extractEmbedded);
//   3. the one the build was configured with (LEANSIP_BARESIP_PATH, e.g. Homebrew's libexec);
//   4. `baresip` in PATH.
Location locate();

// Writes the embedded baresip and its sounds under `cacheRoot`, in a directory named after a
// hash of the binary so that an upgrade gets its own copy, and returns where they are. A copy
// already there is checked against the hash and rewritten when it differs, so a truncated or
// modified file is never run. `resourcePrefix` is kEmbeddedPrefix outside tests.
Location extractEmbedded(const QString &resourcePrefix, const QString &cacheRoot);

// $XDG_CACHE_HOME/leansip (Linux) or ~/Library/Caches/leansip (macOS).
QString defaultCacheRoot();

} // namespace leansip::baresip_binary
