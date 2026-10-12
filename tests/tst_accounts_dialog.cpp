#include "fake_backend.h"
#include "ui/accounts_dialog.h"

#include <QSignalSpy>
#include <QtTest>

using namespace leansip;

class TestAccountsDialog : public QObject {
    Q_OBJECT
private slots:
    void init()
    {
        m_dir = std::make_unique<QTemporaryDir>();
        QVERIFY(m_dir->isValid());
        m_userPath = m_dir->filePath("accounts");
        m_backend = std::make_unique<FakeBackend>(m_dir->filePath("derived-accounts"));
        m_client = std::make_unique<BaresipClient>();
        m_validator = std::make_unique<AccountValidator>(m_backend.get(), m_client.get(), m_userPath);
        m_validator->setTimeoutMs(800);

        AccountsFile initial;
        initial.preamble = {"# header"};
        initial.accounts.append(record("Alice", "alice", "example.com", "pw1"));
        initial.accounts.append(record("", "bob", "example.com", "pw2"));
        m_dialog = std::make_unique<AccountsDialog>(initial, m_validator.get());
        m_dialog->show();
        QVERIFY(QTest::qWaitForWindowActive(m_dialog.get()));
    }

    void cleanup()
    {
        m_dialog.reset();
        m_validator.reset();
        m_client.reset();
        m_backend.reset();
    }

    void showsAccountsAndLoadsFirstIntoForm()
    {
        QVERIFY(m_dialog->windowFlags().testFlag(Qt::FramelessWindowHint));
        QCOMPARE(m_dialog->list()->count(), 2);
        QVERIFY(m_dialog->list()->item(0)->text().contains("Alice"));
        QVERIFY(m_dialog->list()->item(0)->text().contains("active"));
        QVERIFY(!m_dialog->list()->item(1)->text().contains("active"));
        QCOMPARE(m_dialog->list()->currentRow(), 0);
        QCOMPARE(m_dialog->userEdit()->text(), "alice");
        QCOMPARE(m_dialog->passwordEdit()->text(), "pw1");
        QCOMPARE(m_dialog->passwordEdit()->echoMode(), QLineEdit::Password);
        QVERIFY(m_dialog->list()->hasFocus());
    }

    void editingFieldsUpdatesWorkingCopyAndRow()
    {
        m_dialog->displayNameEdit()->setFocus();
        m_dialog->displayNameEdit()->selectAll();
        QTest::keyClicks(m_dialog->displayNameEdit(), "Alicia");
        QCOMPARE(m_dialog->working().accounts[0].displayName, "Alicia");
        QVERIFY(m_dialog->list()->item(0)->text().contains("Alicia"));

        m_dialog->transportBox()->setCurrentIndex(m_dialog->transportBox()->findData("tls"));
        QCOMPARE(m_dialog->working().accounts[0].transport, "tls");

        // Switching rows loads the other record without bleeding edits across.
        QTest::keyClick(m_dialog->list(), Qt::Key_Down);
        QCOMPARE(m_dialog->list()->currentRow(), 1);
        QCOMPARE(m_dialog->userEdit()->text(), "bob");
        QCOMPARE(m_dialog->displayNameEdit()->text(), "");
        QCOMPARE(m_dialog->working().accounts[0].displayName, "Alicia");
    }

    void addRemoveMoveAndMakeActive()
    {
        QTest::keyClick(m_dialog->list(), Qt::Key_N, Qt::ControlModifier);
        QCOMPARE(m_dialog->list()->count(), 3);
        QCOMPARE(m_dialog->list()->currentRow(), 2);
        QVERIFY(m_dialog->userEdit()->hasFocus());
        QTest::keyClicks(m_dialog->userEdit(), "carol");
        QCOMPARE(m_dialog->working().accounts[2].user, "carol");

        // Ctrl+Up moves carol above bob.
        QTest::keyClick(m_dialog->userEdit(), Qt::Key_Up, Qt::ControlModifier);
        QCOMPARE(m_dialog->working().accounts[1].user, "carol");
        QCOMPARE(m_dialog->list()->currentRow(), 1);

        // Enter on the list makes the selection active (first).
        QTest::keyClick(m_dialog->list(), Qt::Key_Return);
        QCOMPARE(m_dialog->working().accounts[0].user, "carol");
        QCOMPARE(m_dialog->working().accounts[1].user, "alice");
        QVERIFY(m_dialog->list()->item(0)->text().contains("active"));
        QCOMPARE(m_dialog->list()->currentRow(), 0);

        // Del removes the current one.
        QTest::keyClick(m_dialog->list(), Qt::Key_Delete);
        QCOMPARE(m_dialog->working().accounts.size(), 2);
        QCOMPARE(m_dialog->working().accounts[0].user, "alice");
        QCOMPARE(m_dialog->list()->currentRow(), 0);
    }

    void saveRejectsInvalidRecordLocally()
    {
        m_dialog->userEdit()->setFocus();
        m_dialog->userEdit()->clear();
        QTest::keyClicks(m_dialog->userEdit(), "a b");
        QTest::keyClick(m_dialog->userEdit(), Qt::Key_S, Qt::ControlModifier);
        QVERIFY(m_dialog->statusLabel()->text().contains("must not contain"));
        QCOMPARE(m_backend->restarts, 0);
        QVERIFY(m_dialog->isVisible());
    }

    void saveValidatesAndClosesOnSuccess()
    {
        QSignalSpy saved(m_dialog.get(), &AccountsDialog::saved);
        QTest::keyClick(m_dialog->list(), Qt::Key_S, Qt::ControlModifier);
        QCOMPARE(m_backend->restarts, 1);
        QVERIFY(!m_dialog->canSave()); // busy
        QVERIFY(!m_dialog->userEdit()->isEnabled());
        QVERIFY(m_dialog->statusLabel()->text().contains("registering"));

        QTRY_VERIFY(m_backend->server && m_backend->server->hasClient());
        m_backend->server->sendEvent("REGISTER_FAIL", {{"class", "register"}, {"accountaor", "sip:bob@example.com"}, {"param", "403"}});
        QTRY_VERIFY(m_dialog->list()->item(1)->text().startsWith("✗"));
        m_backend->server->sendEvent("REGISTER_OK", {{"class", "register"}, {"accountaor", "sip:alice@example.com"}, {"param", "200 OK"}});

        QTRY_COMPARE(saved.size(), 1);
        QCOMPARE(m_dialog->result(), QDialog::Accepted);
        QVERIFY(!m_dialog->isVisible());
        QFile f(m_userPath);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(f.readAll());
        QVERIFY(text.startsWith("# header\n\"Alice\" <sip:alice@example.com>"));
    }

    void saveFailureStaysOpenWithReason()
    {
        QSignalSpy saved(m_dialog.get(), &AccountsDialog::saved);
        QTest::keyClick(m_dialog->list(), Qt::Key_S, Qt::ControlModifier);
        QTRY_VERIFY(m_backend->server && m_backend->server->hasClient());
        m_backend->server->sendEvent("REGISTER_FAIL", {{"class", "register"}, {"accountaor", "sip:alice@example.com"}, {"param", "401 Unauthorized"}});
        QTRY_VERIFY(m_dialog->statusLabel()->text().contains("401 Unauthorized"));
        QVERIFY(m_dialog->statusLabel()->text().startsWith("✗"));
        QCOMPARE(saved.size(), 0);
        QVERIFY(m_dialog->isVisible());
        QVERIFY(m_dialog->userEdit()->isEnabled());
        QVERIFY(m_dialog->canSave());
        QVERIFY(!QFile::exists(m_userPath));
        QCOMPARE(m_backend->restarts, 2); // restored
    }

    void escapeDuringValidationAbortsThenEscapeCloses()
    {
        QTest::keyClick(m_dialog->list(), Qt::Key_S, Qt::ControlModifier);
        QVERIFY(m_validator->isBusy());
        QTest::keyClick(m_dialog->list(), Qt::Key_Escape);
        QVERIFY(!m_validator->isBusy());
        QVERIFY(m_dialog->isVisible()); // aborted, not closed
        QVERIFY(m_dialog->statusLabel()->text().contains("cancelled"));
        QTest::keyClick(m_dialog->list(), Qt::Key_Escape);
        QTRY_VERIFY(!m_dialog->isVisible());
        QCOMPARE(m_dialog->result(), QDialog::Rejected);
    }

    void passwordEchoToggle()
    {
        QTest::keyClick(m_dialog->list(), Qt::Key_H, Qt::ControlModifier);
        QCOMPARE(m_dialog->passwordEdit()->echoMode(), QLineEdit::Normal);
        QTest::keyClick(m_dialog->passwordEdit(), Qt::Key_H, Qt::ControlModifier);
        QCOMPARE(m_dialog->passwordEdit()->echoMode(), QLineEdit::Password);
    }

    void refusesToSaveDuringCall()
    {
        m_dialog->setCallActive(true);
        QVERIFY(!m_dialog->canSave());
        QTest::keyClick(m_dialog->list(), Qt::Key_S, Qt::ControlModifier);
        QCOMPARE(m_backend->restarts, 0);
        QVERIFY(m_dialog->statusLabel()->text().contains("hang up"));
    }

    void emptyInitialFileStartsWithBlankAccount()
    {
        m_dialog->hide();
        auto blank = std::make_unique<AccountsDialog>(AccountsFile{}, m_validator.get());
        blank->show();
        QVERIFY(QTest::qWaitForWindowActive(blank.get()));
        QCOMPARE(blank->list()->count(), 1);
        QCOMPARE(blank->working().accounts.size(), 1);
        QVERIFY(blank->working().accounts[0].user.isEmpty());
        QVERIFY(blank->userEdit()->hasFocus() || blank->list()->hasFocus());
    }

private:
    static AccountRecord record(const QString &name, const QString &user, const QString &domain, const QString &password)
    {
        AccountRecord r;
        r.displayName = name;
        r.user = user;
        r.domain = domain;
        r.password = password;
        return r;
    }

    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<FakeBackend> m_backend;
    std::unique_ptr<BaresipClient> m_client;
    std::unique_ptr<AccountValidator> m_validator;
    std::unique_ptr<AccountsDialog> m_dialog;
    QString m_userPath;
};

QTEST_MAIN(TestAccountsDialog)
#include "tst_accounts_dialog.moc"
