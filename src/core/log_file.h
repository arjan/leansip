#pragma once

#include <QFile>
#include <QString>

namespace leansip {

// Append-only log with one rotation: past `maxBytes` the file becomes "<path>.1" (replacing an
// older one) and a fresh file starts. Bounds what a long --sip-trace session keeps around.
class LogFile {
public:
    explicit LogFile(QString path, qint64 maxBytes = 5 * 1024 * 1024);

    bool open(QString *error);
    void write(const QByteArray &data);
    void writeLine(const QString &line);
    QString path() const { return m_path; }

private:
    void rotateIfNeeded();

    QString m_path;
    qint64 m_maxBytes;
    QFile m_file;
};

} // namespace leansip
