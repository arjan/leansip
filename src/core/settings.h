#pragma once

#include "accounts_parser.h"

#include <QMap>
#include <QString>
#include <QStringList>

#include <optional>

namespace leansip {

// leansip.toml: named SIP profiles, each one account with its own leansipd and baresip.
//
//   default_profile = "local"
//
//   [baresip]                      # settings for every profile's baresip config
//   audio_player = "pulse,"
//
//   [profiles.local]
//   aor = "sip:alice@127.0.0.1"
//   password_cmd = "rbw get sip-local"
//
//   [profiles.local.baresip]       # this profile only; wins over [baresip]
//   jitter_buffer_delay = "5-10"
//
// Unknown keys are errors, so a typo never silently drops a setting.
namespace settings {

// baresip config overrides: each key replaces every line for that key in the built-in
// template; a list gives one line per element, an empty list removes the key.
using BaresipSettings = QMap<QString, QStringList>;

struct Profile {
    QString name;
    QString displayName; // SIP display name, sent to the far end
    QString label;       // the dialer's name for this profile; empty: display name or AOR
    int shortcut = -1;   // 0..9: Ctrl+<digit> switches the dialer to this profile; -1: none
    QString aor;         // "sip:user@domain[:port]" or "sips:..."
    QString transport;   // "", "udp", "tcp" or "tls"
    QString authUser;
    QString password;    // never logged
    QString passwordCmd; // run with /bin/sh -c; the first line of its output is the password
    QString outbound;
    QMap<QString, QString> accountParams; // more ";key=value" account parameters
    QString uuid;        // baresip's instance id; baresip makes one up when empty
    bool confirmDial = false; // leansipctl dials only with --yes
    bool record = false;      // record every call, as if leansipd got --record
    QString recordDir;        // empty: <runtime dir>/recordings
    BaresipSettings baresip;  // [baresip] merged with [profiles.<name>.baresip]

    // The account without its password, for display and validation.
    AccountRecord account() const;
    // What the dialer shows: the account, titled with `label` when set.
    AccountInfo info() const;
};

struct File {
    QString path;
    QString defaultProfile;
    QMap<QString, Profile> profiles;
};

// The profile with `shortcut = digit`, if any.
std::optional<Profile> byShortcut(const File &file, int digit);

// $LEANSIP_CONFIG, else $XDG_CONFIG_HOME/leansip/leansip.toml.
QString defaultPath();

// Letters, digits, '_', '-' and '.', starting with a letter or digit, at most 32 long: names
// end up in socket and directory paths.
bool isValidName(const QString &name);

std::optional<File> parse(const QString &text, QString *error);

// Reads and parses `path`. The file holds passwords, so it must belong to the current user and
// not be readable by group or others.
std::optional<File> load(const QString &path, QString *error);

// `requested` if given, else $LEANSIP_PROFILE, else default_profile, else the only profile.
std::optional<Profile> select(const File &file, const QString &requested, QString *error);

// The literal password, or the output of password_cmd. Empty when the profile has neither.
std::optional<QString> password(const Profile &profile, QString *error, int timeoutMs = 60000);

// The line for baresip's `accounts` file.
std::optional<QString> accountLine(const Profile &profile, const QString &password, QString *error);

// The built-in baresip config with `overrides` applied.
QString baresipConfig(const BaresipSettings &overrides);
QString baresipTemplate();

} // namespace settings
} // namespace leansip
