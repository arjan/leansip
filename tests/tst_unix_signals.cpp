#include "core/unix_signals.h"

#include <QSignalSpy>
#include <QtTest>

#include <csignal>

using namespace leansip;

class TestUnixSignals : public QObject {
    Q_OBJECT
private slots:
    void deliversSignalThroughEventLoop()
    {
        UnixSignalBridge bridge;
        QVERIFY(bridge.install({SIGUSR1}));
        QSignalSpy received(&bridge, &UnixSignalBridge::signalReceived);
        ::raise(SIGUSR1);
        QVERIFY(received.wait(2000));
        QCOMPARE(received.first().first().toInt(), SIGUSR1);
    }

    void respectsIgnoredSignals()
    {
        ::signal(SIGUSR2, SIG_IGN);
        UnixSignalBridge bridge;
        QVERIFY(bridge.install({SIGUSR2}));
        QSignalSpy received(&bridge, &UnixSignalBridge::signalReceived);
        ::raise(SIGUSR2); // still ignored: must neither kill us nor emit
        QVERIFY(!received.wait(200));
        ::signal(SIGUSR2, SIG_DFL);
    }
};

QTEST_GUILESS_MAIN(TestUnixSignals)
#include "tst_unix_signals.moc"
