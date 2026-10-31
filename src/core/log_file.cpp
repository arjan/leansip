#include "log_file.h"

#include <QDateTime>

namespace leansip {

LogFile::LogFile(QString path, qint64 maxBytes)
    : m_path(std::move(path))
    , m_maxBytes(maxBytes)
{
}

bool LogFile::open(QString *error)
{
    m_file.setFileName(m_path);
    if (m_file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        m_file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        rotateIfNeeded();
        return true;
    }
    if (error)
        *error = QStringLiteral("cannot open %1: %2").arg(m_path, m_file.errorString());
    return false;
}

void LogFile::rotateIfNeeded()
{
    if (m_file.size() < m_maxBytes)
        return;
    m_file.close();
    const QString old = m_path + QStringLiteral(".1");
    QFile::remove(old);
    QFile::rename(m_path, old);
    m_file.setFileName(m_path);
    if (m_file.open(QIODevice::WriteOnly | QIODevice::Append))
        m_file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}

void LogFile::write(const QByteArray &data)
{
    if (!m_file.isOpen())
        return;
    m_file.write(data);
    m_file.flush();
    rotateIfNeeded();
}

void LogFile::writeLine(const QString &line)
{
    write(QDateTime::currentDateTime().toString(Qt::ISODateWithMs).toUtf8() + ' ' + line.toUtf8() + '\n');
}

} // namespace leansip
