#include "accounts_parser.h"

#include <QRegularExpression>

namespace leansip {

namespace {

// Splits the part after the URI into (key, value) pairs; `rest` collects pairs we don't model.
struct LineParams {
    QString authUser;
    QString password;
    QString outbound;
    QString rest; // ";key=value" fragments, verbatim order
};

LineParams splitParams(const QString &params)
{
    LineParams out;
    const QStringList parts = params.split(u';', Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        const QString p = part.trimmed();
        if (p.isEmpty())
            continue;
        const qsizetype eq = p.indexOf(u'=');
        const QString key = eq < 0 ? p : p.left(eq).trimmed();
        const QString value = eq < 0 ? QString() : p.mid(eq + 1).trimmed();
        if (key == QLatin1String("auth_user"))
            out.authUser = value;
        else if (key == QLatin1String("auth_pass"))
            out.password = value;
        else if (key == QLatin1String("outbound"))
            out.outbound = value;
        else
            out.rest += u';' + p;
    }
    return out;
}

// Splits a raw line into display name, URI (inside <> or bare) and the trailing parameter string.
struct SplitLine {
    QString displayName;
    QString uri;
    QString params;
};

std::optional<SplitLine> splitLine(const QString &rawLine)
{
    QString line = rawLine.trimmed();
    if (line.isEmpty() || line.startsWith(u'#'))
        return std::nullopt;

    SplitLine out;
    if (line.startsWith(u'"')) {
        const qsizetype close = line.indexOf(u'"', 1);
        if (close < 0)
            return std::nullopt;
        out.displayName = line.mid(1, close - 1).trimmed();
        line = line.mid(close + 1).trimmed();
    }
    if (line.startsWith(u'<')) {
        const qsizetype close = line.indexOf(u'>');
        if (close < 0)
            return std::nullopt;
        out.uri = line.mid(1, close - 1).trimmed();
        out.params = line.mid(close + 1);
    } else {
        const qsizetype semi = line.indexOf(u';');
        out.uri = semi < 0 ? line : line.left(semi);
        out.params = semi < 0 ? QString() : line.mid(semi);
    }
    return out;
}

// "sip:user@domain;transport=udp" -> scheme, user, domain, transport. Other URI params are not modelled.
bool splitUri(const QString &uri, QString &scheme, QString &user, QString &domain, QString &transport)
{
    if (!uri.startsWith(u"sip:") && !uri.startsWith(u"sips:"))
        return false;
    const qsizetype colon = uri.indexOf(u':');
    scheme = uri.left(colon);
    QString rest = uri.mid(colon + 1);
    const qsizetype semi = rest.indexOf(u';');
    QString uriParams;
    if (semi >= 0) {
        uriParams = rest.mid(semi);
        rest = rest.left(semi);
    }
    const qsizetype at = rest.indexOf(u'@');
    if (at <= 0 || at == rest.size() - 1)
        return false;
    user = rest.left(at);
    domain = rest.mid(at + 1);
    static const QRegularExpression transportRe(QStringLiteral(R"(;transport=([A-Za-z]+))"));
    const auto m = transportRe.match(uriParams);
    transport = m.hasMatch() ? m.captured(1).toLower() : QString();
    return true;
}

} // namespace

AccountInfo AccountRecord::info() const
{
    AccountInfo i;
    i.displayName = displayName;
    i.aor = aor();
    i.user = user;
    i.domain = domain;
    i.authUser = authUser.isEmpty() ? user : authUser;
    return i;
}

std::optional<AccountRecord> AccountRecord::fromLine(const QString &line)
{
    const auto split = splitLine(line);
    if (!split)
        return std::nullopt;
    AccountRecord r;
    if (!splitUri(split->uri, r.scheme, r.user, r.domain, r.transport))
        return std::nullopt;
    r.displayName = split->displayName;
    const LineParams p = splitParams(split->params);
    r.authUser = p.authUser;
    r.password = p.password;
    r.outbound = p.outbound;
    r.extraParams = p.rest;
    return r;
}

QString AccountRecord::toLine() const
{
    QString line;
    if (!displayName.isEmpty())
        line += u'"' + displayName + u"\" ";
    line += u'<' + aor();
    if (!transport.isEmpty())
        line += QStringLiteral(";transport=%1").arg(transport);
    line += u'>';
    if (!authUser.isEmpty())
        line += QStringLiteral(";auth_user=%1").arg(authUser);
    if (!password.isEmpty())
        line += QStringLiteral(";auth_pass=%1").arg(password);
    if (!outbound.isEmpty())
        line += QStringLiteral(";outbound=%1").arg(outbound);
    line += extraParams;
    return line;
}

namespace accounts {

std::optional<AccountInfo> parseLine(const QString &rawLine)
{
    const auto record = AccountRecord::fromLine(rawLine);
    if (!record)
        return std::nullopt;
    return record->info();
}

bool validateRecord(const AccountRecord &record, QString *error)
{
    auto fail = [error](const QString &why) {
        if (error)
            *error = why;
        return false;
    };
    static const QRegularExpression forbidden(QStringLiteral(R"([\s;<>"])"));
    struct Field {
        const char *name;
        const QString *value;
    };
    for (const Field f : {Field{"user", &record.user}, Field{"domain", &record.domain}, Field{"auth user", &record.authUser},
                          Field{"password", &record.password}, Field{"outbound proxy", &record.outbound}}) {
        if (f.value->contains(forbidden))
            return fail(QStringLiteral("%1 must not contain spaces, quotes or ; < >").arg(QLatin1String(f.name)));
    }
    if (record.displayName.contains(u'"') || record.displayName.contains(u'\n'))
        return fail(QStringLiteral("display name must not contain quotes"));
    if (record.user.isEmpty())
        return fail(QStringLiteral("user is required"));
    if (record.domain.isEmpty())
        return fail(QStringLiteral("domain is required"));
    if (record.user.contains(u'@') || record.domain.contains(u'@'))
        return fail(QStringLiteral("user and domain must not contain @"));
    if (!record.transport.isEmpty() && record.transport != QLatin1String("udp") && record.transport != QLatin1String("tcp")
        && record.transport != QLatin1String("tls"))
        return fail(QStringLiteral("transport must be udp, tcp or tls"));
    if (!record.outbound.isEmpty() && !record.outbound.startsWith(QLatin1String("sip:"))
        && !record.outbound.startsWith(QLatin1String("sips:")))
        return fail(QStringLiteral("outbound proxy must start with sip:"));
    return true;
}

} // namespace accounts
} // namespace leansip
