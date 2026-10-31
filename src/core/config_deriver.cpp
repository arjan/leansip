#include "config_deriver.h"

#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QStringList>
#include <QTextStream>

namespace leansip::config_deriver {

namespace {

constexpr QLatin1StringView kDefaultAudioPath("/usr/share/baresip");

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

const QRegularExpression &sndPathLine()
{
    static const QRegularExpression re(QStringLiteral(R"(^\s*snd_path\b)"));
    return re;
}

const QRegularExpression &audioPathLine()
{
    static const QRegularExpression re(QStringLiteral(R"(^\s*audio_path\s+(\S+))"));
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

bool linkInto(const QString &src, const QString &dst)
{
    if (QFile::exists(dst) || QFileInfo(dst).isSymLink())
        QFile::remove(dst);
    return QFile::link(src, dst);
}

} // namespace

QString userAudioPath(const QString &userConfig)
{
    const QStringList lines = userConfig.split(u'\n');
    for (const QString &line : lines) {
        const auto m = audioPathLine().match(line);
        if (m.hasMatch())
            return m.captured(1);
    }
    return QString(kDefaultAudioPath);
}

QString deriveConfigText(const QString &userConfig, const QString &listen, const QString &audioPath,
                         const Options &options)
{
    QStringList out;
    const QStringList lines = userConfig.split(u'\n');
    for (const QString &line : lines) {
        if (droppedModuleLine().match(line).hasMatch())
            continue;
        if (ctrlTcpListenLine().match(line).hasMatch())
            continue;
        if (audioPathLine().match(line).hasMatch())
            continue;
        if (!options.recordDir.isEmpty() && sndPathLine().match(line).hasMatch())
            continue;
        out.append(line);
    }
    // Drop a trailing empty element produced by a final newline so we control spacing.
    while (!out.isEmpty() && out.last().trimmed().isEmpty())
        out.removeLast();

    out.append(QString());
    out.append(QStringLiteral("# --- added by leansip ---"));
    // account.so loads the accounts file; menu.so provides dial/hangup/accept;
    // debug_cmd.so provides `play` for our DTMF tones.
    if (!hasActiveModule(out, QStringLiteral("account")))
        out.append(QStringLiteral("module_app\t\taccount.so"));
    if (!hasActiveModule(out, QStringLiteral("menu")))
        out.append(QStringLiteral("module_app\t\tmenu.so"));
    if (!hasActiveModule(out, QStringLiteral("debug_cmd")))
        out.append(QStringLiteral("module_app\t\tdebug_cmd.so"));
    if (!hasActiveModule(out, QStringLiteral("aufile")))
        out.append(QStringLiteral("module\t\t\taufile.so"));
    if (!options.recordDir.isEmpty()) {
        if (!hasActiveModule(out, QStringLiteral("sndfile")))
            out.append(QStringLiteral("module\t\t\tsndfile.so"));
        out.append(QStringLiteral("snd_path\t\t%1").arg(options.recordDir));
    }
    out.append(QStringLiteral("module_app\t\tctrl_tcp.so"));
    out.append(QStringLiteral("ctrl_tcp_listen\t\t%1").arg(listen));
    out.append(QStringLiteral("audio_path\t\t%1").arg(audioPath));
    out.append(QString());
    return out.join(u'\n');
}

Result deriveDirectory(const QString &userDir, const QString &targetDir, const QString &listen, bool linkAccounts,
                       const Options &options)
{
    const QDir user(userDir);
    if (!user.exists())
        return {false, {}, {}, QStringLiteral("baresip directory %1 does not exist").arg(userDir)};

    QFile userConfig(user.filePath(QStringLiteral("config")));
    if (!userConfig.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {false, {}, {},
                QStringLiteral("cannot read %1 (run baresip once to create it): %2")
                    .arg(userConfig.fileName(), userConfig.errorString())};
    }
    const QString text = QTextStream(&userConfig).readAll();

    QDir target(targetDir);
    const QString soundsDir = target.filePath(QStringLiteral("sounds"));
    if (!QDir().mkpath(soundsDir))
        return {false, {}, {}, QStringLiteral("cannot create %1").arg(soundsDir)};
    QFile::setPermissions(targetDir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);

    QFile derived(target.filePath(QStringLiteral("config")));
    if (!derived.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return {false, {}, {}, QStringLiteral("cannot write %1: %2").arg(derived.fileName(), derived.errorString())};
    QTextStream(&derived) << deriveConfigText(text, listen, soundsDir, options);
    derived.close();

    // Symlink (never copy) the files holding credentials and identity.
    for (const char *name : {"accounts", "contacts", "uuid"}) {
        const QString src = user.filePath(QLatin1String(name));
        const QString dst = target.filePath(QLatin1String(name));
        if (!QFile::exists(src))
            continue;
        if (!linkAccounts && QLatin1String(name) == QLatin1String("accounts"))
            continue; // a candidate accounts file is in place; keep it
        if (!linkInto(src, dst))
            return {false, {}, {}, QStringLiteral("cannot link %1 -> %2").arg(dst, src)};
    }

    // baresip resolves every sound (ring, ringback, our tones) against one audio_path, so the
    // stock files must appear in ours too.
    const QDir stock(userAudioPath(text));
    const QStringList wavs = stock.entryList({QStringLiteral("*.wav")}, QDir::Files);
    for (const QString &wav : wavs) {
        if (!linkInto(stock.filePath(wav), QDir(soundsDir).filePath(wav)))
            return {false, {}, {}, QStringLiteral("cannot link sound %1").arg(wav)};
    }

    Result result;
    result.ok = true;
    result.dir = targetDir;
    result.soundsDir = soundsDir;
    return result;
}

} // namespace leansip::config_deriver
