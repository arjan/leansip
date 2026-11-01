#include "profile.h"

#include "baresip_process.h"

#include <QDir>

namespace leansip::profile {

QString runtimeDir(const QString &name)
{
    return BaresipProcess::defaultDerivedDir() + u'-' + name;
}

QString socketPath(const QString &name)
{
    return QDir(runtimeDir(name)).filePath(QStringLiteral("leansipd.sock"));
}

QString logPath(const QString &name)
{
    return QDir(runtimeDir(name)).filePath(QStringLiteral("leansipd.log"));
}

} // namespace leansip::profile
