#include "unix_signals.h"

#include <csignal>
#include <sys/socket.h>
#include <unistd.h>

namespace leansip {

int UnixSignalBridge::s_fds[2] = {-1, -1};

UnixSignalBridge::UnixSignalBridge(QObject *parent)
    : QObject(parent)
{
}

UnixSignalBridge::~UnixSignalBridge()
{
    for (int &fd : s_fds) {
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }
}

bool UnixSignalBridge::install(std::initializer_list<int> signums)
{
    if (s_fds[0] < 0 && ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, s_fds) != 0)
        return false;

    m_notifier = new QSocketNotifier(s_fds[1], QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, [this] {
        unsigned char signum = 0;
        if (::read(s_fds[1], &signum, sizeof signum) == sizeof signum)
            emit signalReceived(signum);
    });

    for (int signum : signums) {
        struct sigaction current{};
        if (::sigaction(signum, nullptr, &current) == 0 && current.sa_handler == SIG_IGN)
            continue; // respect `nohup` and friends
        struct sigaction action{};
        action.sa_handler = &UnixSignalBridge::handler;
        ::sigemptyset(&action.sa_mask);
        action.sa_flags = SA_RESTART;
        ::sigaction(signum, &action, nullptr);
    }
    return true;
}

void UnixSignalBridge::handler(int signum)
{
    const unsigned char byte = static_cast<unsigned char>(signum);
    // Only async-signal-safe calls here; a failed write just means a missed shutdown request.
    [[maybe_unused]] const ssize_t n = ::write(s_fds[0], &byte, sizeof byte);
}

} // namespace leansip
