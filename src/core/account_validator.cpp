#include "account_validator.h"

#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcValidate, "leansip.validate")

namespace leansip {

AccountValidator::AccountValidator(BaresipBackend *backend, BaresipClient *client, QString userAccountsPath,
                                   QObject *parent)
    : QObject(parent)
    , m_backend(backend)
    , m_client(client)
    , m_userAccountsPath(std::move(userAccountsPath))
{
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, [this] {
        if (m_busy)
            conclude(false, QStringLiteral("no answer from the server within %1 s").arg(m_timeoutMs / 1000));
    });
    connect(m_client, &BaresipClient::eventReceived, this, &AccountValidator::onEvent);
    connect(m_client, &BaresipClient::connectionFailed, this, [this](const QString &err) {
        if (m_busy)
            conclude(false, QStringLiteral("cannot reach baresip after restart: %1").arg(err));
    });
}

void AccountValidator::validate(const AccountsFile &candidate)
{
    if (m_busy)
        return;
    if (candidate.accounts.isEmpty()) {
        emit finished(false, QStringLiteral("no accounts to register"));
        return;
    }
    for (const AccountRecord &record : candidate.accounts) {
        QString why;
        if (!accounts::validateRecord(record, &why)) {
            emit finished(false, QStringLiteral("%1: %2").arg(record.aor(), why));
            return;
        }
    }

    m_busy = true;
    m_candidate = candidate;
    m_activeAor = candidate.accounts.first().aor();

    // The candidate lives only in baresip's private derived dir until it has proven itself.
    QString error;
    if (!accounts::writeFile(m_backend->derivedAccountsPath(), candidate, &error)) {
        m_busy = false;
        emit finished(false, error);
        return;
    }
    m_backend->setAccountsOverride(true);

    emit progress(QStringLiteral("restarting baresip…"));
    m_backend->detachClient(m_client);
    if (!m_backend->restart()) {
        conclude(false, QStringLiteral("could not restart baresip: %1").arg(m_backend->lastError()));
        return;
    }
    if (!reconnect())
        return;
    emit progress(QStringLiteral("registering %1…").arg(m_activeAor));
    m_timer.start(m_timeoutMs);
}

bool AccountValidator::reconnect()
{
    m_backend->attachClient(m_client);
    return true;
}

void AccountValidator::abort()
{
    if (m_busy)
        conclude(false, QStringLiteral("cancelled"));
}

void AccountValidator::onEvent(const BaresipEvent &ev)
{
    if (!m_busy)
        return;
    if (ev.type != QLatin1String("REGISTER_OK") && ev.type != QLatin1String("REGISTER_FAIL"))
        return;
    const bool ok = ev.type == QLatin1String("REGISTER_OK");
    const QString detail = ev.param.trimmed();
    qCInfo(lcValidate) << ev.accountAor << (ok ? "registered" : "failed") << detail;
    emit accountResult(ev.accountAor, ok, detail);
    if (ev.accountAor == m_activeAor)
        conclude(ok, ok ? QStringLiteral("registered") : (detail.isEmpty() ? QStringLiteral("registration failed") : detail));
}

void AccountValidator::conclude(bool ok, const QString &summary)
{
    m_timer.stop();
    m_busy = false;
    QString finalSummary = summary;

    if (ok) {
        // Promote the candidate: it is what baresip is running right now, so no restart needed.
        QString error;
        if (accounts::writeFile(m_userAccountsPath, m_candidate, &error)) {
            m_backend->setAccountsOverride(false);
        } else {
            ok = false;
            finalSummary = QStringLiteral("registered, but saving failed: %1").arg(error);
        }
    }
    if (!ok) {
        // Back to the user's own file: relink and restart so the dialer is usable again.
        emit progress(QStringLiteral("restoring previous accounts…"));
        m_backend->setAccountsOverride(false);
        m_backend->detachClient(m_client);
        if (!m_backend->restart())
            qCWarning(lcValidate) << "restore restart failed:" << m_backend->lastError();
        else
            reconnect();
    }
    emit finished(ok, finalSummary);
}

} // namespace leansip
