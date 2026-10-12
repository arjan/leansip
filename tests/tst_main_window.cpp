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
    }

    void cleanup()
    {
        m_window.reset();
        m_controller.reset();
        m_client.reset();
        m_server.reset();
    }

    void showsAccountAndStartsDisconnected()
    {
        QCOMPARE(m_window->findChild<QLabel *>("accountLabel")->text(), "Test Phone");
        QVERIFY(m_window->registrationLabel()->text().contains("connecting"));
        QVERIFY(m_window->callButton()->isVisible());
        QVERIFY(!m_window->callButton()->isEnabled());
        QVERIFY(!m_window->hangupButton()->isVisible());
        QVERIFY(!m_window->answerButton()->isVisible());
    }

    void keypadAppendsToNumber()
    {
        QTest::mouseClick(m_window->keypadButton('1'), Qt::LeftButton);
        QTest::mouseClick(m_window->keypadButton('2'), Qt::LeftButton);
        QTest::mouseClick(m_window->keypadButton('#'), Qt::LeftButton);
        QCOMPARE(m_window->numberEdit()->text(), "12#");
    }

    void callEnabledOnlyWhenRegisteredAndNumberPresent()
    {
        m_window->numberEdit()->setText("123");
        QVERIFY(!m_window->callButton()->isEnabled()); // backend not up

        connectAndRegister();
        QVERIFY(m_window->registrationLabel()->text().contains("registered"));
        QVERIFY(m_window->callButton()->isEnabled());

        m_window->numberEdit()->clear();
        QVERIFY(!m_window->callButton()->isEnabled());
    }

    void callButtonDialsAndButtonsFollowState()
    {
        connectAndRegister();
        m_window->numberEdit()->setText("555");
        QTest::mouseClick(m_window->callButton(), Qt::LeftButton);

        QTRY_COMPARE(m_server->received.size(), 2);
        QCOMPARE(m_server->lastCommand(), "dial");
        QCOMPARE(m_server->lastParams(), "555");
        QVERIFY(!m_window->callButton()->isVisible());
        QVERIFY(m_window->hangupButton()->isVisible());
        QVERIFY(m_window->numberEdit()->isReadOnly());
        QVERIFY(m_window->statusLabel()->text().contains("555"));

        m_server->sendEvent("CALL_ESTABLISHED");
        QTRY_VERIFY(m_window->statusLabel()->text().contains("Connected"));

        // Keypad sends DTMF instead of editing while active.
        QTest::mouseClick(m_window->keypadButton('9'), Qt::LeftButton);
        QTRY_COMPARE(m_server->received.size(), 3);
        QCOMPARE(m_server->lastCommand(), "sndcode");
        QCOMPARE(m_window->numberEdit()->text(), "555");

        QTest::mouseClick(m_window->hangupButton(), Qt::LeftButton);
        QTRY_COMPARE(m_server->received.size(), 4);
        QCOMPARE(m_server->lastCommand(), "hangup");

        m_server->sendEvent("CALL_CLOSED", {{"param", "Normal"}});
        QTRY_VERIFY(m_window->callButton()->isVisible());
        QVERIFY(!m_window->hangupButton()->isVisible());
        QVERIFY(!m_window->numberEdit()->isReadOnly());
        QCOMPARE(m_window->statusLabel()->text(), "Ended: Normal");
    }

    void enterKeyDials()
    {
        connectAndRegister();
        m_window->numberEdit()->setText("42");
        QTest::keyClick(m_window->numberEdit(), Qt::Key_Return);
        QTRY_COMPARE(m_server->lastCommand(), "dial");
        QCOMPARE(m_server->lastParams(), "42");
    }

    void incomingCallShowsAnswerDecline()
    {
        connectAndRegister();
        m_server->sendEvent("CALL_INCOMING", {{"peeruri", "sip:bob@h"}, {"peerdisplayname", "Bob"}});
        QTRY_VERIFY(m_window->answerButton()->isVisible());
        QVERIFY(m_window->declineButton()->isVisible());
        QVERIFY(!m_window->callButton()->isVisible());
        QVERIFY(m_window->statusLabel()->text().contains("Bob"));

        QTest::mouseClick(m_window->answerButton(), Qt::LeftButton);
        QTRY_COMPARE(m_server->lastCommand(), "accept");
    }

    void dialNumberFillsFieldAndDials()
    {
        connectAndRegister();
        m_window->dialNumber("31201234567");
        QCOMPARE(m_window->numberEdit()->text(), "31201234567");
        QTRY_COMPARE(m_server->lastCommand(), "dial");
        QCOMPARE(m_server->lastParams(), "31201234567");
    }

    void dialNumberBeforeRegistrationShowsWaiting()
    {
        m_window->dialNumber("123");
        QVERIFY(m_controller->hasPendingDial());
        QVERIFY(m_window->statusLabel()->text().contains("Waiting"));
        connectAndRegister();
        QTRY_COMPARE(m_server->lastCommand(), "dial");
    }

    void commandFailureIsShown()
    {
        connectAndRegister();
        m_window->dialNumber("1");
        QTRY_COMPARE(m_server->lastCommand(), "dial");
        m_server->sendResponse(m_server->lastToken(), false, "no route");
        QTRY_VERIFY(m_window->statusLabel()->text().contains("no route"));
        QVERIFY(m_window->callButton()->isVisible());
    }

    void backendLossDisablesCalling()
    {
        connectAndRegister();
        m_window->numberEdit()->setText("1");
        QVERIFY(m_window->callButton()->isEnabled());
        m_server->dropClient();
        QTRY_VERIFY(!m_window->callButton()->isEnabled());
        QVERIFY(m_window->statusLabel()->text().contains("lost"));
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
