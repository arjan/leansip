#pragma once

#include <QString>
#include <QStringList>
#include <optional>

namespace leansip {

// The subset of a baresip account line we show in the UI.
// Deliberately has no field for auth_pass: credentials never leave the accounts file.
struct AccountInfo {
    QString displayName; // optional, from the quoted prefix
    QString aor;         // "sip:user@domain" without URI parameters
    QString user;
    QString domain;
    QString authUser;    // falls back to `user` when absent

    // What to show as a single label: display name if present, else the AOR.
    QString label() const { return displayName.isEmpty() ? aor : displayName; }
};

// A full account line. Fields we don't model are kept verbatim in `extraParams`
// (codec lists, regint, medianat, ...).
struct AccountRecord {
    QString scheme = QStringLiteral("sip"); // "sip" or "sips"
    QString displayName;
    QString user;
    QString domain;
    QString transport;   // "", "udp", "tcp" or "tls" (URI parameter inside the <>)
    QString authUser;
    QString password;    // auth_pass; written to the file, never logged
    QString outbound;    // outbound proxy URI
    QString extraParams; // ";key=value;..." leftovers, exactly as found (may be empty)

    QString aor() const { return QStringLiteral("%1:%2@%3").arg(scheme, user, domain); }
    AccountInfo info() const;

    static std::optional<AccountRecord> fromLine(const QString &line);
    QString toLine() const;

    bool operator==(const AccountRecord &) const = default;
};

namespace accounts {

// Parses one account line of baresip's `accounts` file, e.g.
//   "Alice" <sip:alice@example.com;transport=udp>;auth_user=alice;auth_pass=secret
std::optional<AccountInfo> parseLine(const QString &line);

// Checks a record can be serialised unambiguously (non-empty user/domain, no characters that
// would break the line grammar, known transport). `error` gets a human-readable reason.
bool validateRecord(const AccountRecord &record, QString *error);

} // namespace accounts
} // namespace leansip
