#pragma once

#include "settings.h"

#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

namespace leansip {

// First run: turns existing baresip setups (~/.baresip, ~/.baresip2, ~/.baresip-work, ...) into
// leansip.toml profiles. Every account line in a directory's `accounts` file becomes a profile.
// Only what the user chose is carried over: a `config` setting is kept when its value differs
// both from what baresip writes into a fresh config and from leansip's built-in template, and
// account parameters at baresip's default value are dropped. baresip's instance `uuid` file is
// not carried over; leansip derives a stable one per profile.
namespace baresip_import {

// baresip config text as key -> values, one value per line for that key. Comments, blank lines
// and trailing "# ..." comments are dropped; whitespace inside a value is collapsed.
using ConfigLines = QMap<QString, QStringList>;
ConfigLines parseConfig(const QString &text);

// The config baresip writes when it starts with an empty config directory: `binary -f <tmp>
// -t 1`. Empty when baresip cannot be run; the import then compares against leansip's
// template only.
QString baresipDefaultConfig(const QString &binary = QStringLiteral("baresip"), int timeoutMs = 10000);

struct ImportedProfile {
    settings::Profile profile; // name, display name, account fields and baresip overrides only
    QString sourceDir;         // the directory it came from
    QStringList notes;         // what was left out and why, for the summary
};

struct Plan {
    QList<ImportedProfile> profiles;
    QString defaultProfile;
    QStringList skipped; // directories or lines that could not be imported, with the reason
};

// Looks for `.baresip*` directories with an `accounts` file in `homeDir`. `baresipDefaults` is
// baresipDefaultConfig() (may be empty).
Plan discover(const QString &homeDir, const QString &baresipDefaults);

// leansip.toml text for the plan. It parses back to the plan's profiles.
QString toToml(const Plan &plan);

// What discover() found, for a confirmation dialog. Passwords are never included.
QString summary(const Plan &plan);

// Writes `toml` to `path` (a new file only: refuses to replace one), private to the user, in a
// directory made private if it has to be created. Re-parses it first, so a broken file is never
// written.
bool write(const QString &path, const QString &toml, QString *error);

} // namespace baresip_import
} // namespace leansip
