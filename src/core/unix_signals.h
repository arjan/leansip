#pragma once

#include <QObject>
#include <QSocketNotifier>

#include <initializer_list>

namespace leansip {

// Turns Unix signals into a Qt signal so the normal shutdown path (aboutToQuit) runs on
// SIGINT/SIGTERM/SIGHUP. Uses the self-pipe pattern: the handler only writes a byte.
// Signals that are ignored when install() runs (e.g. under nohup) are left ignored.
class UnixSignalBridge : public QObject {
    Q_OBJECT
public:
    explicit UnixSignalBridge(QObject *parent = nullptr);
    ~UnixSignalBridge() override;

    // Returns false if the pipe could not be created; otherwise installs handlers.
    bool install(std::initializer_list<int> signums);

signals:
    void signalReceived(int signum);

private:
    static void handler(int signum);
    static int s_fds[2];
    QSocketNotifier *m_notifier = nullptr;
};

} // namespace leansip
