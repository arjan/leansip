#pragma once

#include <QString>
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

namespace accounts {

// Parses one account line of baresip's `accounts` file, e.g.
//   "Alice" <sip:alice@example.com;transport=udp>;auth_user=alice;auth_pass=secret
std::optional<AccountInfo> parseLine(const QString &line);

// Returns the first non-comment, non-empty account in the file contents.
std::optional<AccountInfo> parseFirst(const QString &contents);

// Reads `path` and returns the first account, or an error message.
struct LoadResult {
    std::optional<AccountInfo> account;
    QString error;
};
LoadResult loadFirst(const QString &path);

} // namespace accounts
} // namespace leansip
