#pragma once

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

    // `userDir` is the user's baresip directory (default ~/.baresip).
    // `binary` defaults to "baresip" resolved via PATH.
    bool start(const QString &userDir, const QString &binary = QStringLiteral("baresip"));

    // Graceful stop: terminate, then kill after a short grace period. Blocks briefly.
    void stop();

    bool isRunning() const;
    quint16 port() const { return m_port; }
    QString host() const { return QStringLiteral("127.0.0.1"); }
    QString derivedDir() const { return m_derivedDir; }
    QString lastError() const { return m_error; }

    // Picks a free loopback TCP port. Exposed for tests.
    static quint16 pickFreePort();
    // Where the derived config goes: $XDG_RUNTIME_DIR/leansip or a temp fallback.
    static QString defaultDerivedDir();

signals:
    void finished(int exitCode, QProcess::ExitStatus status);
    void errorOccurred(const QString &error);

private:
    QProcess m_process;
    QString m_derivedDir;
    QString m_error;
    quint16 m_port = 0;
};

} // namespace leansip
