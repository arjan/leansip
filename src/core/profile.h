#pragma once

#include <QString>

namespace leansip {

// Where a profile's runtime state lives. The GUI, leansipd and leansipctl all derive the same paths
// from the profile name, so they meet without any further configuration.
namespace profile {

// $XDG_RUNTIME_DIR/leansip-<name>: leansipd's baresip config, socket, log and media (0700).
QString runtimeDir(const QString &name);
QString socketPath(const QString &name);
QString logPath(const QString &name);

} // namespace profile
} // namespace leansip
