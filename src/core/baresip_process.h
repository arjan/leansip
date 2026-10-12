#pragma once

#include "baresip_backend.h"

#include <QObject>
#include <QProcess>
#include <QString>

namespace leansip {

// Owns a child `baresip` started from a derived config directory with ctrl_tcp on loopback.
class BaresipProcess : public QObject, public BaresipBackend {
    Q_OBJECT
public:
    explicit BaresipProcess(QObject *parent = nullptr);
    ~BaresipProcess() override;

    // `userDir` is the user's baresip directory (default ~/.baresip).
    // `binary` defaults to "baresip" resolved via PATH.
    bool start(const QString &userDir, const QString &binary = QStringLiteral("baresip"));

    // Graceful stop: terminate, then kill after a short grace period. Blocks briefly.
    void stop();

    // BaresipBackend
    bool restart() override;
    QString host() const override { return QStringLiteral("127.0.0.1"); }
    quint16 port() const override { return m_port; }
    QString derivedAccountsPath() const override;
    void setAccountsOverride(bool enabled) override { m_accountsOverride = enabled; }
    QString lastError() const override { return m_error; }

    bool isRunning() const;
    QString derivedDir() const { return m_derivedDir; }
    // Where to put the derived config instead of defaultDerivedDir(); call before start().
    void setDerivedDir(const QString &dir) { m_derivedDir = dir; }

    // Picks a free loopback TCP port. Exposed for tests.
    static quint16 pickFreePort();
    // Where the derived config goes: $XDG_RUNTIME_DIR/leansip or a temp fallback.
    static QString defaultDerivedDir();

    // First run: if `<userDir>/config` is missing, lets baresip write its own template config
    // (`baresip -f <userDir> -t 1`). Blocks for up to a few seconds.
    static bool ensureUserConfig(const QString &userDir, QString *error,
                                 const QString &binary = QStringLiteral("baresip"));

signals:
    void finished(int exitCode, QProcess::ExitStatus status);
    void errorOccurred(const QString &error);

private:
    QProcess m_process;
    QString m_userDir;
    QString m_binary;
    QString m_derivedDir;
    QString m_error;
    quint16 m_port = 0;
    bool m_accountsOverride = false;
    bool m_stopping = false;
};

} // namespace leansip
