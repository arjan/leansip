#pragma once

#include "core/baresip_backend.h"
#include "fake_baresip_server.h"

#include <QList>
#include <memory>

// Stands in for BaresipProcess: every restart() brings up a fresh fake ctrl_tcp server.
class FakeBackend : public leansip::BaresipBackend {
public:
    explicit FakeBackend(const QString &derivedAccounts)
        : m_derivedAccounts(derivedAccounts)
    {
    }
    bool restart() override
    {
        ++restarts;
        if (failNextRestart) {
            failNextRestart = false;
            m_error = QStringLiteral("simulated restart failure");
            return false;
        }
        server = std::make_unique<FakeBaresipServer>();
        return server->listen();
    }
    QString host() const override { return QStringLiteral("127.0.0.1"); }
    quint16 port() const override { return server ? server->port() : 0; }
    QString derivedAccountsPath() const override { return m_derivedAccounts; }
    void setAccountsOverride(bool enabled) override
    {
        overrideHistory.append(enabled);
        overrideEnabled = enabled;
    }
    QString lastError() const override { return m_error; }

    std::unique_ptr<FakeBaresipServer> server;
    int restarts = 0;
    bool failNextRestart = false;
    bool overrideEnabled = false;
    QList<bool> overrideHistory;

private:
    QString m_derivedAccounts;
    QString m_error;
};
