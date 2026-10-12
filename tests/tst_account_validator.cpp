#include "core/account_validator.h"
#include "fake_backend.h"

#include <QSignalSpy>
#include <QtTest>

using namespace leansip;

class TestAccountValidator : public QObject {
    Q_OBJECT
private slots:
    void init()
    {
        m_dir = std::make_unique<QTemporaryDir>();
        QVERIFY(m_dir->isValid());
        m_userPath = m_dir->filePath("user/accounts");
        m_backend = std::make_unique<FakeBackend>(m_dir->filePath("derived/accounts"));
        m_client = std::make_unique<BaresipClient>();
        m_validator = std::make_unique<AccountValidator>(m_backend.get(), m_client.get(), m_userPath);
        m_validator->setTimeoutMs(800);

        // Pre-existing user file so we can prove it is untouched on failure.
        AccountsFile original;
        original.accounts.append(record("old", "example.com", "oldpass"));
        QString error;
        QVERIFY2(accounts::writeFile(m_userPath, original, &error), qPrintable(error));
        m_originalText = readAll(m_userPath);
    }

    void successWritesUserFileWithoutRestoreRestart()
    {
        AccountsFile candidate;
        candidate.accounts.append(record("alice", "example.com", "s3cret"));
        candidate.accounts.append(record("bob", "example.com", "x"));

        QSignalSpy finished(m_validator.get(), &AccountValidator::finished);
        QSignalSpy results(m_validator.get(), &AccountValidator::accountResult);
        QSignalSpy progress(m_validator.get(), &AccountValidator::progress);
        m_validator->validate(candidate);
        QVERIFY(m_validator->isBusy());
        QCOMPARE(m_backend->restarts, 1);
        QVERIFY(m_backend->overrideEnabled);
        // The candidate went to the derived dir, not the user's file.
        QVERIFY(readAll(m_backend->derivedAccountsPath()).contains("alice@example.com"));
        QCOMPARE(readAll(m_userPath), m_originalText);

        QTRY_VERIFY(m_backend->server && m_backend->server->hasClient());
        m_backend->server->sendEvent("REGISTER_FAIL", {{"class", "register"}, {"accountaor", "sip:bob@example.com"}, {"param", "401 Unauthorized"}});
        m_backend->server->sendEvent("REGISTER_OK", {{"class", "register"}, {"accountaor", "sip:alice@example.com"}, {"param", "200 OK\n"}});

        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(finished.first().at(0).toBool());
        QCOMPARE(finished.first().at(1).toString(), "registered");
        QCOMPARE(results.size(), 2);
        QCOMPARE(results.at(0).at(0).toString(), "sip:bob@example.com");
        QVERIFY(!results.at(0).at(1).toBool()); // a secondary failure does not block
        QCOMPARE(results.at(1).at(2).toString(), "200 OK");
        QVERIFY(!m_validator->isBusy());
        QCOMPARE(m_backend->restarts, 1); // no restore needed
        QVERIFY(!m_backend->overrideEnabled);
        const QString saved = readAll(m_userPath);
        QVERIFY(saved.contains("alice@example.com"));
        QVERIFY(saved.contains("auth_pass=s3cret"));
        QVERIFY(saved.startsWith("<sip:alice@")); // candidate order kept: alice is active
        QVERIFY(progress.size() >= 2);
    }

    void activeFailureRestoresAndLeavesUserFile()
    {
        AccountsFile candidate;
        candidate.accounts.append(record("alice", "example.com", "wrong"));
        QSignalSpy finished(m_validator.get(), &AccountValidator::finished);
        m_validator->validate(candidate);
        QTRY_VERIFY(m_backend->server && m_backend->server->hasClient());
        m_backend->server->sendEvent("REGISTER_FAIL", {{"class", "register"}, {"accountaor", "sip:alice@example.com"}, {"param", "401 Unauthorized"}});

        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(!finished.first().at(0).toBool());
        QCOMPARE(finished.first().at(1).toString(), "401 Unauthorized");
        QCOMPARE(readAll(m_userPath), m_originalText);
        QCOMPARE(m_backend->restarts, 2); // validation + restore
        QCOMPARE(m_backend->overrideHistory, (QList<bool>{true, false}));
        // The client is reconnected to the restored backend.
        QTRY_VERIFY(m_backend->server->hasClient());
    }

    void timeoutFailsAndRestores()
    {
        AccountsFile candidate;
        candidate.accounts.append(record("alice", "example.com", "p"));
        QSignalSpy finished(m_validator.get(), &AccountValidator::finished);
        m_validator->validate(candidate);
        QTRY_VERIFY(m_backend->server && m_backend->server->hasClient());
        // Silence from the server.
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 3000);
        QVERIFY(!finished.first().at(0).toBool());
        QVERIFY(finished.first().at(1).toString().contains("no answer"));
        QCOMPARE(m_backend->restarts, 2);
        QCOMPARE(readAll(m_userPath), m_originalText);
    }

    void abortRestores()
    {
        AccountsFile candidate;
        candidate.accounts.append(record("alice", "example.com", "p"));
        QSignalSpy finished(m_validator.get(), &AccountValidator::finished);
        m_validator->validate(candidate);
        QTRY_VERIFY(m_backend->server && m_backend->server->hasClient());
        m_validator->abort();
        QCOMPARE(finished.size(), 1);
        QCOMPARE(finished.first().at(1).toString(), "cancelled");
        QCOMPARE(m_backend->restarts, 2);
        QVERIFY(!m_validator->isBusy());
        m_validator->abort(); // idle: no-op
        QCOMPARE(finished.size(), 1);
    }

    void restartFailureIsReported()
    {
        AccountsFile candidate;
        candidate.accounts.append(record("alice", "example.com", "p"));
        m_backend->failNextRestart = true;
        QSignalSpy finished(m_validator.get(), &AccountValidator::finished);
        m_validator->validate(candidate);
        QCOMPARE(finished.size(), 1);
        QVERIFY(!finished.first().at(0).toBool());
        QVERIFY(finished.first().at(1).toString().contains("simulated restart failure"));
        QCOMPARE(readAll(m_userPath), m_originalText);
        QVERIFY(!m_validator->isBusy());
    }

    void rejectsInvalidOrEmptyCandidateWithoutRestarting()
    {
        QSignalSpy finished(m_validator.get(), &AccountValidator::finished);
        m_validator->validate(AccountsFile{});
        QCOMPARE(finished.size(), 1);
        QVERIFY(finished.last().at(1).toString().contains("no accounts"));

        AccountsFile bad;
        bad.accounts.append(record("", "example.com", "p"));
        m_validator->validate(bad);
        QCOMPARE(finished.size(), 2);
        QVERIFY(finished.last().at(1).toString().contains("user is required"));
        QCOMPARE(m_backend->restarts, 0);
        QVERIFY(!QFile::exists(m_backend->derivedAccountsPath()));
    }

    void ignoresSecondValidateWhileBusy()
    {
        AccountsFile candidate;
        candidate.accounts.append(record("alice", "example.com", "p"));
        m_validator->validate(candidate);
        m_validator->validate(candidate);
        QCOMPARE(m_backend->restarts, 1);
        m_validator->abort();
    }

private:
    static AccountRecord record(const QString &user, const QString &domain, const QString &password)
    {
        AccountRecord r;
        r.user = user;
        r.domain = domain;
        r.password = password;
        return r;
    }
    static QString readAll(const QString &path)
    {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            return {};
        return QString::fromUtf8(f.readAll());
    }

    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<FakeBackend> m_backend;
    std::unique_ptr<BaresipClient> m_client;
    std::unique_ptr<AccountValidator> m_validator;
    QString m_userPath;
    QString m_originalText;
};

QTEST_GUILESS_MAIN(TestAccountValidator)
#include "tst_account_validator.moc"
