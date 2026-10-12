#include "core/baresip_process.h"

#include <QStandardPaths>
#include <QtTest>

using namespace leansip;

// Exercises the parts of BaresipProcess that need the real baresip binary. Skipped when it is
// not installed so the suite stays green on build machines without it.
class TestBaresipProcess : public QObject {
    Q_OBJECT
private slots:
    void initTestCase()
    {
        m_baresip = QStandardPaths::findExecutable(QStringLiteral("baresip"));
        if (m_baresip.isEmpty())
            QSKIP("baresip not installed");
    }

    void ensureUserConfigCreatesTemplate()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString dir = root.filePath("dotbaresip");
        QString error;
        QVERIFY2(BaresipProcess::ensureUserConfig(dir, &error), qPrintable(error));
        QVERIFY(QFile::exists(dir + "/config"));
        QVERIFY(QFile::exists(dir + "/accounts")); // baresip writes an (empty) accounts template too
        const auto perms = QFileInfo(dir).permissions();
        QVERIFY(!(perms & QFileDevice::ReadOther));

        // Idempotent: a second call does nothing and keeps the existing config.
        QFile cfg(dir + "/config");
        QVERIFY(cfg.open(QIODevice::Append));
        cfg.write("# marker\n");
        cfg.close();
        QVERIFY(BaresipProcess::ensureUserConfig(dir, &error));
        QVERIFY(cfg.open(QIODevice::ReadOnly));
        QVERIFY(cfg.readAll().contains("# marker"));
    }

    void ensureUserConfigFailsWithoutBinary()
    {
        QTemporaryDir root;
        QString error;
        QVERIFY(!BaresipProcess::ensureUserConfig(root.filePath("x"), &error, QStringLiteral("no-such-baresip")));
        QVERIFY(error.contains("not found"));
    }

    void restartKeepsCandidateAccountsWhenOverridden()
    {
        // A private user dir with a template config and one non-registering account.
        QTemporaryDir root;
        const QString user = root.filePath("user");
        QString error;
        QVERIFY2(BaresipProcess::ensureUserConfig(user, &error), qPrintable(error));
        QFile acc(user + "/accounts");
        QVERIFY(acc.open(QIODevice::WriteOnly | QIODevice::Truncate));
        acc.write("<sip:a@127.0.0.1:1;regint=0>\n");
        acc.close();

        BaresipProcess process;
        process.setDerivedDir(root.filePath("derived")); // never touch the real runtime dir
        QVERIFY2(process.start(user), qPrintable(process.lastError()));
        QVERIFY(process.isRunning());
        const quint16 firstPort = process.port();
        QVERIFY(firstPort != 0);
        QVERIFY(QFileInfo(process.derivedAccountsPath()).isSymLink());

        // Put a candidate in place and restart with the override: the file must survive.
        process.setAccountsOverride(true);
        QFile::remove(process.derivedAccountsPath());
        QFile cand(process.derivedAccountsPath());
        QVERIFY(cand.open(QIODevice::WriteOnly));
        cand.write("<sip:candidate@127.0.0.1:1;regint=0>\n");
        cand.close();
        QSignalSpy finished(&process, &BaresipProcess::finished);
        QVERIFY2(process.restart(), qPrintable(process.lastError()));
        QVERIFY(process.isRunning());
        QVERIFY(!QFileInfo(process.derivedAccountsPath()).isSymLink());
        QCOMPARE(finished.size(), 0); // a deliberate restart is not reported as an exit

        // Lifting the override re-links the user's file on the next restart.
        process.setAccountsOverride(false);
        QVERIFY2(process.restart(), qPrintable(process.lastError()));
        QVERIFY(QFileInfo(process.derivedAccountsPath()).isSymLink());

        process.stop();
        QVERIFY(!process.isRunning());
    }

private:
    QString m_baresip;
};

QTEST_GUILESS_MAIN(TestBaresipProcess)
#include "tst_baresip_process.moc"
