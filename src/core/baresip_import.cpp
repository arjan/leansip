#include "baresip_import.h"

#include "accounts_parser.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryDir>

#include <algorithm>

namespace leansip::baresip_import {

namespace {

// leansipd writes these itself, or they point into baresip's own install.
const QSet<QString> &managedKeys()
{
    static const QSet<QString> keys{
        QStringLiteral("module_path"),   QStringLiteral("module_app"),  QStringLiteral("module_tmp"),
        QStringLiteral("ctrl_tcp_listen"), QStringLiteral("audio_path"), QStringLiteral("snd_path"),
        QStringLiteral("poll_method"),   QStringLiteral("sip_cafile"),  QStringLiteral("sip_capath"),
    };
    return keys;
}

// Account parameters at the value baresip uses when they are absent.
bool isDefaultAccountParam(const QString &key, const QString &value)
{
    static const QMap<QString, QString> defaults{
        {QStringLiteral("answermode"), QStringLiteral("manual")},
        {QStringLiteral("regint"), QStringLiteral("3600")},
        {QStringLiteral("pubint"), QStringLiteral("0")},
        {QStringLiteral("ptime"), QStringLiteral("20")},
    };
    return defaults.contains(key) && defaults.value(key) == value;
}

// The `module` lines without the modules leansipd removes (user interfaces, other control
// interfaces) or adds itself (aufile, sndfile), so they do not count as a difference.
ConfigLines forComparison(ConfigLines lines)
{
    static const QSet<QString> handled{
        QStringLiteral("stdio.so"),    QStringLiteral("cons.so"),     QStringLiteral("gtk.so"),
        QStringLiteral("evdev.so"),    QStringLiteral("wincons.so"),  QStringLiteral("ctrl_tcp.so"),
        QStringLiteral("ctrl_dbus.so"), QStringLiteral("httpd.so"),   QStringLiteral("aufile.so"),
        QStringLiteral("sndfile.so"),
    };
    if (auto it = lines.find(QStringLiteral("module")); it != lines.end())
        it->removeIf([](const QString &m) { return handled.contains(m); });
    return lines;
}

bool sameValues(const QString &key, QStringList a, QStringList b)
{
    if (key == QLatin1String("module")) { // load order does not matter to baresip
        a.sort();
        b.sort();
    }
    return a == b;
}

// ";a=1;b=\"x;y\"" -> {a: 1, b: x;y}, in order. A parameter without '=' gets an empty value.
QList<QPair<QString, QString>> splitParams(const QString &params)
{
    QList<QPair<QString, QString>> out;
    QString current;
    bool quoted = false;
    auto flush = [&] {
        const QString item = current.trimmed();
        current.clear();
        if (item.isEmpty())
            return;
        const qsizetype eq = item.indexOf(u'=');
        if (eq < 0) {
            out.append({item, QString()});
            return;
        }
        QString value = item.mid(eq + 1).trimmed();
        if (value.size() >= 2 && value.startsWith(u'"') && value.endsWith(u'"'))
            value = value.mid(1, value.size() - 2);
        out.append({item.left(eq).trimmed(), value});
    };
    for (const QChar c : params) {
        if (c == u'"')
            quoted = !quoted;
        if (c == u';' && !quoted)
            flush();
        else
            current += c;
    }
    flush();
    return out;
}

QString tomlString(const QString &s)
{
    QString out = s;
    out.replace(u'\\', QStringLiteral("\\\\"));
    out.replace(u'"', QStringLiteral("\\\""));
    return u'"' + out + u'"';
}

QString tomlKey(const QString &k)
{
    static const QRegularExpression bare(QStringLiteral("^[A-Za-z0-9_-]+$"));
    return bare.match(k).hasMatch() ? k : tomlString(k);
}

QString home(const QString &path, const QString &homeDir)
{
    return path.startsWith(homeDir + u'/') ? u'~' + path.mid(homeDir.size()) : path;
}

} // namespace

ConfigLines parseConfig(const QString &text)
{
    static const QRegularExpression trailingComment(QStringLiteral("\\s+#.*$"));
    static const QRegularExpression ws(QStringLiteral("\\s+"));
    ConfigLines out;
    for (QString line : text.split(u'\n')) {
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith(u'#'))
            continue;
        line.remove(trailingComment);
        const qsizetype sep = line.indexOf(ws);
        if (sep < 0)
            continue; // a key without a value sets nothing
        out[line.left(sep)].append(line.mid(sep).trimmed().replace(ws, QStringLiteral(" ")));
    }
    return out;
}

QString baresipDefaultConfig(const QString &binary, int timeoutMs)
{
    QTemporaryDir dir;
    if (!dir.isValid())
        return {};
    QProcess p;
    p.setProgram(binary);
    p.setArguments({QStringLiteral("-f"), dir.path(), QStringLiteral("-t"), QStringLiteral("1")});
    p.setStandardInputFile(QProcess::nullDevice());
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start();
    if (!p.waitForStarted(timeoutMs))
        return {};
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();
        p.waitForFinished(1000);
    }
    QFile f(dir.filePath(QStringLiteral("config")));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(f.readAll());
}

Plan discover(const QString &homeDir, const QString &baresipDefaults)
{
    Plan plan;
    const ConfigLines leansipTemplate = forComparison(parseConfig(settings::baresipTemplate()));
    const ConfigLines baresipDefault = forComparison(parseConfig(baresipDefaults));
    QSet<QString> usedNames;

    static const QRegularExpression dirRe(QStringLiteral("^\\.baresip[A-Za-z0-9_.-]*$"));
    QStringList dirs = QDir(homeDir).entryList(QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot, QDir::Name);
    dirs.erase(std::remove_if(dirs.begin(), dirs.end(), [](const QString &d) { return !dirRe.match(d).hasMatch(); }),
               dirs.end());

    for (const QString &dirName : dirs) {
        const QString dirPath = QDir(homeDir).filePath(dirName);
        const QString shown = home(dirPath, homeDir);
        QFile accounts(QDir(dirPath).filePath(QStringLiteral("accounts")));
        if (!accounts.open(QIODevice::ReadOnly))
            continue; // not a baresip config directory

        // Settings from this directory's config that are the user's own choice.
        settings::BaresipSettings overrides;
        int leftOut = 0;
        QFile configFile(QDir(dirPath).filePath(QStringLiteral("config")));
        if (configFile.open(QIODevice::ReadOnly)) {
            const ConfigLines user = forComparison(parseConfig(QString::fromUtf8(configFile.readAll())));
            for (auto it = user.cbegin(); it != user.cend(); ++it) {
                // leansip is audio only: video settings do nothing for it.
                if (managedKeys().contains(it.key()) || it.key().contains(QLatin1String("video"))
                    || (leansipTemplate.contains(it.key()) && sameValues(it.key(), it.value(), leansipTemplate.value(it.key())))
                    || (baresipDefault.contains(it.key()) && sameValues(it.key(), it.value(), baresipDefault.value(it.key())))) {
                    ++leftOut;
                    continue;
                }
                overrides.insert(it.key(), it.value());
            }
        }

        const QString baseName = dirName.mid(1); // ".baresip-work" -> "baresip-work"
        int lineNo = 0;
        int imported = 0;
        const qsizetype skippedBefore = plan.skipped.size();
        for (const QString &raw : QString::fromUtf8(accounts.readAll()).split(u'\n')) {
            ++lineNo;
            const QString line = raw.trimmed();
            if (line.isEmpty() || line.startsWith(u'#'))
                continue;
            const auto record = AccountRecord::fromLine(line);
            if (!record) {
                plan.skipped << QStringLiteral("%1/accounts line %2: not a SIP account line").arg(shown).arg(lineNo);
                continue;
            }
            QString name = baseName;
            for (int n = 2; usedNames.contains(name); ++n)
                name = QStringLiteral("%1-%2").arg(baseName).arg(n);
            if (!settings::isValidName(name)) {
                plan.skipped << QStringLiteral("%1: \"%2\" cannot be a profile name").arg(shown, name);
                break;
            }

            ImportedProfile ip;
            ip.sourceDir = shown;
            settings::Profile &p = ip.profile;
            p.name = name;
            p.displayName = record->displayName;
            p.aor = record->aor();
            p.transport = record->transport;
            p.authUser = record->authUser == record->user ? QString() : record->authUser;
            p.password = record->password;
            p.outbound = record->outbound;
            if (p.outbound.size() >= 2 && p.outbound.startsWith(u'"') && p.outbound.endsWith(u'"'))
                p.outbound = p.outbound.mid(1, p.outbound.size() - 2); // ;outbound="sip:..." is baresip's own example
            p.baresip = overrides;
            static const QRegularExpression keyRe(QStringLiteral("^[a-z][a-z0-9_]*$"));
            static const QRegularExpression forbidden(QStringLiteral(R"([\s;<>"])"));
            for (const auto &[key, value] : splitParams(record->extraParams)) {
                if (isDefaultAccountParam(key, value))
                    continue;
                if (value.isEmpty() || !keyRe.match(key).hasMatch() || value.contains(forbidden)) {
                    ip.notes << QStringLiteral("left out account parameter \"%1\": leansip cannot express it").arg(key);
                    continue;
                }
                p.accountParams.insert(key, value);
            }
            if (leftOut > 0)
                ip.notes << QStringLiteral("%1 config setting(s) left out: baresip/leansip defaults or managed by leansip")
                                .arg(leftOut);

            // A profile only goes in when leansip would load it.
            Plan single;
            single.profiles << ip;
            QString why;
            if (!settings::parse(toToml(single), &why)) {
                plan.skipped << QStringLiteral("%1/accounts line %2: %3").arg(shown).arg(lineNo).arg(why);
                continue;
            }
            usedNames.insert(name);
            plan.profiles << ip;
            ++imported;
        }
        if (imported == 0 && plan.skipped.size() == skippedBefore)
            plan.skipped << QStringLiteral("%1: no account in its accounts file").arg(shown);
    }

    for (const ImportedProfile &ip : std::as_const(plan.profiles)) {
        if (ip.profile.name == QLatin1String("baresip"))
            plan.defaultProfile = ip.profile.name;
    }
    if (plan.defaultProfile.isEmpty() && !plan.profiles.isEmpty())
        plan.defaultProfile = plan.profiles.first().profile.name;
    return plan;
}

QString toToml(const Plan &plan)
{
    QStringList out;
    out << QStringLiteral("# leansip profiles, imported from ~/.baresip* on %1. See \"Configuration\" in")
               .arg(QDate::currentDate().toString(Qt::ISODate))
        << QStringLiteral("# the leansip README. This file holds passwords: keep it chmod 600.");
    if (!plan.defaultProfile.isEmpty())
        out << QString() << QStringLiteral("default_profile = %1").arg(tomlString(plan.defaultProfile));

    for (const ImportedProfile &ip : plan.profiles) {
        const settings::Profile &p = ip.profile;
        const QString table = QStringLiteral("profiles.%1").arg(tomlKey(p.name));
        out << QString() << QStringLiteral("# from %1/accounts").arg(ip.sourceDir) << QStringLiteral("[%1]").arg(table);
        auto field = [&](const char *key, const QString &value) {
            if (!value.isEmpty())
                out << QStringLiteral("%1 = %2").arg(QLatin1String(key), tomlString(value));
        };
        field("display_name", p.displayName);
        field("aor", p.aor);
        field("transport", p.transport);
        field("auth_user", p.authUser);
        field("password", p.password);
        field("outbound", p.outbound);
        if (!p.accountParams.isEmpty()) {
            QStringList kv;
            for (auto it = p.accountParams.cbegin(); it != p.accountParams.cend(); ++it)
                kv << QStringLiteral("%1 = %2").arg(it.key(), tomlString(it.value()));
            out << QStringLiteral("account_params = { %1 }").arg(kv.join(QStringLiteral(", ")));
        }
        if (!p.baresip.isEmpty()) {
            out << QString() << QStringLiteral("[%1.baresip]").arg(table);
            for (auto it = p.baresip.cbegin(); it != p.baresip.cend(); ++it) {
                if (it.value().size() == 1) {
                    out << QStringLiteral("%1 = %2").arg(it.key(), tomlString(it.value().first()));
                } else {
                    QStringList items;
                    for (const QString &v : it.value())
                        items << tomlString(v);
                    out << QStringLiteral("%1 = [%2]").arg(it.key(), items.join(QStringLiteral(", ")));
                }
            }
        }
    }
    out << QString();
    return out.join(u'\n');
}

QString summary(const Plan &plan)
{
    QStringList out;
    for (const ImportedProfile &ip : plan.profiles) {
        const settings::Profile &p = ip.profile;
        QString head = QStringLiteral("%1  (from %2)").arg(p.name, ip.sourceDir);
        if (p.name == plan.defaultProfile)
            head += QStringLiteral(", default");
        out << head;
        out << QStringLiteral("    %1%2").arg(p.displayName.isEmpty() ? QString() : QStringLiteral("\"%1\" ").arg(p.displayName), p.aor);
        QStringList facts;
        if (!p.transport.isEmpty())
            facts << p.transport;
        if (!p.authUser.isEmpty())
            facts << QStringLiteral("auth user %1").arg(p.authUser);
        if (!p.outbound.isEmpty())
            facts << QStringLiteral("via %1").arg(p.outbound);
        facts << (p.password.isEmpty() ? QStringLiteral("no password") : QStringLiteral("password saved in the file"));
        out << QStringLiteral("    %1").arg(facts.join(QStringLiteral(", ")));
        for (auto it = p.accountParams.cbegin(); it != p.accountParams.cend(); ++it)
            out << QStringLiteral("    %1=%2").arg(it.key(), it.value());
        for (auto it = p.baresip.cbegin(); it != p.baresip.cend(); ++it)
            out << QStringLiteral("    baresip: %1 %2").arg(it.key(), it.value().join(QStringLiteral(", ")));
        for (const QString &n : ip.notes)
            out << QStringLiteral("    (%1)").arg(n);
    }
    if (!plan.skipped.isEmpty()) {
        out << QString() << QStringLiteral("Not imported:");
        for (const QString &s : plan.skipped)
            out << QStringLiteral("    %1").arg(s);
    }
    return out.join(u'\n');
}

bool write(const QString &path, const QString &toml, QString *error)
{
    auto fail = [error](const QString &why) {
        if (error)
            *error = why;
        return false;
    };
    if (QFileInfo::exists(path) || QFileInfo(path).isSymLink())
        return fail(QStringLiteral("%1 already exists; not replacing it").arg(path));
    QString why;
    if (!settings::parse(toml, &why))
        return fail(QStringLiteral("the imported config does not parse (%1); nothing written").arg(why));
    const QString dirPath = QFileInfo(path).absolutePath();
    if (!QDir(dirPath).exists()) {
        if (!QDir().mkpath(dirPath)
            || !QFile::setPermissions(dirPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner))
            return fail(QStringLiteral("cannot create %1").arg(dirPath));
    }
    const QByteArray bytes = toml.toUtf8();
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly) || !out.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
        || out.write(bytes) != bytes.size() || !out.commit())
        return fail(QStringLiteral("cannot write %1: %2").arg(path, out.errorString()));
    return true;
}

} // namespace leansip::baresip_import
