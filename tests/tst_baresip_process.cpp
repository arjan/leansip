#include "core/baresip_process.h"
#include "core/settings.h"

#include <QStandardPaths>
#include <QTcpSocket>
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

    void startsFromSetupAndRestartsQuietly()
    {
        QTemporaryDir root;
        config_deriver::Setup setup;
        setup.config = settings::baresipConfig({{QStringLiteral("sip_listen"), {QStringLiteral("127.0.0.1:0")}},
                                                {QStringLiteral("audio_player"), {QStringLiteral("aufile,/dev/null")}},
                                                {QStringLiteral("audio_source"), {QStringLiteral("aufile,/dev/null")}}});
        setup.accountLine = QStringLiteral("<sip:a@127.0.0.1:1;regint=0>");

        BaresipProcess process;
        process.setDerivedDir(root.filePath("derived")); // never touch the real runtime dir
        process.setCaptureOutput(true);
        QVERIFY2(process.start(setup), qPrintable(process.lastError()));
        QVERIFY(process.isRunning());
        QVERIFY(process.port() != 0);
        QFile accounts(root.filePath("derived/accounts"));
        QVERIFY(accounts.open(QIODevice::ReadOnly));
        QCOMPARE(accounts.readAll(), "<sip:a@127.0.0.1:1;regint=0>\n");

        // baresip answers on the control port it was given.
        QTcpSocket ctrl;
        QTRY_VERIFY_WITH_TIMEOUT((ctrl.connectToHost(process.host(), process.port()), ctrl.waitForConnected(200)), 5000);
        ctrl.abort();

        QSignalSpy finished(&process, &BaresipProcess::finished);
        QVERIFY2(process.restart(), qPrintable(process.lastError()));
        QVERIFY(process.isRunning());
        QCOMPARE(finished.size(), 0); // a deliberate restart is not reported as an exit

        process.stop();
        QVERIFY(!process.isRunning());
    }

    void startFailsWithoutBinary()
    {
        QTemporaryDir root;
        BaresipProcess process;
        process.setDerivedDir(root.filePath("derived"));
        QVERIFY(!process.start({}, QStringLiteral("no-such-baresip")));
        QVERIFY(process.lastError().contains("not found"));
    }

private:
    QString m_baresip;
};

QTEST_GUILESS_MAIN(TestBaresipProcess)
#include "tst_baresip_process.moc"
