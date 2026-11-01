#include "fake_baresip_server.h"
#include "core/call_audio.h"
#include "ui/waveform_view.h"
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
        // The dot carries only its colour; the state text lives in the tooltip.
        QVERIFY(!m_window->registrationLabel()->text().contains("connecting"));
        QVERIFY(m_window->registrationLabel()->toolTip().contains("connecting"));
        QVERIFY(!m_window->isHelpVisible());
        QVERIFY(m_window->numberEdit()->hasFocus());
    }

    void numberFontIsBigAndShrinksToFit()
    {
        QCOMPARE(m_window->numberEdit()->alignment(), Qt::AlignCenter);
        QCOMPARE(m_window->statusLabel()->alignment(), Qt::AlignCenter);
        QCOMPARE(m_window->numberEdit()->placeholderText(), "enter number");
        const int placeholder = m_window->numberEdit()->font().pointSize();
        const int height = m_window->numberEdit()->height();
        const int windowHeight = m_window->height();

        m_window->numberEdit()->setText("+31201234567");
        const int big = m_window->numberEdit()->font().pointSize();
        QVERIFY(big >= 22); // a normal number, 12 monospace digits, stays large
        QVERIFY(placeholder < big / 2 + 2); // the hint is small

        m_window->numberEdit()->setText("sip:alice@example.com:5060");
        const int small = m_window->numberEdit()->font().pointSize();
        QVERIFY(small < big);
        QVERIFY(QFontMetrics(m_window->numberEdit()->font()).horizontalAdvance(m_window->numberEdit()->text())
                <= m_window->numberEdit()->width());

        // Beyond the minimum size the field simply scrolls; the font never drops below the floor.
        m_window->numberEdit()->setText("sip:a.very.long.username@some.long.domain.example.com");
        QCOMPARE(m_window->numberEdit()->font().pointSize(), 11);

        m_window->numberEdit()->setText("0612");
        QCOMPARE(m_window->numberEdit()->font().pointSize(), 30); // grows back
        m_window->numberEdit()->clear();
        QCOMPARE(m_window->numberEdit()->font().pointSize(), placeholder);

        m_window->numberEdit()->setText("1");
        QTest::qWait(100); // let the layout pass run
        QCOMPARE(m_window->numberEdit()->height(), height); // font size never moves the layout
        QCOMPARE(m_window->height(), windowHeight);
    }

    void windowHeightFollowsCurrentPage()
    {
        // The dialer must not be sized for the (taller) hidden help page.
        const QWidget *dialer = m_window->findChild<QWidget *>("dialerPage");
        QVERIFY(dialer);
        QCOMPARE(m_window->height(), dialer->sizeHint().height());
        QVERIFY(m_window->height() < 200);
        const int compact = m_window->height();

        QTest::keyClick(m_window->numberEdit(), Qt::Key_F1);
        QVERIFY(m_window->isHelpVisible());
        QTRY_VERIFY(m_window->height() > compact);
        QCOMPARE(m_window->width(), 272); // help text wraps; it never widens the window

        QTest::keyClick(m_window.get(), Qt::Key_Escape);
        QTRY_COMPARE(m_window->height(), compact);
        QCOMPARE(m_window->width(), 272);
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
        QTest::keyClicks(m_window->numberEdit(), "555");
        QTest::keyClick(m_window->numberEdit(), Qt::Key_Return);

        QTRY_COMPARE(m_server->received.size(), 2);
        QCOMPARE(m_server->lastCommand(), "dial");
        QCOMPARE(m_server->lastParams(), "555");
        QVERIFY(m_window->numberEdit()->isReadOnly());
        QVERIFY(m_window->statusLabel()->text().contains("555"));

        m_server->sendEvent("CALL_ESTABLISHED");
        QTRY_VERIFY(m_window->statusLabel()->text().contains("00:00"));

        // In a call the status line shows the peer and the clock; the field is empty (the
        // level display shows behind it) and shows the DTMF digits sent.
        QCOMPARE(m_window->statusLabel()->text(), "555 · 00:00");
        QCOMPARE(m_window->numberEdit()->text(), "");
        QCOMPARE(m_window->numberEdit()->placeholderText(), "");

        // A DTMF key sends the code plus a tone request and flashes the digit bright for ~400 ms.
        QTest::keyClick(m_window->numberEdit(), Qt::Key_9);
        QTRY_COMPARE(m_server->received.size(), 4);
        QCOMPARE(m_server->received.at(2).value("command").toString(), "sndcode");
        QCOMPARE(m_server->received.at(2).value("params").toString(), "9");
        QCOMPARE(m_server->lastCommand(), "play");
        QCOMPARE(m_server->lastParams(), "dtmf-9.wav");
        QCOMPARE(m_window->numberEdit()->text(), "9");
        QTRY_COMPARE(m_window->numberEdit()->text(), "");

        QTest::keyClick(m_window->numberEdit(), Qt::Key_NumberSign, Qt::ShiftModifier, 0);
        QTRY_COMPARE(m_server->received.size(), 6);
        QCOMPARE(m_server->received.at(4).value("params").toString(), "#");
        QCOMPARE(m_window->numberEdit()->text(), "#");
        QTest::keyClick(m_window->numberEdit(), Qt::Key_A); // not DTMF: swallowed
        QTest::qWait(30);
        QCOMPARE(m_server->received.size(), 6);
        QTRY_COMPARE(m_window->numberEdit()->text(), "");

        QTest::keyClick(m_window->numberEdit(), Qt::Key_Escape);
        QTRY_COMPARE(m_server->received.size(), 7);
        QCOMPARE(m_server->lastCommand(), "hangup");

        m_server->sendEvent("CALL_CLOSED", {{"param", "Normal"}});
        QTRY_VERIFY(!m_window->numberEdit()->isReadOnly());
        QCOMPARE(m_window->statusLabel()->text(), "Ended: Normal");
        QCOMPARE(m_window->lastDialed(), "555");
        QCOMPARE(m_window->numberEdit()->text(), "555"); // dialled number kept for a retry
        QCOMPARE(m_window->numberEdit()->placeholderText(), "enter number");
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
        // Ringing: the caller in the display, the hint below it.
        QTRY_COMPARE(m_window->numberEdit()->text(), "Bob");
        QCOMPARE(m_window->statusLabel()->text(), "press Enter to accept");

        QTest::keyClick(m_window->numberEdit(), Qt::Key_Return);
        QTRY_COMPARE(m_server->lastCommand(), "accept");
        m_server->sendEvent("CALL_ESTABLISHED");
        QTRY_COMPARE(m_controller->callState(), CallState::Active);
        QVERIFY(m_window->statusLabel()->text().startsWith("Bob · ")); // peer in the status line
        QCOMPARE(m_window->numberEdit()->text(), "");
        QTest::keyClick(m_window->numberEdit(), Qt::Key_1);
        QCOMPARE(m_window->numberEdit()->text(), "1");
        QTRY_COMPARE(m_window->numberEdit()->text(), "");

        m_server->sendEvent("CALL_CLOSED");
        QTRY_COMPARE(m_controller->callState(), CallState::Idle);
        QCOMPARE(m_window->numberEdit()->text(), ""); // nothing to restore after an incoming call
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

    void setAccountUpdatesHeader()
    {
        AccountInfo other;
        other.displayName = "Work";
        other.aor = "sip:1234@pbx.local";
        m_window->setAccount(other);
        QCOMPARE(m_window->findChild<QLabel *>("accountLabel")->text(), "Work");
        QCOMPARE(m_window->findChild<QLabel *>("accountLabel")->toolTip(), "sip:1234@pbx.local");

        // A profile label replaces the name; the address stays in the tooltip.
        other.title = "Desk";
        m_window->setAccount(other);
        QCOMPARE(m_window->findChild<QLabel *>("accountLabel")->text(), "Desk");
        QCOMPARE(m_window->findChild<QLabel *>("accountLabel")->toolTip(), "sip:1234@pbx.local");
    }

    void ctrlDigitAsksForAProfile()
    {
        QSignalSpy asked(m_window.get(), &MainWindow::profileShortcut);
        QTest::keyClick(m_window->numberEdit(), Qt::Key_3, Qt::ControlModifier);
        QTest::keyClick(m_window->numberEdit(), Qt::Key_0, Qt::ControlModifier | Qt::KeypadModifier);
        QCOMPARE(asked.size(), 2);
        QCOMPARE(asked.at(0).at(0).toInt(), 3);
        QCOMPARE(asked.at(1).at(0).toInt(), 0);
        QCOMPARE(m_window->numberEdit()->text(), ""); // never typed
        QTest::keyClick(m_window->numberEdit(), Qt::Key_3);
        QCOMPARE(asked.size(), 2);
        QCOMPARE(m_window->numberEdit()->text(), "3");
    }

    void ringingFlashesTheCase()
    {
        connectAndRegister();
        const QRgb plain = m_window->grab().toImage().pixel(6, 6);
        QVERIFY(!m_window->isRingFlashActive());
        m_server->sendEvent("CALL_INCOMING", {{"id", "c1"}, {"peeruri", "sip:bob@h"}});
        QTRY_VERIFY(m_window->isRingFlashActive());
        QVERIFY(m_window->isFlashing()); // lit straight away
        QCOMPARE(m_window->grab().toImage().pixel(6, 6), QColor("#f2c14e").rgb());
        // Toggles three times a second: off and back on within ~400 ms.
        QTRY_VERIFY_WITH_TIMEOUT(!m_window->isFlashing(), 400);
        QTRY_VERIFY_WITH_TIMEOUT(m_window->isFlashing(), 400);
        QCOMPARE(m_window->grab().toImage().pixel(6, 6), QColor("#f2c14e").rgb());

        // Answered: the flashing stops, unlit.
        m_server->sendEvent("CALL_ESTABLISHED", {{"id", "c1"}, {"peeruri", "sip:bob@h"}});
        QTRY_VERIFY(!m_window->isRingFlashActive());
        QVERIFY(!m_window->isFlashing());
        QCOMPARE(m_window->grab().toImage().pixel(6, 6), plain);

        // A call that stops ringing unanswered stops it too.
        m_server->sendEvent("CALL_CLOSED", {{"id", "c1"}});
        QTRY_COMPARE(m_controller->callState(), CallState::Idle);
        m_server->sendEvent("CALL_INCOMING", {{"id", "c2"}, {"peeruri", "sip:bob@h"}});
        QTRY_VERIFY(m_window->isRingFlashActive());
        m_server->sendEvent("CALL_CLOSED", {{"id", "c2"}});
        QTRY_VERIFY(!m_window->isRingFlashActive());
        QVERIFY(!m_window->isFlashing());
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
    }

    void mKeyMutesOnlyDuringACall()
    {
        connectAndRegister();
        QTest::keyClicks(m_window->numberEdit(), "m"); // idle: an ordinary character (sip:user)
        QCOMPARE(m_window->numberEdit()->text(), "m");
        m_window->numberEdit()->clear();

        QTest::keyClicks(m_window->numberEdit(), "555");
        QTest::keyClick(m_window->numberEdit(), Qt::Key_Return);
        QTRY_COMPARE(m_server->lastCommand(), "dial");
        m_server->sendResponse(m_server->lastToken(), true, "call id: c1");
        m_server->sendEvent("CALL_ESTABLISHED", {{"id", "c1"}, {"peeruri", "sip:555@example"}});
        QTRY_COMPARE(m_controller->callState(), CallState::Active);

        QTest::keyClick(m_window->numberEdit(), Qt::Key_M);
        QTRY_COMPARE(m_server->lastCommand(), "mute");
        QVERIFY(!m_window->muteLabel()->isVisible());
        m_server->sendResponse(m_server->lastToken(), true, "call muted");
        QTRY_VERIFY(m_window->muteLabel()->isVisible());
        QVERIFY(m_controller->isMuted());

        QTest::keyClick(m_window->numberEdit(), Qt::Key_M);
        QTRY_COMPARE(m_server->received.size(), 4); // reginfo, dial, mute, mute
        m_server->sendResponse(m_server->lastToken(), true, "call un-muted");
        QTRY_VERIFY(!m_window->muteLabel()->isVisible());

        // Ending the call clears the indicator.
        QTest::keyClick(m_window->numberEdit(), Qt::Key_M);
        QTRY_COMPARE(m_server->received.size(), 5);
        m_server->sendResponse(m_server->lastToken(), true, "call muted");
        QTRY_VERIFY(m_controller->isMuted());
        m_server->sendEvent("CALL_CLOSED", {{"id", "c1"}, {"param", "bye"}});
        QTRY_COMPARE(m_controller->callState(), CallState::Idle);
        QVERIFY(!m_window->muteLabel()->isVisible());
    }

    void otherCallsShowAsIndicator()
    {
        connectAndRegister();
        m_server->sendEvent("CALL_INCOMING", {{"id", "a"}, {"peeruri", "sip:alice@example"}});
        QTRY_COMPARE(m_controller->callState(), CallState::Incoming);
        m_server->sendEvent("CALL_RINGING", {{"id", "b"}, {"peeruri", "sip:ivr@example"}});
        QTRY_VERIFY(m_window->indicatorLabel()->text().contains("+1"));
        m_server->sendEvent("CALL_CLOSED", {{"id", "b"}, {"param", "bye"}});
        QTRY_VERIFY(!m_window->indicatorLabel()->text().contains("+1"));
        QCOMPARE(m_controller->callState(), CallState::Incoming); // another call ending is not ours
    }

    void activityBarsFadeOutFromTheCentre()
    {
        WaveformView view;
        view.setColor(QColor("#1d2615"));
        view.setBars(4, 1.0); // 2 per side; 4 bars and 3 gaps of 10 px
        view.setWindowMs(1000);
        view.resize(70, 41);
        // Ink is dark on the light palette background a grab paints behind the widget.
        auto inked = [](const QImage &img, int x, int y) { return qGray(img.pixel(x, y)) < 128; };

        // Silence: 1 px dashes on the middle line, nothing above or below it.
        QVector<float> points(Waveform::kLength, 0.0f);
        view.setPoints(points);
        QImage img = view.grab().toImage();
        for (const int x : {5, 25, 45, 65}) // bars: 0..10, 20..30 | 40..50, 60..70
            QVERIFY(inked(img, x, 20));
        QVERIFY(!inked(img, 15, 20)); // a gap
        QVERIFY(!inked(img, 5, 18) && !inked(img, 5, 22));

        // Loud audio throughout: full height next to the centre, the outermost bars still 1 px.
        points.fill(1.0f);
        view.setPoints(points);
        img = view.grab().toImage();
        QCOMPARE(view.offsetFor(1.0f), 20.5);
        QCOMPARE(view.offsetFor(0.5f), 10.25);
        QCOMPARE(view.offsetFor(3.0f), 20.5); // clamped
        QVERIFY(inked(img, 25, 2) && inked(img, 45, 2) && inked(img, 25, 38));
        QVERIFY(!inked(img, 5, 18) && !inked(img, 65, 18));
        QVERIFY(inked(img, 5, 20) && inked(img, 65, 20));

        // Audio older than the window does not show; the newest is next to the centre.
        points.fill(0.0f);
        std::fill(points.begin(), points.begin() + 200, 1.0f);
        points.last() = 1.0f;
        view.setPoints(points);
        img = view.grab().toImage();
        QVERIFY(inked(img, 25, 5) && inked(img, 45, 5));
        QVERIFY(!inked(img, 5, 18) && !inked(img, 65, 18));
    }

    void callAudioShowsWaveformsWhileTheCallRuns()
    {
        QTemporaryDir dir;
        const QString sent = dir.filePath("c1-sent.wav");
        const QString received = dir.filePath("c1-received.wav");
        writeGrowingWav(sent, {});
        writeGrowingWav(received, {});

        connectAndRegister();
        QTest::keyClicks(m_window->numberEdit(), "555");
        QTest::keyClick(m_window->numberEdit(), Qt::Key_Return);
        QTRY_COMPARE(m_server->lastCommand(), "dial");
        m_server->sendResponse(m_server->lastToken(), true, "call id: c1");
        m_server->sendEvent("CALL_ESTABLISHED", {{"id", "c1"}, {"peeruri", "sip:555@example"}});
        QTRY_COMPARE(m_controller->callState(), CallState::Active);
        QVERIFY(!m_window->receivedWave()->isVisible());
        QVERIFY(!m_window->sentWave()->isVisible());

        m_server->sendEvent("AUDIO_TAP", {{"class", "leansipd"}, {"id", "c1"}, {"sent", sent}, {"received", received}});
        QTRY_VERIFY(m_window->receivedWave()->isVisible());
        QVERIFY(m_window->sentWave()->isVisible());
        // Behind the number field, which stays on top.
        QVERIFY(m_window->numberEdit()->geometry().contains(m_window->receivedWave()->geometry()));
        const auto children = m_window->receivedWave()->parentWidget()->children();
        QVERIFY(children.indexOf(m_window->receivedWave()) < children.indexOf(m_window->numberEdit()));

        appendTone(received, 300.0, 0.5);
        QTRY_VERIFY(maxAbs(m_window->receivedWave()->points()) > 0.8f);
        QCOMPARE(maxAbs(m_window->sentWave()->points()), 0.0f);

        QTest::keyClick(m_window->numberEdit(), Qt::Key_M);
        QTRY_COMPARE(m_server->lastCommand(), "mute");
        m_server->sendResponse(m_server->lastToken(), true, "call muted");
        QTRY_VERIFY(!m_window->sentWave()->isVisible());
        QVERIFY(m_window->muteLabel()->isVisible()); // in the level's place, at its size
        QCOMPARE(m_window->muteLabel()->geometry(), m_window->sentWave()->geometry());
        QVERIFY(m_window->receivedWave()->isVisible());

        m_server->sendEvent("CALL_CLOSED", {{"id", "c1"}, {"param", "bye"}});
        QTRY_VERIFY(!m_window->receivedWave()->isVisible());
        QVERIFY(!m_window->audioMonitor()->isRunning());
    }

    // LEANSIP_SCREENSHOT_DIR=/some/dir ctest -R tst_main_window  saves the look for review.
    void screenshots()
    {
        const QString dir = qEnvironmentVariable("LEANSIP_SCREENSHOT_DIR");
        if (dir.isEmpty())
            QSKIP("set LEANSIP_SCREENSHOT_DIR to save screenshots");
        connectAndRegister();
        m_window->grab().save(dir + "/idle.png");
        m_window->setFocus();
        m_window->grab().save(dir + "/idle-unfocused.png");
        m_window->numberEdit()->setFocus();
        QTest::keyClicks(m_window->numberEdit(), "0612345678");
        m_window->grab().save(dir + "/typed.png");
        QTest::keyClick(m_window->numberEdit(), Qt::Key_Return);
        QTRY_COMPARE(m_server->lastCommand(), "dial");
        m_server->sendResponse(m_server->lastToken(), true, "call id: c1");
        m_server->sendEvent("CALL_ESTABLISHED", {{"id", "c1"}, {"peeruri", "sip:0612345678@example"}});
        QTRY_COMPARE(m_controller->callState(), CallState::Active);

        // Both directions talking: a voice-like mix far away, a quieter one from the mic.
        QTemporaryDir audio;
        const QString sent = audio.filePath("sent.wav");
        const QString received = audio.filePath("received.wav");
        writeGrowingWav(sent, {});
        writeGrowingWav(received, {});
        m_server->sendEvent("AUDIO_TAP", {{"class", "leansipd"}, {"id", "c1"}, {"sent", sent}, {"received", received}});
        QTRY_VERIFY(m_window->receivedWave()->isVisible());
        m_window->grab().save(dir + "/call-silent.png"); // nothing heard yet: flat lines
        for (int ms = 0; ms < 1000; ms += 100) { // a second of both directions talking
            appendVoice(received, 0.2, ms, 100);
            appendVoice(sent, 0.15, ms + 350, 100);
            m_window->audioMonitor()->poll();
        }
        m_window->grab().save(dir + "/call-audio.png");

        QTest::keyClick(m_window->numberEdit(), Qt::Key_M);
        QTRY_COMPARE(m_server->lastCommand(), "mute");
        m_server->sendResponse(m_server->lastToken(), true, "call muted");
        m_server->sendEvent("CALL_INCOMING", {{"id", "c2"}, {"peeruri", "sip:other@example"}});
        QTRY_VERIFY(m_window->indicatorLabel()->text().contains("+1"));
        m_window->grab().save(dir + "/call.png");
        QTest::keyClick(m_window->numberEdit(), Qt::Key_F1);
        QTRY_VERIFY(m_window->isHelpVisible());
        QTest::qWait(50);
        m_window->grab().save(dir + "/help.png");
    }

private:
    static float maxAbs(const QVector<float> &v)
    {
        float m = 0;
        for (const float x : v)
            m = std::max(m, std::abs(x));
        return m;
    }

    // A WAV the way a recorder leaves it while still writing (sizes 0), 8 kHz mono.
    static void writeGrowingWav(const QString &path, const QVector<qint16> &samples)
    {
        QByteArray h("RIFF\0\0\0\0WAVEfmt \x10\0\0\0\x01\0\x01\0\x40\x1f\0\0\x80\x3e\0\0\x02\0\x10\0data\0\0\0\0", 44);
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(h);
        f.write(reinterpret_cast<const char *>(samples.constData()), samples.size() * 2);
    }
    static void appendSamples(const QString &path, const QVector<qint16> &samples)
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::Append));
        f.write(reinterpret_cast<const char *>(samples.constData()), samples.size() * 2);
    }
    static void appendTone(const QString &path, double hz, double amplitude)
    {
        QVector<qint16> s(8000 / 4); // 250 ms
        for (qsizetype i = 0; i < s.size(); ++i)
            s[i] = static_cast<qint16>(amplitude * 32767 * std::sin(2 * M_PI * hz * static_cast<double>(i) / 8000));
        appendSamples(path, s);
    }
    // Harmonics of a 150 Hz voice, falling off like speech does above its fundamental, spoken
    // in syllables of about 200 ms: `ms` long, starting `startMs` into the utterance.
    static void appendVoice(const QString &path, double amplitude, int startMs, int ms)
    {
        QVector<qint16> s(8 * ms);
        for (qsizetype i = 0; i < s.size(); ++i) {
            const double t = (startMs * 8 + static_cast<double>(i)) / 8000;
            double v = 0;
            for (int k = 1; k <= 20; ++k)
                v += std::sin(2 * M_PI * 150 * k * t + k) / std::pow(k, 1.6);
            const double syllable = std::pow(std::abs(std::sin(M_PI * t / 0.2)), 1.5) * (0.6 + 0.4 * std::sin(2 * M_PI * t / 0.7));
            s[i] = static_cast<qint16>(std::clamp(amplitude * syllable * 32767 * v / 2, -32767.0, 32767.0));
        }
        appendSamples(path, s);
    }

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
