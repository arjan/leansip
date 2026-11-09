#pragma once

#include "config_deriver.h"

#include <QObject>
#include <QProcess>
#include <QString>

namespace leansip {

// Owns a child `baresip` started from a derived config directory with ctrl_tcp on loopback.
class BaresipProcess : public QObject {
    Q_OBJECT
public:
    explicit BaresipProcess(QObject *parent = nullptr);
    ~BaresipProcess() override;

    // Writes `setup` into the derived directory and starts baresip on it. An empty `binary`
    // means the one baresip_binary::locate() finds (embedded, configured, or in PATH).
    bool start(const config_deriver::Setup &setup, const QString &binary = QString());

    // Graceful stop: terminate, then kill after a short grace period. Blocks briefly.
    void stop();

    // Stops and starts again with the setup of the last start().
    bool restart();
    QString host() const { return QStringLiteral("127.0.0.1"); }
    quint16 port() const { return m_port; }
    QString lastError() const { return m_error; }

    bool isRunning() const;
    QString derivedDir() const { return m_derivedDir; }
    // Where to put the derived config instead of defaultDerivedDir(); call before start().
    void setDerivedDir(const QString &dir) { m_derivedDir = dir; }
    // Extra baresip arguments, e.g. {"-s"} for a SIP trace on stderr; call before start().
    void setExtraArgs(const QStringList &args) { m_extraArgs = args; }
    qint64 pid() const { return m_process.processId(); }
    // Record every call into `dir` (sndfile.so); empty disables. Call before start()/restart().
    void setRecordDir(const QString &dir) { m_options.recordDir = dir; }
    // Deliver baresip's stdout/stderr through outputReceived() instead of forwarding it to ours.
    void setCaptureOutput(bool capture);

    // Picks a free loopback TCP port. Exposed for tests.
    static quint16 pickFreePort();
    // Where the derived config goes: $XDG_RUNTIME_DIR/leansip or a temp fallback.
    static QString defaultDerivedDir();


signals:
    void finished(int exitCode, QProcess::ExitStatus status);
    void errorOccurred(const QString &error);
    void outputReceived(const QByteArray &data);

private:
    QProcess m_process;
    config_deriver::Setup m_setup;
    bool m_started = false;
    QString m_binary;
    QString m_derivedDir;
    QStringList m_extraArgs;
    config_deriver::Options m_options;
    QString m_error;
    quint16 m_port = 0;
    bool m_stopping = false;
};

} // namespace leansip
