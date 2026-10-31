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
        qunsetenv("XDG_ACTIVATION_TOKEN");
    }

    void wireFormatRoundTrip()
    {
        QString number;
        QString token;
        SingleInstance::decodeMessage(SingleInstance::encodeMessage("0612345678", "tok-123"), number, token);
        QCOMPARE(number, "0612345678");
        QCOMPARE(token, "tok-123");

        SingleInstance::decodeMessage(SingleInstance::encodeMessage("", ""), number, token);
        QVERIFY(number.isEmpty());
        QVERIFY(token.isEmpty());

        // Old-style frame without a tab still yields the number.
        SingleInstance::decodeMessage("42\n", number, token);
        QCOMPARE(number, "42");
        QVERIFY(token.isEmpty());

        // Separators inside the fields are stripped rather than corrupting the frame.
        QCOMPARE(SingleInstance::encodeMessage("1\t2\n3", "a\tb"), QByteArray("123\tab\n"));
    }

    void profileSuffixSeparatesBaresipDirs()
    {
        const QString home = QDir::homePath();
        QVERIFY(SingleInstance::profileSuffix(home + "/.baresip").isEmpty());
        QVERIFY(SingleInstance::profileSuffix(home + "/./.baresip/").isEmpty());

        const QString second = SingleInstance::profileSuffix(home + "/.baresip2");
        QCOMPARE(second.size(), 9);
        QVERIFY(second.startsWith(u'-'));
        QCOMPARE(SingleInstance::profileSuffix(home + "/.baresip2/"), second); // stable
        QVERIFY(SingleInstance::profileSuffix(home + "/.baresip3") != second);
    }

    void forwardFailsWhenNobodyListens()
    {
        SingleInstance second(m_name);
        QVERIFY(!second.tryForward("123", 200));
    }

    void primaryReceivesForwardedNumberAndToken()
    {
        SingleInstance primary(m_name);
        QVERIFY(primary.listen());
        QSignalSpy received(&primary, &SingleInstance::numberReceived);

        // tryForward blocks on the ack, so the primary must process events on another thread.
        qputenv("XDG_ACTIVATION_TOKEN", "launcher-token");
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
        QCOMPARE(received.first().at(0).toString(), "0612345678");
        QCOMPARE(received.first().at(1).toString(), "launcher-token");
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
        QVERIFY(received.first().at(0).toString().isEmpty());
        QVERIFY(received.first().at(1).toString().isEmpty()); // no token in this environment
        QVERIFY(worker.wait(3000));
    }

    void staleSocketIsReclaimed()
    {
        // Simulate a crashed primary that left its socket file behind.
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
