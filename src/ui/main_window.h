#pragma once

#include "core/accounts_parser.h"
#include "core/call_controller.h"

#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTimer>
#include <QWidget>

namespace leansip {

// Minimal dialer: account line, number entry, keypad, contextual call buttons.
class MainWindow : public QWidget {
    Q_OBJECT
public:
    MainWindow(const AccountInfo &account, CallController *controller, QWidget *parent = nullptr);

    // Fills the number field and dials (used for the CLI argument and forwarded numbers).
    void dialNumber(const QString &number);
    void raiseWindow();

    // Accessors for tests.
    QLineEdit *numberEdit() const { return m_number; }
    QPushButton *callButton() const { return m_call; }
    QPushButton *hangupButton() const { return m_hangup; }
    QPushButton *answerButton() const { return m_answer; }
    QPushButton *declineButton() const { return m_decline; }
    QLabel *statusLabel() const { return m_status; }
    QLabel *registrationLabel() const { return m_registration; }
    QPushButton *keypadButton(QChar key) const { return m_keys.value(key); }

private:
    void buildUi(const AccountInfo &account);
    void onKey(QChar key);
    void onCallClicked();
    void updateForState(CallController::CallState state);
    void updateRegistration(CallController::RegistrationState state, const QString &detail);
    void showError(const QString &message);
    void tickElapsed();

    CallController *m_controller;
    QLabel *m_accountLabel = nullptr;
    QLabel *m_registration = nullptr;
    QLineEdit *m_number = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_call = nullptr;
    QPushButton *m_hangup = nullptr;
    QPushButton *m_answer = nullptr;
    QPushButton *m_decline = nullptr;
    QHash<QChar, QPushButton *> m_keys;
    QTimer m_elapsedTimer;
    QElapsedTimer m_callClock;
};

} // namespace leansip
