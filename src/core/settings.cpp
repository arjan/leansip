#include "settings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSaveFile>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUuid>

#include <csignal>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

// Header-only, errors as values. This is the only translation unit that includes toml++.
#define TOML_HEADER_ONLY 1
#define TOML_EXCEPTIONS 0
#if __has_include(<toml++/toml.hpp>)
#include <toml++/toml.hpp>
#else
#include <toml++/toml.h> // toml++ before 3.4 (Debian 12)
#endif

namespace leansip::settings {

namespace {

// What differs per platform in baresip's own defaults: the poll method (ignored by baresip 4),
// where the system keeps its CA bundle, and the sound system.
#ifdef Q_OS_MACOS
#define LEANSIP_POLL_METHOD "kqueue"
#define LEANSIP_CAFILE "/etc/ssl/cert.pem"
#define LEANSIP_AUDIO "coreaudio"
#else
#define LEANSIP_POLL_METHOD "epoll"
#define LEANSIP_CAFILE "/etc/ssl/certs/ca-certificates.crt"
#define LEANSIP_AUDIO "alsa"
#endif

// baresip's own template, trimmed to audio calls. leansipd adds ctrl_tcp, aufile and the
// account/menu modules it needs on top (config_deriver).
constexpr const char *kTemplate = "# Core\n"
                                  "poll_method\t\t" LEANSIP_POLL_METHOD "\n"
                                  "sip_cafile\t\t" LEANSIP_CAFILE "\n"
                                  R"(sip_tos			160

# Call
call_local_timeout	120
call_max_calls		4
call_hold_other_calls	yes

# Audio
)"
                                  "audio_player\t\t" LEANSIP_AUDIO ",default\n"
                                  "audio_source\t\t" LEANSIP_AUDIO ",default\n"
                                  "audio_alert\t\t" LEANSIP_AUDIO ",default\n"
                                  R"(ausrc_srate		48000
auplay_srate		48000
audio_level		no
ausrc_format		s16
auplay_format		s16
auenc_format		s16
audec_format		s16
audio_buffer		20-160
audio_buffer_mode	fixed
audio_silence		-35.0
audio_telev_pt		101

# RTP
rtp_tos			184
rtcp_mux		no
jitter_buffer_type	adaptive
jitter_buffer_delay	5-15
rtp_stats		no

# Modules
module_path		/usr/lib/baresip/modules
module			opus.so
module			g722.so
module			g711.so
)"
                                  "module\t\t\t" LEANSIP_AUDIO ".so\n"
                                  R"(module			uuid.so
module			stun.so
module			turn.so
module			ice.so
module_app		account.so
module_app		contact.so
module_app		debug_cmd.so
module_app		menu.so

# Opus
opus_bitrate		64000
opus_inbandfec		yes
opus_complexity		10
opus_application	audio
opus_samplerate		48000
)";

QString qs(std::string_view v)
{
    return QString::fromUtf8(v.data(), static_cast<qsizetype>(v.size()));
}

QString nodeTypeName(const toml::node &n)
{
    switch (n.type()) {
    case toml::node_type::table: return QStringLiteral("a table");
    case toml::node_type::array: return QStringLiteral("a list");
    case toml::node_type::string: return QStringLiteral("a string");
    case toml::node_type::integer: return QStringLiteral("a number");
    case toml::node_type::floating_point: return QStringLiteral("a number");
    case toml::node_type::boolean: return QStringLiteral("true/false");
    default: return QStringLiteral("a date or time");
    }
}

// Collects the first error with its location; every reader returns false once it is set.
struct Reader {
    QString *error;

    bool fail(const QString &where, const QString &why)
    {
        if (error)
            *error = where.isEmpty() ? why : QStringLiteral("%1: %2").arg(where, why);
        return false;
    }

    bool string(const toml::node &n, const QString &where, QString *out)
    {
        const auto *s = n.as_string();
        if (!s)
            return fail(where, QStringLiteral("expected a string, got %1").arg(nodeTypeName(n)));
        *out = qs(s->get());
        if (out->contains(u'\n') || out->contains(u'\r') || out->contains(QChar(0)))
            return fail(where, QStringLiteral("must be a single line"));
        return true;
    }

    bool boolean(const toml::node &n, const QString &where, bool *out)
    {
        const auto *b = n.as_boolean();
        if (!b)
            return fail(where, QStringLiteral("expected true or false, got %1").arg(nodeTypeName(n)));
        *out = b->get();
        return true;
    }

    // A string, number or boolean as baresip spells it in its config.
    bool scalar(const toml::node &n, const QString &where, QString *out)
    {
        if (const auto *i = n.as_integer()) {
            *out = QString::number(i->get());
            return true;
        }
        if (const auto *f = n.as_floating_point()) {
            *out = QString::number(f->get());
            return true;
        }
        if (const auto *b = n.as_boolean()) {
            *out = b->get() ? QStringLiteral("yes") : QStringLiteral("no");
            return true;
        }
        if (n.is_string())
            return string(n, where, out);
        return fail(where, QStringLiteral("expected a string, number or true/false, got %1").arg(nodeTypeName(n)));
    }

    bool baresip(const toml::node &n, const QString &where, BaresipSettings *out)
    {
        static const QRegularExpression keyRe(QStringLiteral("^[a-z][a-z0-9_]*$"));
        const auto *t = n.as_table();
        if (!t)
            return fail(where, QStringLiteral("expected a table, got %1").arg(nodeTypeName(n)));
        for (auto &&[k, v] : *t) {
            const QString key = qs(k.str());
            const QString at = QStringLiteral("%1.%2").arg(where, key);
            if (!keyRe.match(key).hasMatch())
                return fail(at, QStringLiteral("not a baresip setting name"));
            QStringList values;
            if (const auto *arr = v.as_array()) {
                for (const toml::node &e : *arr) {
                    QString value;
                    if (!scalar(e, at, &value))
                        return false;
                    values << value;
                }
            } else {
                QString value;
                if (!scalar(v, at, &value))
                    return false;
                values << value;
            }
            out->insert(key, values);
        }
        return true;
    }

    bool accountParams(const toml::node &n, const QString &where, QMap<QString, QString> *out)
    {
        static const QRegularExpression keyRe(QStringLiteral("^[a-z][a-z0-9_]*$"));
        static const QRegularExpression forbidden(QStringLiteral(R"([\s;<>"])"));
        const auto *t = n.as_table();
        if (!t)
            return fail(where, QStringLiteral("expected a table, got %1").arg(nodeTypeName(n)));
        for (auto &&[k, v] : *t) {
            const QString key = qs(k.str());
            const QString at = QStringLiteral("%1.%2").arg(where, key);
            if (!keyRe.match(key).hasMatch())
                return fail(at, QStringLiteral("not an account parameter name"));
            static const QStringList owned{QStringLiteral("auth_user"), QStringLiteral("auth_pass"),
                                           QStringLiteral("outbound"), QStringLiteral("transport")};
            if (owned.contains(key))
                return fail(at, QStringLiteral("set it on the profile itself, not in account_params"));
            QString value;
            if (!scalar(v, at, &value))
                return false;
            if (value.contains(forbidden))
                return fail(at, QStringLiteral("must not contain spaces, quotes or ; < >"));
            out->insert(key, value);
        }
        return true;
    }

    // [phonebook]: digit keys "0".."9", each a number or SIP address as typed in the dialer.
    bool phonebook(const toml::node &n, const QString &where, Phonebook *out)
    {
        static const QRegularExpression digitRe(QStringLiteral("^[0-9]$"));
        const auto *t = n.as_table();
        if (!t)
            return fail(where, QStringLiteral("expected a table, got %1").arg(nodeTypeName(n)));
        for (auto &&[k, v] : *t) {
            const QString key = qs(k.str());
            const QString at = QStringLiteral("%1.%2").arg(where, key);
            if (!digitRe.match(key).hasMatch())
                return fail(at, QStringLiteral("keys are the digits 0..9 (Alt+<digit> in the dialer)"));
            QString number;
            if (!string(v, at, &number))
                return false;
            if (!isValidPhonebookNumber(number))
                return fail(at, QStringLiteral("expected a number or SIP address without spaces, quotes, \\ or ; < >"));
            out->insert(key.toInt(), number);
        }
        return true;
    }

    bool profile(const QString &name, const toml::node &n, const BaresipSettings &shared,
                 const Phonebook &sharedPhonebook, Profile *p)
    {
        const QString where = QStringLiteral("profiles.%1").arg(name);
        if (!isValidName(name))
            return fail(where, QStringLiteral("profile names use letters, digits, '_', '-' and '.' (at most 32)"));
        const auto *t = n.as_table();
        if (!t)
            return fail(where, QStringLiteral("expected a table, got %1").arg(nodeTypeName(n)));
        p->name = name;
        p->baresip = shared;
        p->phonebook = sharedPhonebook;
        for (auto &&[k, v] : *t) {
            const QString key = qs(k.str());
            const QString at = QStringLiteral("%1.%2").arg(where, key);
            bool ok = true;
            if (key == QLatin1String("display_name"))
                ok = string(v, at, &p->displayName);
            else if (key == QLatin1String("label"))
                ok = string(v, at, &p->label);
            else if (key == QLatin1String("shortcut")) {
                const auto *i = v.as_integer();
                if (!i || i->get() < 0 || i->get() > 9)
                    return fail(at, QStringLiteral("expected a digit 0..9 (Ctrl+<digit> in the dialer)"));
                p->shortcut = static_cast<int>(i->get());
            }
            else if (key == QLatin1String("aor"))
                ok = string(v, at, &p->aor);
            else if (key == QLatin1String("transport"))
                ok = string(v, at, &p->transport);
            else if (key == QLatin1String("auth_user"))
                ok = string(v, at, &p->authUser);
            else if (key == QLatin1String("password"))
                ok = string(v, at, &p->password);
            else if (key == QLatin1String("password_cmd"))
                ok = string(v, at, &p->passwordCmd);
            else if (key == QLatin1String("outbound"))
                ok = string(v, at, &p->outbound);
            else if (key == QLatin1String("uuid"))
                ok = string(v, at, &p->uuid);
            else if (key == QLatin1String("confirm_dial"))
                ok = boolean(v, at, &p->confirmDial);
            else if (key == QLatin1String("record"))
                ok = boolean(v, at, &p->record);
            else if (key == QLatin1String("record_dir"))
                ok = string(v, at, &p->recordDir);
            else if (key == QLatin1String("account_params"))
                ok = accountParams(v, at, &p->accountParams);
            else if (key == QLatin1String("baresip")) {
                BaresipSettings own;
                ok = baresip(v, at, &own);
                for (auto it = own.cbegin(); it != own.cend(); ++it)
                    p->baresip.insert(it.key(), it.value());
            } else if (key == QLatin1String("phonebook")) {
                Phonebook own;
                ok = phonebook(v, at, &own);
                for (auto it = own.cbegin(); it != own.cend(); ++it)
                    p->phonebook.insert(it.key(), it.value());
            } else
                return fail(at, QStringLiteral("unknown key"));
            if (!ok)
                return false;
        }
        return check(where, p);
    }

    bool check(const QString &where, Profile *p)
    {
        if (p->aor.isEmpty())
            return fail(where, QStringLiteral("aor is required, e.g. aor = \"sip:alice@example.com\""));
        static const QRegularExpression aorRe(QStringLiteral("^sips?:[^@:;<>\"\\s]+@[^@;<>\"\\s]+$"));
        if (!aorRe.match(p->aor).hasMatch())
            return fail(where + QStringLiteral(".aor"), QStringLiteral("expected sip:user@domain[:port]"));
        if (!p->password.isEmpty() && !p->passwordCmd.isEmpty())
            return fail(where, QStringLiteral("set password or password_cmd, not both"));
        p->transport = p->transport.toLower();
        QString why;
        if (!accounts::validateRecord(p->account(), &why))
            return fail(where, why);
        static const QRegularExpression forbidden(QStringLiteral(R"([\s;<>"])"));
        if (p->password.contains(forbidden))
            return fail(where + QStringLiteral(".password"), QStringLiteral("must not contain spaces, quotes or ; < >"));
        static const QRegularExpression uuidRe(QStringLiteral("^[0-9A-Fa-f-]{8,64}$"));
        if (!p->uuid.isEmpty() && !uuidRe.match(p->uuid).hasMatch())
            return fail(where + QStringLiteral(".uuid"), QStringLiteral("expected a UUID"));
        if (p->recordDir.startsWith(QLatin1String("~/")))
            p->recordDir = QDir::home().filePath(p->recordDir.mid(2));
        if (!p->recordDir.isEmpty() && !QDir::isAbsolutePath(p->recordDir))
            return fail(where + QStringLiteral(".record_dir"), QStringLiteral("must be an absolute path or start with ~/"));
        return true;
    }
};

} // namespace

AccountRecord Profile::account() const
{
    AccountRecord r;
    const qsizetype colon = aor.indexOf(u':');
    const qsizetype at = aor.indexOf(u'@');
    r.scheme = aor.left(colon);
    r.user = aor.mid(colon + 1, at - colon - 1);
    r.domain = aor.mid(at + 1);
    r.displayName = displayName;
    r.transport = transport;
    r.authUser = authUser;
    r.outbound = outbound;
    for (auto it = accountParams.cbegin(); it != accountParams.cend(); ++it)
        r.extraParams += QStringLiteral(";%1=%2").arg(it.key(), it.value());
    return r;
}

AccountInfo Profile::info() const
{
    AccountInfo i = account().info();
    i.title = label;
    return i;
}

std::optional<Profile> byShortcut(const File &file, int digit)
{
    if (digit < 0)
        return std::nullopt;
    for (const Profile &p : file.profiles) {
        if (p.shortcut == digit)
            return p;
    }
    return std::nullopt;
}

QString defaultPath()
{
    const QString env = qEnvironmentVariable("LEANSIP_CONFIG");
    if (!env.isEmpty())
        return QDir(env).absolutePath();
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation))
        .filePath(QStringLiteral("leansip/leansip.toml"));
}

bool isValidName(const QString &name)
{
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9_.-]{0,31}$"));
    return re.match(name).hasMatch();
}

std::optional<File> parse(const QString &text, QString *error)
{
    const QByteArray utf8 = text.toUtf8();
    toml::parse_result parsed = toml::parse(std::string_view(utf8.constData(), static_cast<size_t>(utf8.size())));
    if (!parsed) {
        const toml::parse_error &e = parsed.error();
        if (error)
            *error = QStringLiteral("line %1, column %2: %3")
                         .arg(e.source().begin.line)
                         .arg(e.source().begin.column)
                         .arg(qs(e.description()));
        return std::nullopt;
    }
    const toml::table &root = parsed.table();
    Reader r{error};
    File file;
    BaresipSettings shared;
    if (const toml::node *b = root.get("baresip"); b && !r.baresip(*b, QStringLiteral("baresip"), &shared))
        return std::nullopt;
    Phonebook sharedPhonebook;
    if (const toml::node *pb = root.get("phonebook"); pb && !r.phonebook(*pb, QStringLiteral("phonebook"), &sharedPhonebook))
        return std::nullopt;
    for (auto &&[k, v] : root) {
        const QString key = qs(k.str());
        if (key == QLatin1String("default_profile")) {
            if (!r.string(v, key, &file.defaultProfile))
                return std::nullopt;
        } else if (key == QLatin1String("profiles")) {
            const auto *profiles = v.as_table();
            if (!profiles) {
                r.fail(key, QStringLiteral("expected a table of profiles"));
                return std::nullopt;
            }
            for (auto &&[name, node] : *profiles) {
                Profile p;
                if (!r.profile(qs(name.str()), node, shared, sharedPhonebook, &p))
                    return std::nullopt;
                for (const Profile &other : std::as_const(file.profiles)) {
                    if (p.shortcut >= 0 && other.shortcut == p.shortcut) {
                        r.fail(QStringLiteral("profiles.%1.shortcut").arg(p.name),
                               QStringLiteral("%1 is already the shortcut of profile %2").arg(p.shortcut).arg(other.name));
                        return std::nullopt;
                    }
                }
                file.profiles.insert(p.name, p);
            }
        } else if (key != QLatin1String("baresip") && key != QLatin1String("phonebook")) {
            r.fail(key, QStringLiteral("unknown key"));
            return std::nullopt;
        }
    }
    if (file.profiles.isEmpty()) {
        r.fail({}, QStringLiteral("no profiles; add one under [profiles.<name>]"));
        return std::nullopt;
    }
    if (!file.defaultProfile.isEmpty() && !file.profiles.contains(file.defaultProfile)) {
        r.fail(QStringLiteral("default_profile"), QStringLiteral("no profile named '%1'").arg(file.defaultProfile));
        return std::nullopt;
    }
    return file;
}

std::optional<File> load(const QString &path, QString *error)
{
    auto fail = [error](const QString &why) -> std::optional<File> {
        if (error)
            *error = why;
        return std::nullopt;
    };
    struct stat st {};
    if (::stat(QFile::encodeName(path).constData(), &st) != 0)
        return fail(QStringLiteral("no config file at %1; see docs/configuration.md in the leansip source or /usr/share/doc/leansip").arg(path));
    if (st.st_uid != ::getuid())
        return fail(QStringLiteral("%1 belongs to another user").arg(path));
    if ((st.st_mode & 077) != 0)
        return fail(QStringLiteral("%1 can hold passwords but is readable by others (mode %2); run: chmod 600 %1")
                        .arg(path)
                        .arg(st.st_mode & 0777, 4, 8, QLatin1Char('0')));
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("cannot read %1: %2").arg(path, f.errorString()));
    QString why;
    auto file = parse(QString::fromUtf8(f.readAll()), &why);
    if (!file)
        return fail(QStringLiteral("%1: %2").arg(path, why));
    file->path = path;
    return file;
}

bool isValidPhonebookNumber(const QString &number)
{
    static const QRegularExpression re(QStringLiteral(R"(^[^\s"'\\<>;]{1,128}$)"));
    return re.match(number).hasMatch();
}

namespace {

// The lines of `text` with their line ends, so joining them gives `text` back.
QStringList splitKeepingEnds(const QString &text)
{
    QStringList lines;
    qsizetype start = 0;
    for (qsizetype i = 0; i < text.size(); ++i) {
        if (text.at(i) == u'\n') {
            lines.append(text.mid(start, i - start + 1));
            start = i + 1;
        }
    }
    if (start < text.size())
        lines.append(text.mid(start));
    return lines;
}

// Matches the header line of [phonebook] or [profiles.<name>.phonebook], name bare or quoted.
bool isHeaderOf(const QString &line, const QString &profile)
{
    if (profile.isEmpty()) {
        static const QRegularExpression re(QStringLiteral(R"(^\s*\[\s*phonebook\s*\]\s*(#.*)?\s*$)"));
        return re.match(line).hasMatch();
    }
    const QString name = QRegularExpression::escape(profile);
    const QRegularExpression re(
        QStringLiteral(R"(^\s*\[\s*profiles\s*\.\s*(%1|"%1"|'%1')\s*\.\s*phonebook\s*\]\s*(#.*)?\s*$)").arg(name));
    return re.match(line).hasMatch();
}

} // namespace

PhonebookSave savePhonebookEntry(const QString &path, const QString &profile, int digit, const QString &number)
{
    PhonebookSave result;
    auto fail = [&result](const QString &why) {
        result.error = why;
        return result;
    };
    if (digit < 0 || digit > 9)
        return fail(QStringLiteral("phonebook keys are the digits 0..9"));
    if (!isValidPhonebookNumber(number))
        return fail(QStringLiteral("%1 cannot be a phonebook entry").arg(number));

    const QString target = QFileInfo(path).canonicalFilePath(); // write through a link, not over it
    if (target.isEmpty())
        return fail(QStringLiteral("no config file at %1").arg(path));
    QString error;
    const auto before = load(target, &error); // also checks owner and mode
    if (!before)
        return fail(error);
    if (!profile.isEmpty() && !before->profiles.contains(profile))
        return fail(QStringLiteral("no profile %1 in %2").arg(profile, path));

    QFile in(target);
    if (!in.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("cannot read %1: %2").arg(target, in.errorString()));
    const QByteArray raw = in.readAll();
    in.close();
    toml::parse_result parsed = toml::parse(std::string_view(raw.constData(), static_cast<size_t>(raw.size())));
    if (!parsed)
        return fail(QStringLiteral("%1 changed while saving; try again").arg(path));
    const toml::table &root = parsed.table();

    const std::string key = std::to_string(digit);
    const std::string profileKey = profile.toStdString();
    const toml::table *table = profile.isEmpty()
        ? root["phonebook"].as_table()
        : root["profiles"][profileKey]["phonebook"].as_table();
    const QString tableName = profile.isEmpty() ? QStringLiteral("[phonebook]")
                                                : QStringLiteral("[profiles.%1.phonebook]").arg(profile);
    const QString handEdit = QStringLiteral("%1 in %2 has a layout leansip does not edit; change it by hand")
                                 .arg(tableName, path);
    const QString literal = QStringLiteral("\"%1\"").arg(number);

    const QString text = QString::fromUtf8(raw);
    QStringList lines = splitKeepingEnds(text);
    if (table) {
        if (table->is_inline())
            return fail(handEdit);
        if (const toml::node *old = table->get(key)) {
            // Replace the one string on that entry's line.
            const auto line = static_cast<qsizetype>(old->source().begin.line) - 1;
            static const QRegularExpression value(QStringLiteral(R"(=\s*("[^"\\\n]*"|'[^'\n]*'))"));
            if (line < 0 || line >= lines.size())
                return fail(handEdit);
            const QString &current = lines.at(line);
            auto matches = value.globalMatch(current);
            QRegularExpressionMatch only;
            int count = 0;
            while (matches.hasNext()) {
                only = matches.next();
                ++count;
            }
            if (count != 1)
                return fail(handEdit);
            if (const auto *s = old->as_string())
                result.previous = qs(s->get());
            lines[line] = current.left(only.capturedStart(1)) + literal + current.mid(only.capturedEnd(1));
        } else {
            // A new line right under the table's header.
            const auto header = static_cast<qsizetype>(table->source().begin.line) - 1;
            if (header < 0 || header >= lines.size() || !isHeaderOf(lines.at(header), profile))
                return fail(handEdit);
            const QString eol = lines.at(header).endsWith(QLatin1String("\r\n")) ? QStringLiteral("\r\n")
                                                                               : QStringLiteral("\n");
            if (!lines.at(header).endsWith(u'\n'))
                lines[header] += eol;
            lines.insert(header + 1, QStringLiteral("%1 = %2").arg(digit).arg(literal) + eol);
        }
    } else {
        // No such table yet: a new one at the end of the file.
        QString tail;
        if (!text.isEmpty() && !text.endsWith(u'\n'))
            tail += u'\n';
        tail += profile.isEmpty() ? QStringLiteral("\n[phonebook]\n")
                                  : QStringLiteral("\n[profiles.\"%1\".phonebook]\n").arg(profile);
        tail += QStringLiteral("%1 = %2\n").arg(digit).arg(literal);
        lines.append(tail);
    }
    const QString edited = lines.join(QString());

    // The edit must mean exactly "this entry, nothing else".
    toml::table expected = root;
    toml::table *into = &expected;
    if (!profile.isEmpty()) {
        into = expected["profiles"][profileKey].as_table();
        if (!into)
            return fail(handEdit);
    }
    if (!into->contains("phonebook"))
        into->insert("phonebook", toml::table{});
    toml::table *pb = (*into)["phonebook"].as_table();
    if (!pb)
        return fail(handEdit);
    pb->insert_or_assign(key, number.toStdString());
    const QByteArray editedUtf8 = edited.toUtf8();
    toml::parse_result check = toml::parse(std::string_view(editedUtf8.constData(), static_cast<size_t>(editedUtf8.size())));
    if (!check || check.table() != expected || !parse(edited, &error))
        return fail(handEdit);

    QSaveFile out(target);
    if (!out.open(QIODevice::WriteOnly)
        || !out.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
        || out.write(editedUtf8) != editedUtf8.size() || !out.commit()) {
        return fail(QStringLiteral("cannot write %1: %2").arg(target, out.errorString()));
    }
    result.ok = true;
    return result;
}

std::optional<Profile> select(const File &file, const QString &requested, QString *error)
{
    QString name = requested;
    if (name.isEmpty())
        name = qEnvironmentVariable("LEANSIP_PROFILE");
    if (name.isEmpty())
        name = file.defaultProfile;
    if (name.isEmpty() && file.profiles.size() == 1)
        name = file.profiles.firstKey();
    const QString known = file.profiles.keys().join(QStringLiteral(", "));
    if (name.isEmpty()) {
        if (error)
            *error = QStringLiteral("%1 has several profiles and no default_profile; pick one with --profile (%2)")
                         .arg(file.path, known);
        return std::nullopt;
    }
    if (!file.profiles.contains(name)) {
        if (error)
            *error = QStringLiteral("no profile '%1' in %2 (have: %3)").arg(name, file.path, known);
        return std::nullopt;
    }
    return file.profiles.value(name);
}

std::optional<QString> password(const Profile &profile, QString *error, int timeoutMs)
{
    if (profile.passwordCmd.isEmpty())
        return profile.password;
    auto fail = [&](const QString &why) -> std::optional<QString> {
        if (error)
            *error = QStringLiteral("profile %1: password_cmd %2").arg(profile.name, why);
        return std::nullopt;
    };
    QProcess p;
    p.setStandardInputFile(QProcess::nullDevice());
    p.setProcessChannelMode(QProcess::ForwardedErrorChannel); // its complaints go to our log
    // Its own process group, so a timeout also stops what the shell started (a prompt, say).
    p.setChildProcessModifier([] { ::setpgid(0, 0); });
    p.start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), profile.passwordCmd});
    if (!p.waitForStarted(5000))
        return fail(QStringLiteral("did not start: %1").arg(p.errorString()));
    if (!p.waitForFinished(timeoutMs)) {
        ::kill(-static_cast<pid_t>(p.processId()), SIGKILL);
        p.kill();
        p.waitForFinished(1000);
        return fail(QStringLiteral("did not finish within %1 s").arg(timeoutMs / 1000));
    }
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0)
        return fail(QStringLiteral("failed (exit code %1)").arg(p.exitCode()));
    QString out = QString::fromUtf8(p.readAllStandardOutput());
    out = out.left(out.indexOf(u'\n'));
    if (out.endsWith(u'\r'))
        out.chop(1);
    if (out.isEmpty())
        return fail(QStringLiteral("printed nothing"));
    return out;
}

std::optional<QString> accountLine(const Profile &profile, const QString &password, QString *error)
{
    AccountRecord r = profile.account();
    r.password = password;
    QString why;
    if (!accounts::validateRecord(r, &why)) {
        if (error)
            *error = QStringLiteral("profile %1: %2").arg(profile.name, why);
        return std::nullopt;
    }
    return r.toLine();
}

QString instanceId(const Profile &profile, const QByteArray &machineId)
{
    if (!profile.uuid.isEmpty())
        return profile.uuid;
    if (machineId.isEmpty())
        return QString();
    // Fixed namespace for leansip instance ids; changing it changes every derived id.
    static const QUuid ns(QStringLiteral("8f0c2d6e-5b1a-4c7e-9d3f-2a6b8e4c1f70"));
    const QByteArray name = machineId + '\n' + profile.name.toUtf8() + '\n' + profile.aor.toUtf8();
    return QUuid::createUuidV5(ns, name).toString(QUuid::WithoutBraces);
}

QByteArray machineId()
{
    QFile f(QStringLiteral("/etc/machine-id"));
    if (!f.open(QIODevice::ReadOnly))
        return QByteArray();
    return f.read(256).trimmed();
}

QString baresipTemplate()
{
    return QString::fromLatin1(kTemplate);
}

QString baresipConfig(const BaresipSettings &overrides)
{
    auto linesFor = [](const QString &key, const QStringList &values) {
        QStringList out;
        for (const QString &v : values)
            out << QStringLiteral("%1\t\t%2").arg(key, v);
        return out;
    };
    QStringList out;
    QStringList written;
    const QStringList lines = baresipTemplate().split(u'\n');
    for (const QString &line : lines) {
        const QString key = line.section(QRegularExpression(QStringLiteral("\\s+")), 0, 0);
        if (key.isEmpty() || key.startsWith(u'#') || !overrides.contains(key)) {
            out << line;
            continue;
        }
        // The override takes the place of the first line it replaces.
        if (!written.contains(key)) {
            out << linesFor(key, overrides.value(key));
            written << key;
        }
    }
    QStringList extra;
    for (auto it = overrides.cbegin(); it != overrides.cend(); ++it) {
        if (!written.contains(it.key()))
            extra << linesFor(it.key(), it.value());
    }
    while (!out.isEmpty() && out.last().isEmpty())
        out.removeLast();
    if (!extra.isEmpty())
        out << QString() << QStringLiteral("# leansip.toml") << extra;
    out << QString();
    return out.join(u'\n');
}

} // namespace leansip::settings
