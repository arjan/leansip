#pragma once

#include "core/accounts_parser.h"
#include "core/call_audio.h"
#include "core/call_controller.h"

#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QStackedLayout>
#include <QTimer>
#include <QWidget>

class QKeyEvent;

namespace leansip {

class WaveformView;

// Frameless, keyboard-driven dialer. All interaction goes through handleKey():
//   digits/*/#/+  type (idle) or send DTMF (active call)
//   Enter         call / answer         Esc   hang up / decline / clear / quit
//   Up            recall last number    ?/F1  help         Ctrl+Q  quit
//   m             mute / unmute the microphone during a call
//   Ctrl+0..9     switch to the profile with that shortcut (profileShortcut())
class MainWindow : public QWidget {
    Q_OBJECT
public:
    MainWindow(const AccountInfo &account, CallController *controller, QWidget *parent = nullptr);

    // Fills the number field and dials (used for the CLI argument and forwarded numbers).
    void dialNumber(const QString &number);
    void raiseWindow();
    // Swap the account shown in the header.
    void setAccount(const AccountInfo &account);

public:
    // Accessors for tests.
    QLineEdit *numberEdit() const { return m_number; }
    QLabel *statusLabel() const { return m_status; }
    QLabel *registrationLabel() const { return m_registration; }
    QLabel *indicatorLabel() const { return m_indicators; }
    QLabel *muteLabel() const { return m_muteLabel; }
    WaveformView *receivedWave() const { return m_receivedWave; }
    WaveformView *sentWave() const { return m_sentWave; }
    CallAudioMonitor *audioMonitor() { return &m_audioMonitor; }
    bool isHelpVisible() const;
    // The background lit by an incoming call; toggles kRingFlashMs while it rings.
    bool isFlashing() const;
    bool isRingFlashActive() const { return m_ringFlash.isActive(); }
    QString lastDialed() const { return m_lastDialed; }

signals:
    // Ctrl+<digit>: the user asks for the profile with that shortcut.
    void profileShortcut(int digit);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    // Big digits by default; shrinks the font until the current text fits the field.
    void fitNumberFont();
    static constexpr int kWindowWidth = 272;
    static constexpr int kNumberMaxPt = 30;
    static constexpr int kNumberMinPt = 11;
    static constexpr int kPlaceholderPt = 12;
    void buildUi(const AccountInfo &account);
    QWidget *buildHelpPage();
    bool handleKey(QKeyEvent *event);
    void startCall();
    void escape();
    void toggleHelp();
    void updateForState(CallController::CallState state);
    void updateRegistration(CallController::RegistrationState state, const QString &detail);
    void showError(const QString &message);
    void tickElapsed();
    // "+N" (calls besides the shown one) and MUTE, which takes the microphone level's place,
    // in the corner of the display.
    void updateIndicators();
    static constexpr int kDtmfFlashMs = 400;
    // Three flashes a second: lit and unlit for 1/6 s each.
    static constexpr int kRingFlashMs = 167;
    void setFlash(bool on);
    // Waveforms during a call: what we receive, large across the display; what we send, small
    // in the top right corner while the microphone is on.
    void onCallAudio(const CallController::CallAudio &audio);
    void updateWaveforms();
    void placeReceivedWave();

    CallController *m_controller;
    QStackedLayout *m_pages = nullptr;
    QWidget *m_dialerPage = nullptr;
    QWidget *m_helpPage = nullptr;
    QLabel *m_accountLabel = nullptr;
    QLabel *m_registration = nullptr;
    QLineEdit *m_number = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_indicators = nullptr;
    QLabel *m_muteLabel = nullptr;
    WaveformView *m_receivedWave = nullptr;
    WaveformView *m_sentWave = nullptr;
    CallAudioMonitor m_audioMonitor;
    QString m_lastDialed;
    bool m_outgoingCall = false; // current/last call was started by us: restore the number afterwards
    CallController::CallState m_appliedState = CallController::CallState::Idle; // last state updateForState() saw
    QTimer m_elapsedTimer;
    QTimer m_dtmfFlash;
    QTimer m_ringFlash;
    QElapsedTimer m_callClock;
};

} // namespace leansip
