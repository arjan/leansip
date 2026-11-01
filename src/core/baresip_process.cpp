#include "baresip_process.h"

#include "config_deriver.h"
#include "dtmf_tone.h"

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
    connect(&m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        if (!m_stopping) // a deliberate stop()/restart() is not something the UI should report
            emit finished(code, status);
    });
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        m_error = m_process.errorString();
        qCWarning(lcProcess) << "baresip process error:" << m_error;
        emit errorOccurred(m_error);
    });
}

void BaresipProcess::setCaptureOutput(bool capture)
{
    m_process.setProcessChannelMode(capture ? QProcess::MergedChannels : QProcess::ForwardedChannels);
    disconnect(&m_process, &QProcess::readyReadStandardOutput, this, nullptr);
    if (capture) {
        connect(&m_process, &QProcess::readyReadStandardOutput, this,
                [this] { emit outputReceived(m_process.readAllStandardOutput()); });
    }
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

bool BaresipProcess::start(const config_deriver::Setup &setup, const QString &binary)
{
    if (isRunning())
        return true;
    m_setup = setup;
    m_started = true;
    m_binary = binary;

    m_port = pickFreePort();
    if (m_port == 0) {
        m_error = QStringLiteral("could not allocate a loopback port");
        return false;
    }
    const QString listen = QStringLiteral("%1:%2").arg(host()).arg(m_port);
    const QString derivedDir = m_derivedDir.isEmpty() ? defaultDerivedDir() : m_derivedDir;
    if (!m_options.recordDir.isEmpty() && !QDir().mkpath(m_options.recordDir)) {
        m_error = QStringLiteral("cannot create %1").arg(m_options.recordDir);
        return false;
    }
    const auto derived = config_deriver::writeDirectory(setup, derivedDir, listen, m_options);
    if (!derived.ok) {
        m_error = derived.error;
        return false;
    }
    m_derivedDir = derived.dir;
    if (!dtmf::installSoundBank(derived.soundsDir))
        qCWarning(lcProcess) << "could not write DTMF tones to" << derived.soundsDir << "- keys will be silent";

    const QString resolved = QStandardPaths::findExecutable(binary);
    if (resolved.isEmpty()) {
        m_error = QStringLiteral("baresip executable '%1' not found in PATH").arg(binary);
        return false;
    }

    qCInfo(lcProcess) << "starting" << resolved << "-f" << m_derivedDir << "ctrl on" << listen;
    m_stopping = false;
    m_process.start(resolved, QStringList{QStringLiteral("-f"), m_derivedDir} + m_extraArgs);
    if (!m_process.waitForStarted(3000)) {
        m_error = m_process.errorString();
        return false;
    }
    return true;
}

bool BaresipProcess::restart()
{
    if (!m_started) {
        m_error = QStringLiteral("baresip was never started");
        return false;
    }
    stop();
    return start(m_setup, m_binary);
}

void BaresipProcess::stop()
{
    if (!isRunning())
        return;
    m_stopping = true;
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
