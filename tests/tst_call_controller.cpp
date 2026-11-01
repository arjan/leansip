#include "core/call_controller.h"
#include "fake_baresip_server.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QtTest>

using namespace leansip;
using CallState = CallController::CallState;
using RegState = CallController::RegistrationState;

// Shared fixture: fake server + connected client + controller.
struct Rig {
    FakeBaresipServer server;
    BaresipClient client;
    CallController controller{&client};

    bool connectAndRegister(bool registered = true)
    {
        if (!server.listen())
            return false;
        QSignalSpy ready(&controller, &CallController::backendReady);
        client.connectToHost("127.0.0.1", server.port());
        if (!ready.wait(2000))
            return false;
        // Controller seeds state with reginfo on connect.
        if (!QTest::qWaitFor([&] { return server.received.size() >= 1; }, 2000))
            return false;
        if (server.lastCommand() != "reginfo")
            return false;
        if (registered) {
            QSignalSpy reg(&controller, &CallController::registrationStateChanged);
            server.sendEvent("REGISTER_OK", {{"class", "register"}, {"param", "200 OK"}});
            if (!reg.wait(2000))
                return false;
        }
        return true;
    }

    // Wait until the server has seen `count` commands.
    bool waitCommands(int count) { return QTest::qWaitFor([&] { return server.received.size() >= count; }, 2000); }
};

class TestCallController : public QObject {
    Q_OBJECT
private slots:
    void numberValidation()
    {
        QVERIFY(CallController::isValidNumber("0612345678"));
        QVERIFY(CallController::isValidNumber("+31612345678"));
        QVERIFY(CallController::isValidNumber("*21#"));
        QVERIFY(CallController::isValidNumber("sip:alice@example.com"));
        QVERIFY(CallController::isValidNumber("alice@example.com:5060"));
        QVERIFY(!CallController::isValidNumber(""));
        QVERIFY(!CallController::isValidNumber("06 1234"));
        QVERIFY(!CallController::isValidNumber("123;hangup"));
        QVERIFY(!CallController::isValidNumber("a\nb"));
        QVERIFY(!CallController::isValidNumber("sip:"));
    }

    void parseRegInfo()
    {
        const QString aor = "sip:phone1@example.com";
        const QString ok = "\n--- User Agents (1) ---\n> sip:phone1@example.com                OK sip:1.2.3.4 Expires 3600s\n";
        QCOMPARE(CallController::parseRegInfo(ok, aor), RegState::Registered);
        const QString err = "> sip:phone1@example.com                ERR sip:1.2.3.4 Expires 0s\n";
        QCOMPARE(CallController::parseRegInfo(err, aor), RegState::Failed);
        // Other account's line must not be picked up; unknown formats stay Unknown.
        QCOMPARE(CallController::parseRegInfo("  sip:other@example.com OK\n", aor), RegState::Unknown);
        QCOMPARE(CallController::parseRegInfo("> sip:phone1@example.com   registering\n", aor), RegState::Unknown);
        QCOMPARE(CallController::parseRegInfo(ok, QString()), RegState::Unknown);
        // Verbatim baresip 1.1.0 output: coloured status with ANSI escapes.
        const QString real = QString::fromUtf8(
            "\n--- User Agents (1) ---\n0 - sip:phone1@example.com                  \x1b[32mOK \x1b[;m kamailio (5.8.4 (x86_64/linux)) Expires 3706s\n\n");
        QCOMPARE(CallController::parseRegInfo(real, aor), RegState::Registered);
        const QString realErr = QString::fromUtf8("0 - sip:phone1@example.com   \x1b[31mERR \x1b[;m 401 Unauthorized\n");
        QCOMPARE(CallController::parseRegInfo(realErr, aor), RegState::Failed);
        // "OK" must be a token, not a substring of e.g. a hostname.
        QCOMPARE(CallController::parseRegInfo("> sip:phone1@example.com  ERR sip:OKAY.host\n", aor), RegState::Failed);
    }

    void seedsRegistrationFromRegInfo()
    {
        Rig rig;
        rig.controller.setAccountAor("sip:a@b");
        rig.controller.dial("555"); // queued
        QVERIFY(rig.connectAndRegister(false));
        QSignalSpy reg(&rig.controller, &CallController::registrationStateChanged);
        rig.server.sendResponse(rig.server.lastToken(), true, "> sip:a@b   OK sip:srv Expires 60s\n");
        QTRY_COMPARE(reg.size(), 1);
        QCOMPARE(rig.controller.registrationState(), RegState::Registered);
        // The queued dial goes out once the seed says we are registered.
        QVERIFY(rig.waitCommands(2));
        QCOMPARE(rig.server.lastCommand(), "dial");
        QCOMPARE(rig.server.lastParams(), "555");
    }

    void regInfoFailureIsSilentAndEventsStillWin()
    {
        Rig rig;
        rig.controller.setAccountAor("sip:a@b");
        QVERIFY(rig.connectAndRegister(false));
        QSignalSpy failed(&rig.controller, &CallController::commandFailed);
        QSignalSpy reg(&rig.controller, &CallController::registrationStateChanged);
        rig.server.sendResponse(rig.server.lastToken(), false, "command not found (reginfo)");
        QTest::qWait(50);
        QCOMPARE(failed.size(), 0);
        QCOMPARE(reg.size(), 0);
        rig.server.sendEvent("REGISTER_OK", {{"class", "register"}});
        QTRY_COMPARE(rig.controller.registrationState(), RegState::Registered);
    }

    void regInfoDoesNotOverrideEarlierEvent()
    {
        Rig rig;
        rig.controller.setAccountAor("sip:a@b");
        QVERIFY(rig.connectAndRegister(false));
        const QString seedToken = rig.server.lastToken();
        rig.server.sendEvent("REGISTER_FAIL", {{"class", "register"}, {"param", "403"}});
        QTRY_COMPARE(rig.controller.registrationState(), RegState::Failed);
        rig.server.sendResponse(seedToken, true, "> sip:a@b   OK sip:srv Expires 60s\n");
        QTest::qWait(50);
        QCOMPARE(rig.controller.registrationState(), RegState::Failed);
    }

    void initialStateIsIdleUnknown()
    {
        BaresipClient client;
        CallController c(&client);
        QCOMPARE(c.callState(), CallState::Idle);
        QCOMPARE(c.registrationState(), RegState::Unknown);
        QVERIFY(!c.backendAvailable());
    }

    void registrationEventsUpdateState()
    {
        Rig rig;
        QVERIFY(rig.connectAndRegister(false));
        QSignalSpy reg(&rig.controller, &CallController::registrationStateChanged);

        rig.server.sendEvent("REGISTER_FAIL", {{"class", "register"}, {"param", "401 Unauthorized"}});
        QTRY_COMPARE(reg.size(), 1);
        QCOMPARE(rig.controller.registrationState(), RegState::Failed);
        QCOMPARE(rig.controller.registrationDetail(), "401 Unauthorized");

        rig.server.sendEvent("REGISTER_OK", {{"class", "register"}, {"param", "200 OK"}});
        QTRY_COMPARE(reg.size(), 2);
        QCOMPARE(rig.controller.registrationState(), RegState::Registered);

        rig.server.sendEvent("UNREGISTERING", {{"class", "register"}});
        QTRY_COMPARE(reg.size(), 3);
        QCOMPARE(rig.controller.registrationState(), RegState::Unregistered);
    }

    void outgoingCallLifecycle()
    {
        Rig rig;
        QVERIFY(rig.connectAndRegister());
        QSignalSpy states(&rig.controller, &CallController::callStateChanged);
        QSignalSpy ended(&rig.controller, &CallController::callEnded);

        rig.controller.dial("12345");
        QCOMPARE(rig.controller.callState(), CallState::Outgoing);
        QCOMPARE(rig.controller.peer(), "12345");
        QVERIFY(rig.waitCommands(2));
        QCOMPARE(rig.server.lastCommand(), "dial");
        QCOMPARE(rig.server.lastParams(), "12345");

        rig.server.sendResponse(rig.server.lastToken(), true);
        rig.server.sendEvent("CALL_RINGING");
        rig.server.sendEvent("CALL_ESTABLISHED");
        QTRY_COMPARE(rig.controller.callState(), CallState::Active);

        // A digit sends the RFC 2833 code and asks baresip to play our local feedback tone.
        rig.controller.sendDtmf('5');
        QVERIFY(rig.waitCommands(4));
        QCOMPARE(rig.server.received.at(2).value("command").toString(), "sndcode");
        QCOMPARE(rig.server.received.at(2).value("params").toString(), "5");
        QCOMPARE(rig.server.lastCommand(), "play");
        QCOMPARE(rig.server.lastParams(), "dtmf-5.wav");
        // A missing tone file must never surface as a user-visible failure.
        QSignalSpy failed(&rig.controller, &CallController::commandFailed);
        rig.server.sendResponse(rig.server.lastToken(), false, "No such file or directory");
        QTest::qWait(50);
        QCOMPARE(failed.size(), 0);

        rig.controller.hangup();
        QVERIFY(rig.waitCommands(5));
        QCOMPARE(rig.server.lastCommand(), "hangup");

        rig.server.sendEvent("CALL_CLOSED", {{"param", "Connection reset by user"}});
        QTRY_COMPARE(rig.controller.callState(), CallState::Idle);
        QCOMPARE(ended.size(), 1);
        QCOMPARE(ended.first().first().toString(), "Connection reset by user");
        QCOMPARE(states.size(), 3); // Outgoing, Active, Idle
    }

    void incomingCallAnswered()
    {
        Rig rig;
        QVERIFY(rig.connectAndRegister());
        QSignalSpy peer(&rig.controller, &CallController::peerChanged);

        rig.server.sendEvent("CALL_INCOMING", {{"peeruri", "sip:bob@h"}, {"peerdisplayname", "Bob"}, {"direction", "incoming"}});
        QTRY_COMPARE(rig.controller.callState(), CallState::Incoming);
        QCOMPARE(rig.controller.peer(), "Bob");

        rig.controller.answer();
        QVERIFY(rig.waitCommands(2));
        QCOMPARE(rig.server.lastCommand(), "accept");
        rig.server.sendEvent("CALL_ESTABLISHED");
        QTRY_COMPARE(rig.controller.callState(), CallState::Active);
    }

    void incomingCallWithoutDisplayNameUsesUri()
    {
        Rig rig;
        QVERIFY(rig.connectAndRegister());
        rig.server.sendEvent("CALL_INCOMING", {{"peeruri", "sip:bob@h"}});
        QTRY_COMPARE(rig.controller.peer(), "sip:bob@h");
    }

    void incomingCallDeclinedViaHangup()
    {
        Rig rig;
        QVERIFY(rig.connectAndRegister());
        rig.server.sendEvent("CALL_INCOMING", {{"peeruri", "sip:bob@h"}});
        QTRY_COMPARE(rig.controller.callState(), CallState::Incoming);
        rig.controller.hangup();
        QVERIFY(rig.waitCommands(2));
        QCOMPARE(rig.server.lastCommand(), "hangup");
        rig.server.sendEvent("CALL_CLOSED", {{"param", "Rejected"}});
        QTRY_COMPARE(rig.controller.callState(), CallState::Idle);
    }

    void secondIncomingCallIsIgnored()
    {
        Rig rig;
        QVERIFY(rig.connectAndRegister());
        rig.server.sendEvent("CALL_INCOMING", {{"peeruri", "sip:first@h"}});
        QTRY_COMPARE(rig.controller.callState(), CallState::Incoming);
        rig.server.sendEvent("CALL_INCOMING", {{"peeruri", "sip:second@h"}});
        QTest::qWait(50);
        QCOMPARE(rig.controller.peer(), "sip:first@h");
    }

    void dialQueuedUntilRegistered()
    {
        Rig rig;
        QVERIFY(rig.connectAndRegister(false));
        rig.controller.dial("999");
        QVERIFY(rig.controller.hasPendingDial());
        QCOMPARE(rig.controller.callState(), CallState::Idle);
        QTest::qWait(50);
        QCOMPARE(rig.server.received.size(), 1); // only reglist so far

        rig.server.sendEvent("REGISTER_OK", {{"class", "register"}});
        QVERIFY(rig.waitCommands(2));
        QCOMPARE(rig.server.lastCommand(), "dial");
        QCOMPARE(rig.server.lastParams(), "999");
        QVERIFY(!rig.controller.hasPendingDial());
        QCOMPARE(rig.controller.callState(), CallState::Outgoing);
    }

    void dialQueuedBeforeBackendConnects()
    {
        Rig rig;
        rig.controller.dial("777");
        QVERIFY(rig.controller.hasPendingDial());
        QVERIFY(rig.connectAndRegister());
        QVERIFY(rig.waitCommands(2));
        QCOMPARE(rig.server.lastParams(), "777");
    }

    void hangupClearsPendingDial()
    {
        Rig rig;
        QVERIFY(rig.connectAndRegister(false));
        rig.controller.dial("999");
        rig.controller.hangup();
        QVERIFY(!rig.controller.hasPendingDial());
    }

    void invalidNumberAndBusyAreRejected()
    {
        Rig rig;
        QVERIFY(rig.connectAndRegister());
        QSignalSpy failed(&rig.controller, &CallController::commandFailed);

        rig.controller.dial("bad number");
        QCOMPARE(failed.size(), 1);
        QCOMPARE(rig.controller.callState(), CallState::Idle);
        // A locally rejected number gets the "invalid number" tone (baresip never sees it).
        QVERIFY(rig.waitCommands(2));
        QCOMPARE(rig.server.lastCommand(), "play");
        QCOMPARE(rig.server.lastParams(), "error.wav");

        rig.controller.dial("1");
        rig.controller.dial("2");
        QCOMPARE(failed.size(), 2);
        QCOMPARE(failed.last().at(1).toString(), "a call is already in progress");
        QCOMPARE(rig.controller.peer(), "1");
    }

    void failedDialResponseReturnsToIdle()
    {
        Rig rig;
        QVERIFY(rig.connectAndRegister());
        QSignalSpy ended(&rig.controller, &CallController::callEnded);
        QSignalSpy failed(&rig.controller, &CallController::commandFailed);

        rig.controller.dial("1");
        QVERIFY(rig.waitCommands(2));
        rig.server.sendResponse(rig.server.lastToken(), false, "could not dial\n"); // baresip appends '\n'
        QTRY_COMPARE(rig.controller.callState(), CallState::Idle);
        QCOMPARE(ended.first().first().toString(), "could not dial");
        QCOMPARE(failed.first().at(1).toString(), "could not dial");
        QCOMPARE(failed.size(), 1);
        QVERIFY(rig.waitCommands(3));
        QCOMPARE(rig.server.lastCommand(), "play");
        QCOMPARE(rig.server.lastParams(), "error.wav");
    }

    void failureSoundMapping()
    {
        QCOMPARE(CallController::failureSound("403 Forbidden"), "error.wav");
        QCOMPARE(CallController::failureSound("404 Not Found"), "error.wav");
        QCOMPARE(CallController::failureSound("503 Service Unavailable"), "error.wav");
        QCOMPARE(CallController::failureSound("486 Busy Here"), "busy.wav");
        QCOMPARE(CallController::failureSound("603 Decline"), "busy.wav");
        QCOMPARE(CallController::failureSound("487 Request Terminated"), QString());
        QCOMPARE(CallController::failureSound("Connection reset by user"), QString());
        QCOMPARE(CallController::failureSound(QString()), QString());
    }

    void rejectedOutgoingCallPlaysErrorTone()
    {
        Rig rig;
        QVERIFY(rig.connectAndRegister());
        QSignalSpy ended(&rig.controller, &CallController::callEnded);
        rig.controller.dial("1");
        QVERIFY(rig.waitCommands(2));
        rig.server.sendResponse(rig.server.lastToken(), true);
        rig.server.sendEvent("CALL_CLOSED", {{"param", "403 Forbidden\n"}});
        QTRY_COMPARE(rig.controller.callState(), CallState::Idle);
        QCOMPARE(ended.first().first().toString(), "403 Forbidden");
        QVERIFY(rig.waitCommands(3));
        QCOMPARE(rig.server.lastCommand(), "play");
        QCOMPARE(rig.server.lastParams(), "error.wav");
    }

    void establishedCallClosingIsSilent()
    {
        Rig rig;
        QVERIFY(rig.connectAndRegister());
        rig.controller.dial("1");
        QVERIFY(rig.waitCommands(2));
        rig.server.sendEvent("CALL_ESTABLISHED");
        QTRY_COMPARE(rig.controller.callState(), CallState::Active);
        rig.server.sendEvent("CALL_CLOSED", {{"param", "Connection reset by peer"}});
        QTRY_COMPARE(rig.controller.callState(), CallState::Idle);
        QTest::qWait(50);
        QCOMPARE(rig.server.received.size(), 2); // no play
    }

    void hangupRejectedResetsToIdle()
    {
        // The UI believes a call is outgoing but baresip has none (its CALL_CLOSED was missed):
        // a rejected hangup must resync to Idle instead of leaving the user stuck.
        Rig rig;
        QVERIFY(rig.connectAndRegister());
        QSignalSpy ended(&rig.controller, &CallController::callEnded);
        QSignalSpy failed(&rig.controller, &CallController::commandFailed);
        rig.controller.dial("1");
        QVERIFY(rig.waitCommands(2));
        rig.server.sendResponse(rig.server.lastToken(), true);
        QCOMPARE(rig.controller.callState(), CallState::Outgoing);

        rig.controller.hangup();
        QVERIFY(rig.waitCommands(3));
        QCOMPARE(rig.server.lastCommand(), "hangup");
        rig.server.sendResponse(rig.server.lastToken(), false, "no active call\n");
        QTRY_COMPARE(rig.controller.callState(), CallState::Idle);
        QCOMPARE(ended.size(), 1);
        QCOMPARE(ended.first().first().toString(), "no active call");
        QCOMPARE(failed.size(), 0); // handled, not surfaced as a failure
    }

    void unknownTokenResponseIsIgnored()
    {
        Rig rig;
        QVERIFY(rig.connectAndRegister());
        QSignalSpy failed(&rig.controller, &CallController::commandFailed);
        rig.server.sendResponse("does-not-exist", false, "whatever");
        QTest::qWait(50);
        QCOMPARE(failed.size(), 0);
    }

    void dtmfOnlyDuringActiveCall()
    {
        Rig rig;
        QVERIFY(rig.connectAndRegister());
        rig.controller.sendDtmf('1');
        QTest::qWait(50);
        QCOMPARE(rig.server.received.size(), 1);

        rig.server.sendEvent("CALL_INCOMING", {{"peeruri", "sip:x@y"}});
        rig.server.sendEvent("CALL_ESTABLISHED");
        QTRY_COMPARE(rig.controller.callState(), CallState::Active);
        rig.controller.setLocalTones(false);
        rig.controller.sendDtmf('z'); // not a DTMF digit
        rig.controller.sendDtmf('#');
        QVERIFY(rig.waitCommands(2));
        QTest::qWait(30);
        QCOMPARE(rig.server.received.size(), 2); // no play command when tones are off
        QCOMPARE(rig.server.lastCommand(), "sndcode");
        QCOMPARE(rig.server.lastParams(), "#");
    }

    void disconnectResetsAndReportsBackendLost()
    {
        Rig rig;
        QVERIFY(rig.connectAndRegister());
        QSignalSpy lost(&rig.controller, &CallController::backendLost);
        rig.controller.dial("1");
        QCOMPARE(rig.controller.callState(), CallState::Outgoing);

        rig.server.dropClient();
        QTRY_COMPARE(lost.size(), 1);
        QCOMPARE(rig.controller.callState(), CallState::Idle);
        QCOMPARE(rig.controller.registrationState(), RegState::Unknown);
        QVERIFY(!rig.controller.backendAvailable());
    }

    void exitEventReportsBackendLost()
    {
        Rig rig;
        QVERIFY(rig.connectAndRegister());
        QSignalSpy lost(&rig.controller, &CallController::backendLost);
        rig.server.sendEvent("EXIT", {{"class", "application"}});
        QTRY_COMPARE(lost.size(), 1);
        QVERIFY(!rig.controller.backendAvailable());
    }

    void daemonBackendDownAndUpReseed()
    {
        Rig rig;
        QVERIFY(rig.connectAndRegister());
        QSignalSpy lost(&rig.controller, &CallController::backendLost);
        QSignalSpy ready(&rig.controller, &CallController::backendReady);
        rig.controller.dial("1");
        QVERIFY(rig.waitCommands(2));

        rig.server.sendEvent("BACKEND_DOWN", {{"class", "leansipd"}, {"param", "restarting baresip"}});
        QTRY_COMPARE(lost.size(), 1);
        QCOMPARE(lost.first().first().toString(), "restarting baresip");
        QCOMPARE(rig.controller.callState(), CallState::Idle);
        QCOMPARE(rig.controller.registrationState(), RegState::Unknown);
        QVERIFY(!rig.controller.backendAvailable());

        // Same connection, fresh baresip: registration is seeded again.
        rig.server.sendEvent("BACKEND_UP", {{"class", "leansipd"}});
        QTRY_COMPARE(ready.size(), 1);
        QVERIFY(rig.controller.backendAvailable());
        QVERIFY(rig.waitCommands(3));
        QCOMPARE(rig.server.lastCommand(), "reginfo");
    }

    void callsOfOtherClientsAreShownButNotOwned()
    {
        Rig rig;
        QVERIFY(rig.connectAndRegister());
        QSignalSpy peer(&rig.controller, &CallController::peerChanged);
        // leansipctl dialled: we only see the events.
        rig.server.sendEvent("CALL_RINGING", {{"peeruri", "sip:ivr@example.com"}});
        QTRY_COMPARE(rig.controller.callState(), CallState::Outgoing);
        QCOMPARE(rig.controller.peer(), "sip:ivr@example.com");
        QVERIFY(!rig.controller.ownsCall());
        rig.server.sendEvent("CALL_CLOSED", {{"param", "486 Busy Here"}});
        QTRY_COMPARE(rig.controller.callState(), CallState::Idle);
        QTest::qWait(50);
        QCOMPARE(rig.server.received.size(), 1); // no busy tone for someone else's call

        // An established call nobody here dialled still gets a peer.
        rig.server.sendEvent("CALL_ESTABLISHED", {{"peeruri", "sip:bob@example.com"}});
        QTRY_COMPARE(rig.controller.callState(), CallState::Active);
        QCOMPARE(rig.controller.peer(), "sip:bob@example.com");
        QVERIFY(!rig.controller.ownsCall());
    }

    void ownCallsAreOwnedUntilIdle()
    {
        Rig rig;
        QVERIFY(rig.connectAndRegister());
        rig.controller.dial("1");
        QVERIFY(rig.controller.ownsCall());
        rig.server.sendEvent("CALL_CLOSED", {{"param", "Connection reset by peer"}});
        QTRY_COMPARE(rig.controller.callState(), CallState::Idle);
        QVERIFY(!rig.controller.ownsCall());

        rig.server.sendEvent("CALL_INCOMING", {{"peeruri", "sip:carol@example.com"}});
        QTRY_COMPARE(rig.controller.callState(), CallState::Incoming);
        QVERIFY(!rig.controller.ownsCall());
        rig.controller.answer();
        QVERIFY(rig.controller.ownsCall());
    }

    void daemonModeTargetsTheCallAndAccountById()
    {
        Rig rig;
        rig.controller.setDaemonMode(true);
        rig.controller.setAccountAor("sip:me@example.com");
        QVERIFY(rig.connectAndRegister());
        auto steps = [&] { return QJsonDocument::fromJson(rig.server.lastParams().toUtf8()).array(); };
        auto seqReply = [&](bool ok, const QString &lastData) {
            const QJsonArray a{QJsonObject{{"command", "x"}, {"ok", true}, {"data", ""}},
                               QJsonObject{{"command", "y"}, {"ok", ok}, {"data", lastData}}};
            rig.server.sendResponse(rig.server.lastToken(), ok, QString::fromUtf8(QJsonDocument(a).toJson(QJsonDocument::Compact)));
        };

        rig.controller.dial("940");
        QVERIFY(rig.waitCommands(2));
        QCOMPARE(rig.server.lastCommand(), "leansipd_seq");
        QCOMPARE(steps(), (QJsonArray{QJsonArray{"uafind", "sip:me@example.com"}, QJsonArray{"dial", "940"}}));
        seqReply(true, "call id: abc123");
        QTRY_COMPARE(rig.controller.callId(), "abc123");

        rig.server.sendEvent("CALL_ESTABLISHED", {{"id", "abc123"}});
        QTRY_COMPARE(rig.controller.callState(), CallState::Active);
        rig.controller.sendDtmf('5');
        QVERIFY(rig.waitCommands(4)); // sequence + local tone
        QCOMPARE(rig.server.received.at(2).value("command").toString(), "leansipd_seq");
        QCOMPARE(QJsonDocument::fromJson(rig.server.received.at(2).value("params").toString().toUtf8()).array(),
                 (QJsonArray{QJsonArray{"callfind", "abc123"}, QJsonArray{"sndcode", "5"}}));

        QSignalSpy muted(&rig.controller, &CallController::muteChanged);
        rig.controller.toggleMute();
        QVERIFY(rig.waitCommands(5));
        QCOMPARE(steps().at(1).toArray().at(0).toString(), "mute");
        seqReply(true, "call muted");
        QTRY_COMPARE(muted.size(), 1);
        QVERIFY(rig.controller.isMuted());

        rig.controller.hangup();
        QVERIFY(rig.waitCommands(6));
        QCOMPARE(steps(), (QJsonArray{QJsonArray{"callfind", "abc123"}, QJsonArray{"hangup", ""}}));
        rig.server.sendEvent("CALL_CLOSED", {{"id", "abc123"}, {"param", "bye"}});
        QTRY_COMPARE(rig.controller.callState(), CallState::Idle);
        QVERIFY(!rig.controller.isMuted());
        QVERIFY(rig.controller.callId().isEmpty());
    }

    void anotherCallClosingLeavesOursAlone()
    {
        Rig rig;
        QVERIFY(rig.connectAndRegister());
        QSignalSpy others(&rig.controller, &CallController::otherCallsChanged);
        rig.server.sendEvent("CALL_INCOMING", {{"id", "mine"}, {"peeruri", "sip:a@x"}});
        QTRY_COMPARE(rig.controller.callState(), CallState::Incoming);
        rig.controller.answer();
        rig.server.sendEvent("CALL_ESTABLISHED", {{"id", "mine"}});
        QTRY_COMPARE(rig.controller.callState(), CallState::Active);

        rig.server.sendEvent("CALL_RINGING", {{"id", "agent"}, {"peeruri", "sip:ivr@x"}});
        QTRY_COMPARE(rig.controller.otherCallCount(), 1);
        rig.server.sendEvent("CALL_ESTABLISHED", {{"id", "agent"}});
        rig.server.sendEvent("CALL_CLOSED", {{"id", "agent"}, {"param", "486 Busy Here"}});
        QTRY_COMPARE(rig.controller.otherCallCount(), 0);
        QCOMPARE(rig.controller.callState(), CallState::Active);
        QCOMPARE(rig.controller.callId(), "mine");
        QCOMPARE(others.size(), 2);
    }

    void connectionFailureReportsBackendLost()
    {
        QTcpServer probe;
        QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
        const quint16 port = probe.serverPort();
        probe.close();

        BaresipClient client;
        CallController controller(&client);
        QSignalSpy lost(&controller, &CallController::backendLost);
        client.connectToHost("127.0.0.1", port, 300);
        QVERIFY(lost.wait(3000));
        QVERIFY(lost.first().first().toString().contains("cannot reach baresip"));
    }
};

QTEST_GUILESS_MAIN(TestCallController)
#include "tst_call_controller.moc"
