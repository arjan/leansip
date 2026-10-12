#include "fake_baresip_server.h"
#include "ui/main_window.h"

#include <QSignalSpy>
#include <QtTest>

using namespace leansip;
using CallState = CallController::CallState;

class TestMainWindow : public QObject {
    Q_OBJECT
private slots:
    void init()
    {
        m_server = std::make_unique<FakeBaresipServer>();
        QVERIFY(m_server->listen());
        m_client = std::make_unique<BaresipClient>();
        m_controller = std::make_unique<CallController>(m_client.get());
        AccountInfo account;
        account.displayName = "Test Phone";
        account.aor = "sip:phone@example";
        m_window = std::make_unique<MainWindow>(account, m_controller.get());
        m_window->show();
        QVERIFY(QTest::qWaitForWindowActive(m_window.get()));
    }

    void cleanup()
    {
        m_window.reset();
        m_controller.reset();
        m_client.reset();
        m_server.reset();
    }

    void isFramelessAndShowsAccount()
    {
        QVERIFY(m_window->windowFlags().testFlag(Qt::FramelessWindowHint));
        QCOMPARE(m_window->findChild<QLabel *>("accountLabel")->text(), "Test Phone");
        QVERIFY(m_window->registrationLabel()->text().contains("connecting"));
        QVERIFY(m_window->hintLabel()->text().contains("waiting for baresip"));
        QVERIFY(!m_window->isHelpVisible());
        QVERIFY(m_window->numberEdit()->hasFocus());
    }

    void typingGoesToNumberField()
    {
        QTest::keyClicks(m_window->numberEdit(), "12#+");
        QCOMPARE(m_window->numberEdit()->text(), "12#+");
        QTest::keyClick(m_window->numberEdit(), Qt::Key_Backspace);
        QCOMPARE(m_window->numberEdit()->text(), "12#");
    }

    void enterDialsAndHintsFollowState()
    {
        connectAndRegister();
        QVERIFY(m_window->hintLabel()->text().contains("call"));
        QTest::keyClicks(m_window->numberEdit(), "555");
        QTest::keyClick(m_window->numberEdit(), Qt::Key_Return);

        QTRY_COMPARE(m_server->received.size(), 2);
        QCOMPARE(m_server->lastCommand(), "dial");
        QCOMPARE(m_server->lastParams(), "555");
        QVERIFY(m_window->numberEdit()->isReadOnly());
        QVERIFY(m_window->statusLabel()->text().contains("555"));
        QVERIFY(m_window->hintLabel()->text().contains("hang up"));

        m_server->sendEvent("CALL_ESTABLISHED");
        QTRY_VERIFY(m_window->statusLabel()->text().contains("Connected"));
        QVERIFY(m_window->hintLabel()->text().contains("dtmf"));

        // Digits send DTMF instead of editing while active; other keys are swallowed.
        QTest::keyClick(m_window->numberEdit(), Qt::Key_9);
        QTRY_COMPARE(m_server->received.size(), 3);
        QCOMPARE(m_server->lastCommand(), "sndcode");
        QCOMPARE(m_server->lastParams(), "9");
        QTest::keyClick(m_window->numberEdit(), Qt::Key_A);
        QTest::qWait(30);
        QCOMPARE(m_server->received.size(), 3);
        QCOMPARE(m_window->numberEdit()->text(), "555");

        QTest::keyClick(m_window->numberEdit(), Qt::Key_Escape);
        QTRY_COMPARE(m_server->received.size(), 4);
        QCOMPARE(m_server->lastCommand(), "hangup");

        m_server->sendEvent("CALL_CLOSED", {{"param", "Normal"}});
        QTRY_VERIFY(!m_window->numberEdit()->isReadOnly());
        QCOMPARE(m_window->statusLabel()->text(), "Ended: Normal");
        QCOMPARE(m_window->lastDialed(), "555");
    }

    void upRecallsLastNumber()
    {
        connectAndRegister();
        m_window->dialNumber("4242");
        QTRY_COMPARE(m_server->lastCommand(), "dial");
        m_server->sendEvent("CALL_CLOSED");
        QTRY_VERIFY(!m_window->numberEdit()->isReadOnly());

        QTest::keyClick(m_window->numberEdit(), Qt::Key_Escape); // clear
        QCOMPARE(m_window->numberEdit()->text(), "");
        QTest::keyClick(m_window->numberEdit(), Qt::Key_Up);
        QCOMPARE(m_window->numberEdit()->text(), "4242");
    }

    void escapeClearsThenCloses()
    {
        QTest::keyClicks(m_window->numberEdit(), "1");
        QTest::keyClick(m_window->numberEdit(), Qt::Key_Escape);
        QCOMPARE(m_window->numberEdit()->text(), "");
        QVERIFY(m_window->isVisible());
        QTest::keyClick(m_window->numberEdit(), Qt::Key_Escape);
        QTRY_VERIFY(!m_window->isVisible());
    }

    void escapeCancelsQueuedDial()
    {
        m_window->dialNumber("123");
        QVERIFY(m_controller->hasPendingDial());
        QVERIFY(m_window->statusLabel()->text().contains("Waiting"));
        QTest::keyClick(m_window->numberEdit(), Qt::Key_Escape);
        QVERIFY(!m_controller->hasPendingDial());
        QVERIFY(m_window->isVisible());
        QCOMPARE(m_window->numberEdit()->text(), "123"); // number kept for a retry
    }

    void ctrlQCloses()
    {
        QTest::keyClick(m_window->numberEdit(), Qt::Key_Q, Qt::ControlModifier);
        QTRY_VERIFY(!m_window->isVisible());
    }

    void incomingCallEnterAnswersEscapeDeclines()
    {
        connectAndRegister();
        m_server->sendEvent("CALL_INCOMING", {{"peeruri", "sip:bob@h"}, {"peerdisplayname", "Bob"}});
        QTRY_VERIFY(m_window->statusLabel()->text().contains("Bob"));
        QVERIFY(m_window->hintLabel()->text().contains("answer"));

        QTest::keyClick(m_window->numberEdit(), Qt::Key_Return);
        QTRY_COMPARE(m_server->lastCommand(), "accept");

        m_server->sendEvent("CALL_CLOSED");
        QTRY_COMPARE(m_controller->callState(), CallState::Idle);
        m_server->sendEvent("CALL_INCOMING", {{"peeruri", "sip:bob@h"}});
        QTRY_COMPARE(m_controller->callState(), CallState::Incoming);
        QTest::keyClick(m_window->numberEdit(), Qt::Key_Escape);
        QTRY_COMPARE(m_server->lastCommand(), "hangup");
    }

    void helpTogglesWithQuestionMarkAndF1()
    {
        QTest::keyClick(m_window->numberEdit(), Qt::Key_Question, Qt::ShiftModifier, 0);
        QVERIFY(m_window->isHelpVisible());
        QVERIFY(m_window->findChild<QLabel *>("helpLabel")->text().contains("Ctrl+Q"));
        QCOMPARE(m_window->numberEdit()->text(), ""); // '?' never reached the field

        // Ordinary keys are swallowed while help is up; Esc closes it instead of clearing/quitting.
        QTest::keyClick(m_window.get(), Qt::Key_5);
        QCOMPARE(m_window->numberEdit()->text(), "");
        QTest::keyClick(m_window.get(), Qt::Key_Escape);
        QVERIFY(!m_window->isHelpVisible());
        QVERIFY(m_window->isVisible());
        QVERIFY(m_window->numberEdit()->hasFocus());

        QTest::keyClick(m_window->numberEdit(), Qt::Key_F1);
        QVERIFY(m_window->isHelpVisible());
        QTest::keyClick(m_window.get(), Qt::Key_F1);
        QVERIFY(!m_window->isHelpVisible());
    }

    void commandFailureIsShown()
    {
        connectAndRegister();
        m_window->dialNumber("1");
        QTRY_COMPARE(m_server->lastCommand(), "dial");
        m_server->sendResponse(m_server->lastToken(), false, "no route");
        QTRY_VERIFY(m_window->statusLabel()->text().contains("no route"));
        QVERIFY(!m_window->numberEdit()->isReadOnly());
    }

    void backendLossIsShown()
    {
        connectAndRegister();
        m_server->dropClient();
        QTRY_VERIFY(m_window->statusLabel()->text().contains("lost"));
        QVERIFY(m_window->hintLabel()->text().contains("waiting for baresip"));
    }

private:
    void connectAndRegister()
    {
        QSignalSpy ready(m_controller.get(), &CallController::backendReady);
        m_client->connectToHost("127.0.0.1", m_server->port());
        QVERIFY(ready.wait(2000));
        QTRY_VERIFY(m_server->received.size() >= 1); // reginfo seed query
        m_server->sendEvent("REGISTER_OK", {{"class", "register"}, {"param", "200 OK"}});
        QTRY_COMPARE(m_controller->registrationState(), CallController::RegistrationState::Registered);
    }

    std::unique_ptr<FakeBaresipServer> m_server;
    std::unique_ptr<BaresipClient> m_client;
    std::unique_ptr<CallController> m_controller;
    std::unique_ptr<MainWindow> m_window;
};

QTEST_MAIN(TestMainWindow)
#include "tst_main_window.moc"
