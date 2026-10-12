#include "config_deriver.h"

#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QStringList>
#include <QTextStream>

namespace leansip::config_deriver {

namespace {

// UI/control modules that would fight with (or expose) our ctrl_tcp link.
const QRegularExpression &droppedModuleLine()
{
    static const QRegularExpression re(
        QStringLiteral(R"(^\s*module(?:_app)?\s+(?:stdio|cons|gtk|evdev|wincons|ctrl_tcp|ctrl_dbus|httpd)\.so\b)"));
    return re;
}

const QRegularExpression &ctrlTcpListenLine()
{
    static const QRegularExpression re(QStringLiteral(R"(^\s*ctrl_tcp_listen\b)"));
    return re;
}

bool hasActiveModule(const QStringList &lines, const QString &module)
{
    const QRegularExpression re(
        QStringLiteral(R"(^\s*module(?:_app)?\s+%1\.so\b)").arg(QRegularExpression::escape(module)));
    for (const QString &line : lines) {
        if (re.match(line).hasMatch())
            return true;
    }
    return false;
}

} // namespace

QString deriveConfigText(const QString &userConfig, const QString &listen)
{
    QStringList out;
    const QStringList lines = userConfig.split(u'\n');
    for (const QString &line : lines) {
        if (droppedModuleLine().match(line).hasMatch())
            continue;
        if (ctrlTcpListenLine().match(line).hasMatch())
            continue;
        out.append(line);
    }
    // Drop a trailing empty element produced by a final newline so we control spacing.
    while (!out.isEmpty() && out.last().trimmed().isEmpty())
        out.removeLast();

    out.append(QString());
    out.append(QStringLiteral("# --- added by leansip ---"));
    // account.so loads the accounts file; menu.so provides dial/hangup/accept commands.
    if (!hasActiveModule(out, QStringLiteral("account")))
        out.append(QStringLiteral("module_app\t\taccount.so"));
    if (!hasActiveModule(out, QStringLiteral("menu")))
        out.append(QStringLiteral("module_app\t\tmenu.so"));
    out.append(QStringLiteral("module_app\t\tctrl_tcp.so"));
    out.append(QStringLiteral("ctrl_tcp_listen\t\t%1").arg(listen));
    out.append(QString());
    return out.join(u'\n');
}

Result deriveDirectory(const QString &userDir, const QString &targetDir, const QString &listen)
{
    Result result;
    const QDir user(userDir);
    if (!user.exists())
        return {false, {}, QStringLiteral("baresip directory %1 does not exist").arg(userDir)};

    QFile userConfig(user.filePath(QStringLiteral("config")));
    if (!userConfig.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {false, {},
                QStringLiteral("cannot read %1 (run baresip once to create it): %2")
                    .arg(userConfig.fileName(), userConfig.errorString())};
    }
    const QString text = QTextStream(&userConfig).readAll();

    QDir target(targetDir);
    if (!target.exists() && !QDir().mkpath(targetDir))
        return {false, {}, QStringLiteral("cannot create %1").arg(targetDir)};
    QFile::setPermissions(targetDir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);

    QFile derived(target.filePath(QStringLiteral("config")));
    if (!derived.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return {false, {}, QStringLiteral("cannot write %1: %2").arg(derived.fileName(), derived.errorString())};
    QTextStream(&derived) << deriveConfigText(text, listen);
    derived.close();

    // Symlink (never copy) the files holding credentials and identity.
    for (const char *name : {"accounts", "contacts", "uuid"}) {
        const QString src = user.filePath(QLatin1String(name));
        const QString dst = target.filePath(QLatin1String(name));
        if (!QFile::exists(src))
            continue;
        if (QFile::exists(dst) || QFileInfo(dst).isSymLink())
            QFile::remove(dst);
        if (!QFile::link(src, dst))
            return {false, {}, QStringLiteral("cannot link %1 -> %2").arg(dst, src)};
    }

    result.ok = true;
    result.dir = targetDir;
    return result;
}

} // namespace leansip::config_deriver
