#include "config_deriver.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringList>

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

// baresip will not start when sip_cafile names a file that does not exist (no ca-certificates).
const QRegularExpression &caFileLine()
{
    static const QRegularExpression re(QStringLiteral(R"(^\s*sip_cafile\s+(\S+))"));
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
        if (const auto ca = caFileLine().match(line); ca.hasMatch() && !QFileInfo::exists(ca.captured(1)))
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

Result writeDirectory(const Setup &setup, const QString &targetDir, const QString &listen, const Options &options)
{
    auto fail = [](const QString &why) { return Result{false, {}, {}, why}; };
    const QDir target(targetDir);
    const QString soundsDir = target.filePath(QStringLiteral("sounds"));
    if (!QDir().mkpath(soundsDir))
        return fail(QStringLiteral("cannot create %1").arg(soundsDir));
    QFile::setPermissions(targetDir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);

    auto write = [&](const QString &name, const QString &text) {
        // Never write through a link left behind (an older layout linked the user's own files).
        QFile::remove(target.filePath(name));
        QFile f(target.filePath(name));
        // Private before the first byte goes in: the accounts file holds the password.
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)
            || !f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) || f.write(text.toUtf8()) < 0) {
            return QStringLiteral("cannot write %1: %2").arg(f.fileName(), f.errorString());
        }
        return QString();
    };
    QString error = write(QStringLiteral("config"), deriveConfigText(setup.config, listen, soundsDir, options));
    if (error.isEmpty())
        error = write(QStringLiteral("accounts"), setup.accountLine + u'\n');
    if (error.isEmpty() && !setup.uuid.isEmpty())
        error = write(QStringLiteral("uuid"), setup.uuid + u'\n');
    if (!error.isEmpty())
        return fail(error);

    // baresip resolves every sound (ring, ringback, our tones) against one audio_path, so the
    // stock files must appear in ours too.
    const QDir stock(userAudioPath(setup.config));
    const QStringList wavs = stock.entryList({QStringLiteral("*.wav")}, QDir::Files);
    for (const QString &wav : wavs) {
        if (!linkInto(stock.filePath(wav), QDir(soundsDir).filePath(wav)))
            return fail(QStringLiteral("cannot link sound %1").arg(wav));
    }

    Result result;
    result.ok = true;
    result.dir = targetDir;
    result.soundsDir = soundsDir;
    return result;
}

} // namespace leansip::config_deriver
