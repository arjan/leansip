#include "main_window.h"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QVBoxLayout>
#include <QWindow>

namespace leansip {

namespace {

QString formatElapsed(qint64 ms)
{
    const qint64 secs = ms / 1000;
    return QStringLiteral("%1:%2").arg(secs / 60, 2, 10, QLatin1Char('0')).arg(secs % 60, 2, 10, QLatin1Char('0'));
}

bool isDtmf(const QString &text)
{
    static const QString allowed = QStringLiteral("0123456789*#");
    return text.size() == 1 && allowed.contains(text.at(0));
}

} // namespace

MainWindow::MainWindow(const AccountInfo &account, CallController *controller, QWidget *parent)
    : QWidget(parent, Qt::Window | Qt::FramelessWindowHint)
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
    connect(m_controller, &CallController::backendLost, this, [this](const QString &reason) { showError(reason); });
    connect(m_controller, &CallController::backendReady, this, [this] { updateForState(m_controller->callState()); });

    m_elapsedTimer.setInterval(1000);
    connect(&m_elapsedTimer, &QTimer::timeout, this, &MainWindow::tickElapsed);

    updateRegistration(m_controller->registrationState(), m_controller->registrationDetail());
    updateForState(m_controller->callState());
}

void MainWindow::buildUi(const AccountInfo &account)
{
    setWindowTitle(QStringLiteral("leansip"));
    setFixedWidth(320);
    setObjectName(QStringLiteral("leansip"));
    // Frameless windows have no border of their own; draw a thin one so the edge is visible.
    setStyleSheet(QStringLiteral(
        "#leansip { border: 1px solid palette(mid); }"
        "QLabel#hintLabel, QLabel#registrationLabel { color: palette(placeholder-text); }"
        "QLineEdit#numberEdit { border: none; border-bottom: 1px solid palette(mid); background: transparent; padding: 4px 0; }"));

    m_pages = new QStackedLayout(this);

    m_dialerPage = new QWidget(this);
    auto *root = new QVBoxLayout(m_dialerPage);
    root->setContentsMargins(16, 12, 16, 12);
    root->setSpacing(8);

    auto *header = new QHBoxLayout;
    m_accountLabel = new QLabel(account.label(), m_dialerPage);
    m_accountLabel->setToolTip(account.aor);
    m_accountLabel->setObjectName(QStringLiteral("accountLabel"));
    m_registration = new QLabel(m_dialerPage);
    m_registration->setObjectName(QStringLiteral("registrationLabel"));
    header->addWidget(m_accountLabel, 1);
    header->addWidget(m_registration);
    root->addLayout(header);

    m_number = new QLineEdit(m_dialerPage);
    m_number->setObjectName(QStringLiteral("numberEdit"));
    m_number->setPlaceholderText(tr("number or sip:user@host"));
    m_number->setFrame(false);
    QFont font = m_number->font();
    font.setPointSize(font.pointSize() + 8);
    m_number->setFont(font);
    m_number->installEventFilter(this);
    connect(m_number, &QLineEdit::textChanged, this, [this] { updateForState(m_controller->callState()); });
    root->addWidget(m_number);

    m_status = new QLabel(m_dialerPage);
    m_status->setObjectName(QStringLiteral("statusLabel"));
    m_status->setWordWrap(true);
    m_status->setMinimumHeight(m_status->fontMetrics().height() * 2);
    root->addWidget(m_status);

    m_hint = new QLabel(m_dialerPage);
    m_hint->setObjectName(QStringLiteral("hintLabel"));
    root->addWidget(m_hint);

    m_helpPage = buildHelpPage();

    m_pages->addWidget(m_dialerPage);
    m_pages->addWidget(m_helpPage);
    m_pages->setCurrentWidget(m_dialerPage);

    setFocusPolicy(Qt::StrongFocus);
    m_number->setFocus();
}

QWidget *MainWindow::buildHelpPage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(16, 12, 16, 12);
    auto *text = new QLabel(page);
    text->setObjectName(QStringLiteral("helpLabel"));
    text->setTextFormat(Qt::RichText);
    text->setText(tr(
        "<b>leansip keys</b>"
        "<table cellspacing='0' cellpadding='3'>"
        "<tr><td><tt>0-9 * # +</tt></td><td>type a number; DTMF during a call</td></tr>"
        "<tr><td><tt>Enter</tt></td><td>call &middot; answer</td></tr>"
        "<tr><td><tt>Esc</tt></td><td>hang up &middot; decline &middot; clear &middot; quit when empty</td></tr>"
        "<tr><td><tt>&uarr;</tt></td><td>recall last number</td></tr>"
        "<tr><td><tt>? F1</tt></td><td>this help</td></tr>"
        "<tr><td><tt>Ctrl+Q</tt></td><td>quit</td></tr>"
        "</table>"
        "<p>Drag anywhere to move the window.<br>Esc closes this screen.</p>"));
    layout->addWidget(text);
    layout->addStretch();
    return page;
}

bool MainWindow::isHelpVisible() const
{
    return m_pages->currentWidget() == m_helpPage;
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_number && event->type() == QEvent::KeyPress && handleKey(static_cast<QKeyEvent *>(event)))
        return true;
    return QWidget::eventFilter(watched, event);
}

void MainWindow::keyPressEvent(QKeyEvent *event)
{
    if (!handleKey(event))
        QWidget::keyPressEvent(event);
}

// Returns true when the key was consumed; otherwise the line edit gets it for ordinary editing.
bool MainWindow::handleKey(QKeyEvent *event)
{
    using S = CallController::CallState;
    const int key = event->key();
    const bool ctrl = event->modifiers().testFlag(Qt::ControlModifier);

    if (ctrl && key == Qt::Key_Q) {
        close();
        return true;
    }
    if (key == Qt::Key_Question || key == Qt::Key_F1) {
        toggleHelp();
        return true;
    }
    if (isHelpVisible()) {
        if (key == Qt::Key_Escape)
            toggleHelp();
        return true; // swallow everything else while help is up
    }

    const S state = m_controller->callState();
    switch (key) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (state == S::Incoming)
            m_controller->answer();
        else
            startCall();
        return true;
    case Qt::Key_Escape:
        escape();
        return true;
    case Qt::Key_Up:
        if (state == S::Idle && !m_lastDialed.isEmpty()) {
            m_number->setText(m_lastDialed);
            return true;
        }
        return false;
    default:
        break;
    }

    if (state == S::Active && isDtmf(event->text())) {
        m_controller->sendDtmf(event->text().at(0));
        return true;
    }
    // Typing is only meaningful while idle; during a call the field is read-only anyway.
    return state != S::Idle;
}

void MainWindow::startCall()
{
    const QString number = m_number->text().trimmed();
    if (number.isEmpty() || m_controller->callState() != CallController::CallState::Idle)
        return;
    m_status->clear();
    m_lastDialed = number;
    m_controller->dial(number);
    // A queued dial leaves the state Idle, so no signal fires; refresh explicitly.
    updateForState(m_controller->callState());
}

void MainWindow::escape()
{
    using S = CallController::CallState;
    switch (m_controller->callState()) {
    case S::Outgoing:
    case S::Active:
    case S::Incoming:
        m_controller->hangup();
        return;
    case S::Idle:
        if (m_controller->hasPendingDial()) {
            m_controller->hangup(); // drops the queued dial
            m_status->clear();
            updateForState(S::Idle);
        } else if (!m_number->text().isEmpty()) {
            m_number->clear();
            m_status->clear();
        } else {
            close();
        }
        return;
    }
}

void MainWindow::toggleHelp()
{
    if (isHelpVisible()) {
        m_pages->setCurrentWidget(m_dialerPage);
        m_number->setFocus();
    } else {
        m_pages->setCurrentWidget(m_helpPage);
        setFocus();
    }
}

void MainWindow::mousePressEvent(QMouseEvent *event)
{
    // No title bar: let the window system move us when the user drags anywhere.
    if (event->button() == Qt::LeftButton && windowHandle())
        windowHandle()->startSystemMove();
    QWidget::mousePressEvent(event);
}

void MainWindow::dialNumber(const QString &number)
{
    m_number->setText(number);
    raiseWindow();
    startCall();
}

void MainWindow::raiseWindow()
{
    show();
    raise();
    activateWindow();
    if (!isHelpVisible())
        m_number->setFocus();
}

void MainWindow::updateForState(CallController::CallState state)
{
    using S = CallController::CallState;
    m_number->setReadOnly(state != S::Idle);

    switch (state) {
    case S::Idle:
        m_elapsedTimer.stop();
        if (m_controller->hasPendingDial()) {
            m_status->setText(tr("Waiting for registration…"));
            m_hint->setText(tr("esc cancel  ·  ? help"));
        } else if (!m_controller->backendAvailable()) {
            m_hint->setText(tr("waiting for baresip  ·  ? help"));
        } else {
            m_hint->setText(tr("⏎ call  ·  ↑ redial  ·  esc clear  ·  ? help"));
        }
        break;
    case S::Outgoing:
        m_status->setText(tr("Calling %1…").arg(m_controller->peer()));
        m_hint->setText(tr("esc hang up"));
        break;
    case S::Incoming:
        m_status->setText(tr("Incoming: %1").arg(m_controller->peer()));
        m_hint->setText(tr("⏎ answer  ·  esc decline"));
        break;
    case S::Active:
        m_callClock.start();
        m_elapsedTimer.start();
        tickElapsed();
        m_hint->setText(tr("0-9 * # dtmf  ·  esc hang up"));
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
