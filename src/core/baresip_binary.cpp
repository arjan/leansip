#include "baresip_binary.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QStandardPaths>
#include <QTemporaryFile>

#include <cerrno>
#include <cstdio>
#include <cstring>

Q_LOGGING_CATEGORY(lcBinary, "leansip.baresip")

namespace leansip::baresip_binary {

namespace {

Location failed(const QString &why)
{
    Location l;
    l.error = why;
    return l;
}

QByteArray sha256(const QByteArray &data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256);
}

bool sameContents(const QString &path, const QByteArray &hash, qint64 size)
{
    QFile f(path);
    if (f.size() != size || !f.open(QIODevice::ReadOnly))
        return false;
    QCryptographicHash h(QCryptographicHash::Sha256);
    return h.addData(&f) && h.result() == hash;
}

// Writes `data` next to `path` and renames it into place, so another leansipd starting at the
// same moment sees either nothing or the whole file, already with its permissions.
QString writeAtomically(const QString &path, const QByteArray &data, QFileDevice::Permissions perms)
{
    QTemporaryFile tmp(path + QStringLiteral(".XXXXXX"));
    tmp.setAutoRemove(true);
    if (!tmp.open() || tmp.write(data) != data.size() || !tmp.flush() || !tmp.setPermissions(perms))
        return QStringLiteral("cannot write %1: %2").arg(path, tmp.errorString());
    tmp.close();
    // POSIX rename() replaces an existing file in one step; QFile::rename() refuses to.
    if (std::rename(QFile::encodeName(tmp.fileName()).constData(), QFile::encodeName(path).constData()) != 0)
        return QStringLiteral("cannot write %1: %2").arg(path, QString::fromLocal8Bit(std::strerror(errno)));
    tmp.setAutoRemove(false);
    return QString();
}

} // namespace

QString defaultCacheRoot()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation))
        .filePath(QStringLiteral("leansip"));
}

Location extractEmbedded(const QString &resourcePrefix, const QString &cacheRoot)
{
    QFile embedded(resourcePrefix + QStringLiteral("/baresip"));
    if (!embedded.open(QIODevice::ReadOnly))
        return failed(QStringLiteral("no embedded baresip"));
    const QByteArray binary = embedded.readAll();
    const QByteArray hash = sha256(binary);

    const QDir dir(QDir(cacheRoot).filePath(QStringLiteral("baresip-") + QString::fromLatin1(hash.toHex().left(16))));
    const QString soundsDir = dir.filePath(QStringLiteral("sounds"));
    if (!QDir().mkpath(soundsDir))
        return failed(QStringLiteral("cannot create %1").arg(soundsDir));
    // Only this user may replace what leansipd will run.
    constexpr auto kPrivateDir = QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;
    QFile::setPermissions(cacheRoot, kPrivateDir);
    QFile::setPermissions(dir.path(), kPrivateDir);

    Location l;
    l.program = dir.filePath(QStringLiteral("baresip"));
    l.soundsDir = soundsDir;
    if (!sameContents(l.program, hash, binary.size())) {
        qCInfo(lcBinary) << "extracting the embedded baresip to" << l.program;
        if (QString e = writeAtomically(l.program, binary, kPrivateDir); !e.isEmpty())
            return failed(e);
    }

    const QDir sounds(resourcePrefix + QStringLiteral("/sounds"));
    const QStringList wavs = sounds.entryList(QDir::Files);
    for (const QString &wav : wavs) {
        QFile in(sounds.filePath(wav));
        if (!in.open(QIODevice::ReadOnly))
            return failed(QStringLiteral("cannot read embedded %1").arg(wav));
        const QByteArray data = in.readAll();
        const QString out = QDir(soundsDir).filePath(wav);
        if (sameContents(out, sha256(data), data.size()))
            continue;
        if (QString e = writeAtomically(out, data, QFileDevice::ReadOwner | QFileDevice::WriteOwner); !e.isEmpty())
            return failed(e);
    }
    return l;
}

Location locate()
{
    if (const QString env = qEnvironmentVariable("LEANSIP_BARESIP"); !env.isEmpty())
        return Location{env, QString(), QString()};

    if (QFileInfo::exists(QString::fromLatin1(kEmbeddedPrefix) + QStringLiteral("/baresip")))
        return extractEmbedded(QString::fromLatin1(kEmbeddedPrefix), defaultCacheRoot());

#ifdef LEANSIP_BARESIP_PATH
    if (QFileInfo(QStringLiteral(LEANSIP_BARESIP_PATH)).isExecutable()) {
#ifdef LEANSIP_BARESIP_SOUNDS
        return Location{QStringLiteral(LEANSIP_BARESIP_PATH), QStringLiteral(LEANSIP_BARESIP_SOUNDS), QString()};
#else
        return Location{QStringLiteral(LEANSIP_BARESIP_PATH), QString(), QString()};
#endif
    }
#endif

    return Location{QStringLiteral("baresip"), QString(), QString()};
}

} // namespace leansip::baresip_binary
