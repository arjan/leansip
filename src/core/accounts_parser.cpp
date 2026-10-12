#include "accounts_parser.h"

#include <QFile>
#include <QStringList>
#include <QTextStream>

namespace leansip::accounts {

namespace {

// Splits "key=value" pairs separated by ';' following the URI.
QString paramValue(const QString &params, const QString &key)
{
    const QStringList parts = params.split(u';', Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        const qsizetype eq = part.indexOf(u'=');
        if (eq < 0)
            continue;
        if (part.left(eq).trimmed() == key)
            return part.mid(eq + 1).trimmed();
    }
    return {};
}

} // namespace

std::optional<AccountInfo> parseLine(const QString &rawLine)
{
    QString line = rawLine.trimmed();
    if (line.isEmpty() || line.startsWith(u'#'))
        return std::nullopt;

    AccountInfo info;

    // Optional quoted display name.
    if (line.startsWith(u'"')) {
        const qsizetype close = line.indexOf(u'"', 1);
        if (close < 0)
            return std::nullopt;
        info.displayName = line.mid(1, close - 1).trimmed();
        line = line.mid(close + 1).trimmed();
    }

    // URI either in angle brackets (with params after '>') or bare.
    QString uri;
    QString params;
    if (line.startsWith(u'<')) {
        const qsizetype close = line.indexOf(u'>');
        if (close < 0)
            return std::nullopt;
        uri = line.mid(1, close - 1).trimmed();
        params = line.mid(close + 1);
    } else {
        const qsizetype semi = line.indexOf(u';');
        uri = semi < 0 ? line : line.left(semi);
        params = semi < 0 ? QString() : line.mid(semi);
    }

    // Strip URI parameters (";transport=udp") from the AOR itself.
    const qsizetype uriSemi = uri.indexOf(u';');
    if (uriSemi >= 0)
        uri = uri.left(uriSemi);
    if (!uri.startsWith(u"sip:") && !uri.startsWith(u"sips:"))
        return std::nullopt;

    const qsizetype colon = uri.indexOf(u':');
    const QString userHost = uri.mid(colon + 1);
    const qsizetype at = userHost.indexOf(u'@');
    if (at <= 0 || at == userHost.size() - 1)
        return std::nullopt;

    info.aor = uri;
    info.user = userHost.left(at);
    info.domain = userHost.mid(at + 1);
    info.authUser = paramValue(params, QStringLiteral("auth_user"));
    if (info.authUser.isEmpty())
        info.authUser = info.user;
    return info;
}

std::optional<AccountInfo> parseFirst(const QString &contents)
{
    const QStringList lines = contents.split(u'\n');
    for (const QString &line : lines) {
        if (auto info = parseLine(line))
            return info;
    }
    return std::nullopt;
}

LoadResult loadFirst(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {std::nullopt, QStringLiteral("cannot open %1: %2").arg(path, file.errorString())};
    QTextStream in(&file);
    auto account = parseFirst(in.readAll());
    if (!account)
        return {std::nullopt, QStringLiteral("no valid account found in %1").arg(path)};
    return {account, {}};
}

} // namespace leansip::accounts
