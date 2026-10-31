#pragma once

#include <QString>

namespace leansip {

// Where the per-baresip-directory runtime state lives. The GUI, leansipd and sipctl all derive
// the same paths from the baresip directory, so they meet without any configuration.
namespace profile {

QString defaultBaresipDir(); // ~/.baresip
// $XDG_RUNTIME_DIR/leansip<suffix>: leansipd's derived baresip config, socket and log (0700).
QString runtimeDir(const QString &baresipDir);
QString socketPath(const QString &baresipDir);
QString logPath(const QString &baresipDir);

} // namespace profile
} // namespace leansip
