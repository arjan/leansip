#pragma once

#include "accounts_parser.h"
#include "baresip_backend.h"
#include "baresip_client.h"

#include <QHash>
#include <QObject>
#include <QTimer>

namespace leansip {

// Validates an edited accounts file by actually registering: the candidate is handed to a
// restarted baresip (via the derived dir, never ~/.baresip), and the first/active account must
// reach REGISTER_OK. Only then is the user's accounts file rewritten; on failure the previous
// state is restored by another restart.
class AccountValidator : public QObject {
    Q_OBJECT
public:
    AccountValidator(BaresipBackend *backend, BaresipClient *client, QString userAccountsPath,
                     QObject *parent = nullptr);

    void setTimeoutMs(int ms) { m_timeoutMs = ms; }
    bool isBusy() const { return m_busy; }

public slots:
    void validate(const leansip::AccountsFile &candidate);
    // User gave up waiting: restore the previous accounts and report failure.
    void abort();

signals:
    void progress(const QString &text);
    void accountResult(const QString &aor, bool ok, const QString &detail);
    void finished(bool ok, const QString &summary);

private:
    void onEvent(const BaresipEvent &event);
    void conclude(bool ok, const QString &summary);
    bool reconnect();

    BaresipBackend *m_backend;
    BaresipClient *m_client;
    QString m_userAccountsPath;
    AccountsFile m_candidate;
    QString m_activeAor;
    QTimer m_timer;
    int m_timeoutMs = 10000;
    bool m_busy = false;
};

} // namespace leansip
