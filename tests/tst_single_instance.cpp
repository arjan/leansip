#include "core/single_instance.h"

#include <QLocalServer>
#include <QSignalSpy>
#include <QtTest>

using namespace leansip;

class TestSingleInstance : public QObject {
    Q_OBJECT
private slots:
    void init()
    {
        m_name = QStringLiteral("leansip-test-%1-%2").arg(QCoreApplication::applicationPid()).arg(++m_counter);
        QLocalServer::removeServer(m_name);
    }

    void forwardFailsWhenNobodyListens()
    {
        SingleInstance second(m_name);
        QVERIFY(!second.tryForward("123", 200));
    }

    void primaryReceivesForwardedNumber()
    {
        SingleInstance primary(m_name);
        QVERIFY(primary.listen());
        QSignalSpy received(&primary, &SingleInstance::numberReceived);

        // tryForward blocks on the ack, so the primary must process events on another thread
        // or we drive it from a timer: simplest is a QThread for the secondary.
        bool acked = false;
        QThread worker;
        QObject starter;
        starter.moveToThread(&worker);
        connect(&worker, &QThread::started, &starter, [&] {
            SingleInstance second(m_name);
            acked = second.tryForward("0612345678", 2000);
            worker.quit();
        });
        worker.start();

        QTRY_COMPARE(received.size(), 1);
        QCOMPARE(received.first().first().toString(), "0612345678");
        QVERIFY(worker.wait(3000));
        QVERIFY(acked);
    }

    void emptyNumberMeansRaise()
    {
        SingleInstance primary(m_name);
        QVERIFY(primary.listen());
        QSignalSpy received(&primary, &SingleInstance::numberReceived);

        QThread worker;
        QObject starter;
        starter.moveToThread(&worker);
        connect(&worker, &QThread::started, &starter, [&] {
            SingleInstance second(m_name);
            second.tryForward(QString(), 2000);
            worker.quit();
        });
        worker.start();

        QTRY_COMPARE(received.size(), 1);
        QVERIFY(received.first().first().toString().isEmpty());
        QVERIFY(worker.wait(3000));
    }

    void staleSocketIsReclaimed()
    {
        // Simulate a crashed primary: a server that listened and was abandoned without close().
        {
            QLocalServer stale;
            QVERIFY(stale.listen(m_name));
            // Destructor closes the server but QLocalServer removes the file; emulate a left-over by
            // recreating the path afterwards.
        }
        QFile::remove(QDir::temp().filePath(m_name));
        QFile leftover(QDir::temp().filePath(m_name));
        QVERIFY(leftover.open(QIODevice::WriteOnly));
        leftover.close();

        SingleInstance primary(m_name);
        QVERIFY(!primary.tryForward("1", 200));
        QVERIFY(primary.listen());
    }

private:
    QString m_name;
    int m_counter = 0;
};

QTEST_GUILESS_MAIN(TestSingleInstance)
#include "tst_single_instance.moc"
