#pragma once

#include <QString>

namespace leansip {

// Produces a private baresip config directory for the child process:
// the user's `config` with console/GTK UI modules removed and ctrl_tcp bound to loopback,
// plus symlinks to the user's accounts/contacts/uuid so credentials stay in one place.
namespace config_deriver {

// Pure transformation of the config file text. `listen` is e.g. "127.0.0.1:41234".
QString deriveConfigText(const QString &userConfig, const QString &listen);

struct Result {
    bool ok = false;
    QString dir;   // the derived directory to pass as `baresip -f <dir>`
    QString error;
};

// Writes the derived config into `targetDir` (created 0700 if missing).
// `userDir` is typically ~/.baresip. A missing user config yields a minimal default.
Result deriveDirectory(const QString &userDir, const QString &targetDir, const QString &listen);

} // namespace config_deriver
} // namespace leansip
