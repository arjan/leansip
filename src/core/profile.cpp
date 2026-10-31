#include "profile.h"

#include "baresip_process.h"
#include "single_instance.h"

#include <QDir>

namespace leansip::profile {

QString defaultBaresipDir()
{
    return QDir::home().filePath(QStringLiteral(".baresip"));
}

QString runtimeDir(const QString &baresipDir)
{
    return BaresipProcess::defaultDerivedDir() + SingleInstance::profileSuffix(baresipDir);
}

QString socketPath(const QString &baresipDir)
{
    return QDir(runtimeDir(baresipDir)).filePath(QStringLiteral("leansipd.sock"));
}

QString logPath(const QString &baresipDir)
{
    return QDir(runtimeDir(baresipDir)).filePath(QStringLiteral("leansipd.log"));
}

} // namespace leansip::profile
