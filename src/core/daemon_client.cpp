#include "daemon_client.h"

#include "profile.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QStandardPaths>
#include <QThread>

#include <cerrno>
#include <csignal>
#include <unistd.h>

namespace leansip::daemon {

QString executable()
{
    const QString sibling = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("leansipd"));
    if (QFileInfo(sibling).isExecutable())
        return sibling;
    return QStandardPaths::findExecutable(QStringLiteral("leansipd"));
}

namespace {

// The daemon reports why it gave up as the last line of its log.
QString lastLogLine(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QStringLiteral("see %1").arg(path);
    if (f.size() > 4096)
        f.seek(f.size() - 4096);
    const QList<QByteArray> lines = f.readAll().trimmed().split('\n');
    return lines.isEmpty() || lines.last().isEmpty() ? QStringLiteral("see %1").arg(path)
                                                     : QString::fromUtf8(lines.last().trimmed());
}

} // namespace

bool isRunning(const QString &socketPath, int timeoutMs)
{
    QLocalSocket probe;
    probe.connectToServer(socketPath);
    const bool ok = probe.waitForConnected(timeoutMs);
    probe.abort();
    return ok;
}

bool ensureRunning(const QString &profileName, const QString &configPath, const QStringList &daemonArgs,
                   QString *error, bool *started, int timeoutMs)
{
    auto fail = [error](const QString &why) {
        if (error)
            *error = why;
        return false;
    };
    if (started)
        *started = false;
    const QString socket = profile::socketPath(profileName);
    if (isRunning(socket))
        return true;

    const QString binary = executable();
    if (binary.isEmpty())
        return fail(QStringLiteral("leansipd not found next to %1 or in PATH").arg(QCoreApplication::applicationFilePath()));

    const QString runtime = profile::runtimeDir(profileName);
    if (!QDir().mkpath(runtime))
        return fail(QStringLiteral("cannot create %1").arg(runtime));
    QFile::setPermissions(runtime, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);

    const QString log = profile::logPath(profileName);
    QProcess launcher;
    launcher.setProgram(binary);
    launcher.setArguments(QStringList{QStringLiteral("--profile"), profileName, QStringLiteral("--config"), configPath}
                          + daemonArgs);
    launcher.setStandardInputFile(QProcess::nullDevice());
    launcher.setStandardOutputFile(log, QIODevice::Append);
    launcher.setStandardErrorFile(log, QIODevice::Append);
    // Own session: no SIGHUP from the terminal or launcher that started us.
    launcher.setChildProcessModifier([] { ::setsid(); });
    qint64 pid = 0;
    if (!launcher.startDetached(&pid))
        return fail(QStringLiteral("cannot start %1: %2").arg(binary, launcher.errorString()));
    if (started)
        *started = true;

    // Startup can wait on password_cmd (an unlock prompt, say), hence the long default timeout;
    // a daemon that gives up is noticed right away.
    const QDeadlineTimer deadline(timeoutMs);
    while (!deadline.hasExpired()) {
        if (isRunning(socket, 200))
            return true;
        if (pid > 0 && ::kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH) {
            if (isRunning(socket, 200)) // another leansipd won the race and serves the socket
                return true;
            return fail(QStringLiteral("leansipd exited: %1").arg(lastLogLine(log)));
        }
        QThread::msleep(50);
    }
    return fail(QStringLiteral("leansipd did not come up within %1 s; see %2").arg(timeoutMs / 1000).arg(log));
}

bool Connection::open(const QString &socketPath, int timeoutMs, QString *error)
{
    m_socket.connectToServer(socketPath);
    if (m_socket.waitForConnected(timeoutMs))
        return true;
    if (error)
        *error = QStringLiteral("leansipd is not running (%1: %2)").arg(socketPath, m_socket.errorString());
    return false;
}

bool Connection::readSome(int timeoutMs)
{
    if (m_socket.bytesAvailable() == 0 && !m_socket.waitForReadyRead(timeoutMs))
        return false;
    const QList<QByteArray> frames = m_decoder.feed(m_socket.readAll());
    for (const QByteArray &frame : frames) {
        const QJsonObject obj = QJsonDocument::fromJson(frame).object();
        if (obj.value(QStringLiteral("event")).toBool())
            m_events.append(obj);
        else if (obj.value(QStringLiteral("response")).toBool())
            m_responses.append(obj);
    }
    return !m_decoder.failed();
}

std::optional<Reply> Connection::request(const QString &command, const QString &params, int timeoutMs, QString *error)
{
    auto fail = [error](const QString &why) -> std::optional<Reply> {
        if (error)
            *error = why;
        return std::nullopt;
    };
    if (!isOpen())
        return fail(QStringLiteral("not connected to leansipd"));
    const QString token = QStringLiteral("c%1").arg(m_nextToken++);
    QJsonObject obj;
    obj[QStringLiteral("command")] = command;
    obj[QStringLiteral("params")] = params;
    obj[QStringLiteral("token")] = token;
    m_socket.write(netstring::encode(QJsonDocument(obj).toJson(QJsonDocument::Compact)));
    if (!m_socket.waitForBytesWritten(timeoutMs) && m_socket.bytesToWrite() > 0)
        return fail(QStringLiteral("cannot write to leansipd: %1").arg(m_socket.errorString()));

    const QDeadlineTimer deadline(timeoutMs);
    for (;;) {
        for (qsizetype i = 0; i < m_responses.size(); ++i) {
            if (m_responses.at(i).value(QStringLiteral("token")).toString() != token)
                continue;
            const QJsonObject r = m_responses.takeAt(i);
            return Reply{r.value(QStringLiteral("ok")).toBool(), r.value(QStringLiteral("data")).toString(),
                         r.value(QStringLiteral("seq")).toInteger()};
        }
        if (deadline.hasExpired())
            return fail(QStringLiteral("no reply to %1 within %2 ms").arg(command).arg(timeoutMs));
        if (!readSome(static_cast<int>(deadline.remainingTime()))) {
            if (m_decoder.failed())
                return fail(QStringLiteral("bad frame from leansipd: %1").arg(m_decoder.lastError()));
            if (!isOpen())
                return fail(QStringLiteral("leansipd closed the connection"));
        }
    }
}

std::optional<QJsonObject> Connection::nextEvent(int timeoutMs)
{
    const QDeadlineTimer deadline(timeoutMs);
    while (m_events.isEmpty()) {
        if (deadline.hasExpired() || !isOpen())
            return std::nullopt;
        if (!readSome(static_cast<int>(deadline.remainingTime())) && (m_decoder.failed() || !isOpen()))
            return std::nullopt;
    }
    return m_events.takeFirst();
}

} // namespace leansip::daemon
