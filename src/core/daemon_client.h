#pragma once

#include "netstring.h"

#include <QJsonObject>
#include <QList>
#include <QLocalSocket>
#include <QString>
#include <QStringList>

#include <optional>

namespace leansip::daemon {

// Path of the leansipd binary: next to the running executable, else from PATH.
QString executable();

// True while process `pid` runs. An exited process that nobody has reaped yet (a zombie, as
// under a container's init that does not reap orphans) counts as gone.
bool processAlive(qint64 pid);

// True when a leansipd accepts connections on `socketPath`.
bool isRunning(const QString &socketPath, int timeoutMs = 300);

// Starts leansipd for a profile of `configPath` unless one already runs, and waits until it
// accepts connections or exits. The daemon gets its own session (so a closing terminal or
// launcher does not take it down) and logs to profile::logPath(). `daemonArgs` go to leansipd,
// e.g. "--ui-owned". `started`, if given, says whether this call launched it.
bool ensureRunning(const QString &profileName, const QString &configPath, const QStringList &daemonArgs,
                   QString *error, bool *started = nullptr, int timeoutMs = 90000);

struct Reply {
    bool ok = false;
    QString data;
    qint64 seq = 0;
};

// Blocking client for leansipd, for command-line use and short synchronous requests.
// Events that arrive while waiting for a reply are queued for nextEvent().
class Connection {
public:
    bool open(const QString &socketPath, int timeoutMs, QString *error);
    bool isOpen() const { return m_socket.state() == QLocalSocket::ConnectedState; }

    std::optional<Reply> request(const QString &command, const QString &params, int timeoutMs, QString *error);
    // Next event, from the queue or the socket; nullopt on timeout or disconnect.
    std::optional<QJsonObject> nextEvent(int timeoutMs);

private:
    bool readSome(int timeoutMs);

    QLocalSocket m_socket;
    netstring::Decoder m_decoder;
    QList<QJsonObject> m_events;
    QList<QJsonObject> m_responses;
    quint64 m_nextToken = 1;
};

} // namespace leansip::daemon
