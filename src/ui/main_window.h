#pragma once

#include "core/accounts_parser.h"
#include "core/call_controller.h"

#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QStackedLayout>
#include <QTimer>
#include <QWidget>

class QKeyEvent;

namespace leansip {

// Frameless, keyboard-driven dialer. All interaction goes through handleKey():
//   digits/*/#/+  type (idle) or send DTMF (active call)
//   Enter         call / answer         Esc   hang up / decline / clear / quit
//   Up            recall last number    ?/F1  help         Ctrl+Q  quit
//   m             mute / unmute the microphone during a call
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
    bool isHelpVisible() const;
    QString lastDialed() const { return m_lastDialed; }

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
    // In a call the field shows the peer number dimmed; a sent DTMF digit flashes bright briefly.
    void showPeerNumber();
    void setNumberDim(bool dim);
    // "MUTE" and "+N" (calls besides the shown one) in the corner of the display.
    void updateIndicators();
    static constexpr int kDtmfFlashMs = 400;

    CallController *m_controller;
    QStackedLayout *m_pages = nullptr;
    QWidget *m_dialerPage = nullptr;
    QWidget *m_helpPage = nullptr;
    QLabel *m_accountLabel = nullptr;
    QLabel *m_registration = nullptr;
    QLineEdit *m_number = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_indicators = nullptr;
    QString m_lastDialed;
    bool m_outgoingCall = false; // current/last call was started by us: restore the number afterwards
    CallController::CallState m_appliedState = CallController::CallState::Idle; // last state updateForState() saw
    QTimer m_elapsedTimer;
    QTimer m_dtmfFlash;
    QElapsedTimer m_callClock;
};

} // namespace leansip
