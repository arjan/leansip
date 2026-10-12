#include "main_window.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QVBoxLayout>

namespace leansip {

namespace {

const QString &keypadLayout()
{
    static const QString keys = QStringLiteral("123456789*0#");
    return keys;
}

QString formatElapsed(qint64 ms)
{
    const qint64 secs = ms / 1000;
    return QStringLiteral("%1:%2").arg(secs / 60, 2, 10, QLatin1Char('0')).arg(secs % 60, 2, 10, QLatin1Char('0'));
}

} // namespace

MainWindow::MainWindow(const AccountInfo &account, CallController *controller, QWidget *parent)
    : QWidget(parent)
    , m_controller(controller)
{
    buildUi(account);

    connect(m_controller, &CallController::callStateChanged, this, &MainWindow::updateForState);
    connect(m_controller, &CallController::registrationStateChanged, this, &MainWindow::updateRegistration);
    connect(m_controller, &CallController::peerChanged, this, [this](const QString &peer) {
        if (m_controller->callState() == CallController::CallState::Incoming)
            m_status->setText(tr("Incoming: %1").arg(peer));
    });
    connect(m_controller, &CallController::callEnded, this, [this](const QString &reason) {
        m_status->setText(reason.isEmpty() ? tr("Call ended") : tr("Ended: %1").arg(reason));
    });
    connect(m_controller, &CallController::commandFailed, this, [this](const QString &cmd, const QString &detail) {
        showError(tr("%1 failed: %2").arg(cmd, detail));
    });
    connect(m_controller, &CallController::backendLost, this, [this](const QString &reason) {
        showError(reason);
        m_call->setEnabled(false);
    });
    connect(m_controller, &CallController::backendReady, this, [this] { updateForState(m_controller->callState()); });

    m_elapsedTimer.setInterval(1000);
    connect(&m_elapsedTimer, &QTimer::timeout, this, &MainWindow::tickElapsed);

    updateRegistration(m_controller->registrationState(), m_controller->registrationDetail());
    updateForState(m_controller->callState());
}

void MainWindow::buildUi(const AccountInfo &account)
{
    setWindowTitle(QStringLiteral("leansip"));
    setFixedWidth(300);

    auto *root = new QVBoxLayout(this);

    // Account + registration status.
    auto *header = new QHBoxLayout;
    m_accountLabel = new QLabel(account.label(), this);
    m_accountLabel->setToolTip(account.aor);
    m_accountLabel->setObjectName(QStringLiteral("accountLabel"));
    m_registration = new QLabel(this);
    m_registration->setObjectName(QStringLiteral("registrationLabel"));
    header->addWidget(m_accountLabel, 1);
    header->addWidget(m_registration);
    root->addLayout(header);

    m_number = new QLineEdit(this);
    m_number->setObjectName(QStringLiteral("numberEdit"));
    m_number->setPlaceholderText(tr("Number or sip:user@host"));
    m_number->setClearButtonEnabled(true);
    QFont font = m_number->font();
    font.setPointSize(font.pointSize() + 6);
    m_number->setFont(font);
    connect(m_number, &QLineEdit::returnPressed, this, &MainWindow::onCallClicked);
    connect(m_number, &QLineEdit::textChanged, this, [this] { updateForState(m_controller->callState()); });
    root->addWidget(m_number);

    auto *grid = new QGridLayout;
    const QString &keys = keypadLayout();
    for (int i = 0; i < keys.size(); ++i) {
        const QChar key = keys.at(i);
        auto *button = new QPushButton(QString(key), this);
        button->setObjectName(QStringLiteral("key_%1").arg(key));
        button->setMinimumSize(60, 44);
        button->setFocusPolicy(Qt::NoFocus);
        connect(button, &QPushButton::clicked, this, [this, key] { onKey(key); });
        grid->addWidget(button, i / 3, i % 3);
        m_keys.insert(key, button);
    }
    root->addLayout(grid);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("statusLabel"));
    m_status->setAlignment(Qt::AlignCenter);
    m_status->setWordWrap(true);
    root->addWidget(m_status);

    auto *buttons = new QHBoxLayout;
    m_call = new QPushButton(tr("Call"), this);
    m_call->setObjectName(QStringLiteral("callButton"));
    m_call->setDefault(true);
    m_hangup = new QPushButton(tr("Hang up"), this);
    m_hangup->setObjectName(QStringLiteral("hangupButton"));
    m_answer = new QPushButton(tr("Answer"), this);
    m_answer->setObjectName(QStringLiteral("answerButton"));
    m_decline = new QPushButton(tr("Decline"), this);
    m_decline->setObjectName(QStringLiteral("declineButton"));
    for (QPushButton *b : {m_call, m_hangup, m_answer, m_decline}) {
        b->setMinimumHeight(40);
        buttons->addWidget(b);
    }
    connect(m_call, &QPushButton::clicked, this, &MainWindow::onCallClicked);
    connect(m_hangup, &QPushButton::clicked, m_controller, &CallController::hangup);
    connect(m_answer, &QPushButton::clicked, m_controller, &CallController::answer);
    connect(m_decline, &QPushButton::clicked, m_controller, &CallController::hangup);
    root->addLayout(buttons);

    m_number->setFocus();
}

void MainWindow::onKey(QChar key)
{
    if (m_controller->callState() == CallController::CallState::Active) {
        m_controller->sendDtmf(key);
        return;
    }
    m_number->insert(QString(key));
    m_number->setFocus();
}

void MainWindow::onCallClicked()
{
    const QString number = m_number->text().trimmed();
    if (number.isEmpty() || m_controller->callState() != CallController::CallState::Idle)
        return;
    m_status->clear();
    m_controller->dial(number);
    // A queued dial leaves the state Idle, so no signal fires; refresh explicitly.
    updateForState(m_controller->callState());
}

void MainWindow::dialNumber(const QString &number)
{
    m_number->setText(number);
    raiseWindow();
    onCallClicked();
}

void MainWindow::raiseWindow()
{
    show();
    raise();
    activateWindow();
}

void MainWindow::updateForState(CallController::CallState state)
{
    using S = CallController::CallState;
    const bool idle = state == S::Idle;
    const bool canDial = idle && m_controller->backendAvailable() && !m_number->text().trimmed().isEmpty();

    m_call->setVisible(idle);
    m_call->setEnabled(canDial);
    m_hangup->setVisible(state == S::Outgoing || state == S::Active);
    m_answer->setVisible(state == S::Incoming);
    m_decline->setVisible(state == S::Incoming);
    m_number->setReadOnly(!idle);

    switch (state) {
    case S::Idle:
        m_elapsedTimer.stop();
        if (m_controller->hasPendingDial())
            m_status->setText(tr("Waiting for registration…"));
        break;
    case S::Outgoing:
        m_status->setText(tr("Calling %1…").arg(m_controller->peer()));
        break;
    case S::Incoming:
        m_status->setText(tr("Incoming: %1").arg(m_controller->peer()));
        break;
    case S::Active:
        m_callClock.start();
        m_elapsedTimer.start();
        tickElapsed();
        break;
    }
}

void MainWindow::updateRegistration(CallController::RegistrationState state, const QString &detail)
{
    using R = CallController::RegistrationState;
    QString colour;
    QString text;
    switch (state) {
    case R::Registered:
        colour = QStringLiteral("#2ecc40");
        text = tr("registered");
        break;
    case R::Failed:
        colour = QStringLiteral("#ff4136");
        text = tr("failed");
        break;
    case R::Unregistered:
        colour = QStringLiteral("#ff851b");
        text = tr("unregistered");
        break;
    case R::Unknown:
        colour = QStringLiteral("#aaaaaa");
        text = tr("connecting");
        break;
    }
    m_registration->setText(QStringLiteral("<span style=\"color:%1\">●</span> %2").arg(colour, text));
    m_registration->setToolTip(detail);
    updateForState(m_controller->callState());
}

void MainWindow::showError(const QString &message)
{
    m_status->setText(message);
}

void MainWindow::tickElapsed()
{
    m_status->setText(tr("Connected %1 · %2").arg(formatElapsed(m_callClock.elapsed()), m_controller->peer()));
}

} // namespace leansip
