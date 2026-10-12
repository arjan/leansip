#include "baresip_process.h"

#include "config_deriver.h"

#include <QDir>
#include <QHostAddress>
#include <QLoggingCategory>
#include <QStandardPaths>
#include <QTcpServer>

#include <csignal>
#include <sys/prctl.h>

Q_LOGGING_CATEGORY(lcProcess, "leansip.process")

namespace leansip {

namespace {
constexpr int kTerminateGraceMs = 2000;
}

BaresipProcess::BaresipProcess(QObject *parent)
    : QObject(parent)
{
    // Forward baresip's own output to our stderr so the user can read SIP logs.
    m_process.setProcessChannelMode(QProcess::ForwardedChannels);
    m_process.setInputChannelMode(QProcess::ForwardedInputChannel);
    // Never leave an orphaned baresip registered with the provider if we die uncleanly
    // (SIGKILL, crash): the kernel delivers SIGTERM to the child when its parent exits.
    m_process.setChildProcessModifier([] { ::prctl(PR_SET_PDEATHSIG, SIGTERM); });
    connect(&m_process, &QProcess::finished, this, &BaresipProcess::finished);
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        m_error = m_process.errorString();
        qCWarning(lcProcess) << "baresip process error:" << m_error;
        emit errorOccurred(m_error);
    });
}

BaresipProcess::~BaresipProcess()
{
    stop();
}

quint16 BaresipProcess::pickFreePort()
{
    QTcpServer probe;
    if (!probe.listen(QHostAddress::LocalHost, 0))
        return 0;
    const quint16 port = probe.serverPort();
    probe.close();
    return port;
}

QString BaresipProcess::defaultDerivedDir()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (base.isEmpty())
        base = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    return QDir(base).filePath(QStringLiteral("leansip"));
}

bool BaresipProcess::start(const QString &userDir, const QString &binary)
{
    if (isRunning())
        return true;

    m_port = pickFreePort();
    if (m_port == 0) {
        m_error = QStringLiteral("could not allocate a loopback port");
        return false;
    }
    const QString listen = QStringLiteral("%1:%2").arg(host()).arg(m_port);
    const auto derived = config_deriver::deriveDirectory(userDir, defaultDerivedDir(), listen);
    if (!derived.ok) {
        m_error = derived.error;
        return false;
    }
    m_derivedDir = derived.dir;

    const QString resolved = QStandardPaths::findExecutable(binary);
    if (resolved.isEmpty()) {
        m_error = QStringLiteral("baresip executable '%1' not found in PATH").arg(binary);
        return false;
    }

    qCInfo(lcProcess) << "starting" << resolved << "-f" << m_derivedDir << "ctrl on" << listen;
    m_process.start(resolved, {QStringLiteral("-f"), m_derivedDir});
    if (!m_process.waitForStarted(3000)) {
        m_error = m_process.errorString();
        return false;
    }
    return true;
}

void BaresipProcess::stop()
{
    if (!isRunning())
        return;
    m_process.terminate();
    if (!m_process.waitForFinished(kTerminateGraceMs)) {
        qCWarning(lcProcess) << "baresip did not exit on SIGTERM, killing";
        m_process.kill();
        m_process.waitForFinished(1000);
    }
}

bool BaresipProcess::isRunning() const
{
    return m_process.state() != QProcess::NotRunning;
}

} // namespace leansip
