#include "main_window.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QStyle>
#include <QVBoxLayout>
#include <QWindow>

namespace leansip {

namespace {

// A stacked layout sizes the window from each page's raw sizeHint(), which a page's fixed width
// does not bound (a 30pt line edit alone asks for ~360px). Pin the hint's width so the window
// stays at kWindowWidth and only its height follows the content.
class FixedWidthPage : public QWidget {
public:
    FixedWidthPage(int width, QWidget *parent)
        : QWidget(parent)
        , m_width(width)
    {
        setFixedWidth(width);
    }
    // With word-wrapping labels the layout's plain hint assumes an arbitrary width; ask for the
    // height at the width the page really has.
    QSize sizeHint() const override
    {
        const int h = layout() && layout()->hasHeightForWidth() ? layout()->totalHeightForWidth(m_width)
                                                                 : QWidget::sizeHint().height();
        return {m_width, h};
    }
    QSize minimumSizeHint() const override { return {m_width, QWidget::minimumSizeHint().height()}; }
    // The height is already resolved for the fixed width above. Reporting height-for-width would
    // make the stacked layout size the window for the tallest page, hidden ones included.
    bool hasHeightForWidth() const override { return false; }

private:
    int m_width;
};

QString formatElapsed(qint64 ms)
{
    const qint64 secs = ms / 1000;
    return QStringLiteral("%1:%2").arg(secs / 60, 2, 10, QLatin1Char('0')).arg(secs % 60, 2, 10, QLatin1Char('0'));
}

// A small handset look: beige case, recessed LCD, monospace digits. Fixed colours, so it looks
// the same in light and dark desktop themes.
constexpr auto kCase = "#e7e1cf";
constexpr auto kInk = "#33352d";
constexpr auto kLcd = "#b7c29d";
constexpr auto kLcdInk = "#1d2615";
constexpr auto kLcdDim = "#62704f";

QFont monoFont(int pointSize, bool bold = false)
{
    QFont f(QStringLiteral("DejaVu Sans Mono"));
    f.setStyleHint(QFont::Monospace);
    f.setPointSize(pointSize);
    f.setBold(bold);
    return f;
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
        // Back to normal editing: bright text, dialled number restored for a retry.
        m_dtmfFlash.stop();
        setNumberDim(false);
        m_number->setText(m_outgoingCall ? m_lastDialed : QString());
    });
    connect(m_controller, &CallController::commandFailed, this, [this](const QString &cmd, const QString &detail) {
        showError(tr("%1 failed: %2").arg(cmd, detail));
    });
    connect(m_controller, &CallController::backendLost, this, [this](const QString &reason) { showError(reason); });
    connect(m_controller, &CallController::backendReady, this, [this] { updateForState(m_controller->callState()); });
    connect(m_controller, &CallController::muteChanged, this, &MainWindow::updateIndicators);
    connect(m_controller, &CallController::otherCallsChanged, this, &MainWindow::updateIndicators);

    m_elapsedTimer.setInterval(1000);
    connect(&m_elapsedTimer, &QTimer::timeout, this, &MainWindow::tickElapsed);
    m_dtmfFlash.setSingleShot(true);
    m_dtmfFlash.setInterval(kDtmfFlashMs);
    connect(&m_dtmfFlash, &QTimer::timeout, this, &MainWindow::showPeerNumber);

    updateRegistration(m_controller->registrationState(), m_controller->registrationDetail());
    updateForState(m_controller->callState());
}

void MainWindow::buildUi(const AccountInfo &account)
{
    setWindowTitle(QStringLiteral("leansip"));
    setFixedWidth(kWindowWidth);
    setObjectName(QStringLiteral("leansip"));
    // Frameless windows have no border of their own; the case outline is drawn here.
    setStyleSheet(QStringLiteral(
                      "#leansip { background: %1; border: 2px solid %2; }"
                      "QLabel { color: %2; }"
                      "QFrame#lcd { background: %3; border: 2px solid; border-radius: 3px;"
                      "  border-color: #4c5440 #d9e0c6 #d9e0c6 #4c5440; }"
                      "QFrame#lcd QLabel { color: %4; }"
                      "QLineEdit#numberEdit { border: none; background: transparent; padding: 0; }")
                      .arg(QLatin1String(kCase), QLatin1String(kInk), QLatin1String(kLcd), QLatin1String(kLcdInk)));
    setAttribute(Qt::WA_StyledBackground, true); // a plain QWidget ignores QSS background/border otherwise

    m_pages = new QStackedLayout(this);
    m_pages->setContentsMargins(0, 0, 0, 0); // the pages' own margins keep content off the case border
    // Track the current page's height exactly. A stacked layout's hint is the max over all
    // pages, which would size the dialer for the taller help page; the help page is therefore
    // ignored for sizing until it is shown (see toggleHelp()).
    m_pages->setSizeConstraint(QLayout::SetFixedSize);

    m_dialerPage = new FixedWidthPage(kWindowWidth, this);
    m_dialerPage->setObjectName(QStringLiteral("dialerPage"));
    auto *root = new QVBoxLayout(m_dialerPage);
    root->setContentsMargins(10, 8, 10, 10);
    root->setSpacing(6);

    // Centred "● ACCOUNT" line; the dot is the registration LED, details in its tooltip.
    auto *header = new QHBoxLayout;
    header->setSpacing(6);
    m_registration = new QLabel(m_dialerPage);
    m_registration->setObjectName(QStringLiteral("registrationLabel"));
    m_accountLabel = new QLabel(account.label(), m_dialerPage);
    m_accountLabel->setToolTip(account.aor);
    m_accountLabel->setObjectName(QStringLiteral("accountLabel"));
    m_accountLabel->setFont(monoFont(9, true));
    header->addStretch(1);
    header->addWidget(m_registration);
    header->addWidget(m_accountLabel);
    header->addStretch(1);
    root->addLayout(header);

    auto *lcd = new QFrame(m_dialerPage);
    lcd->setObjectName(QStringLiteral("lcd"));
    auto *lcdLayout = new QVBoxLayout(lcd);
    lcdLayout->setContentsMargins(4, 2, 4, 4);
    lcdLayout->setSpacing(0);

    m_indicators = new QLabel(lcd);
    m_indicators->setObjectName(QStringLiteral("indicatorLabel"));
    m_indicators->setAlignment(Qt::AlignRight);
    m_indicators->setFont(monoFont(7, true));
    m_indicators->setTextFormat(Qt::RichText);
    m_indicators->setFixedHeight(m_indicators->fontMetrics().height()); // reserved, so nothing jumps
    lcdLayout->addWidget(m_indicators);

    m_number = new QLineEdit(lcd);
    m_number->setObjectName(QStringLiteral("numberEdit"));
    m_number->setPlaceholderText(tr("enter number"));
    m_number->setAlignment(Qt::AlignCenter);
    m_number->setFrame(false);
    m_number->setFont(monoFont(kNumberMaxPt, true));
    QPalette numberPalette = m_number->palette();
    numberPalette.setColor(QPalette::Text, QColor(QLatin1String(kLcdInk)));
    numberPalette.setColor(QPalette::PlaceholderText, QColor(QLatin1String(kLcdDim)));
    m_number->setPalette(numberPalette);
    m_number->installEventFilter(this);
    connect(m_number, &QLineEdit::textChanged, this, [this] {
        fitNumberFont();
        updateForState(m_controller->callState());
    });
    lcdLayout->addWidget(m_number);

    m_status = new QLabel(lcd);
    m_status->setObjectName(QStringLiteral("statusLabel"));
    m_status->setAlignment(Qt::AlignCenter);
    m_status->setWordWrap(true);
    m_status->setFont(monoFont(8));
    m_status->setMinimumHeight(m_status->fontMetrics().height()); // reserve one line so the window doesn't jump
    lcdLayout->addWidget(m_status);
    root->addWidget(lcd);

    m_helpPage = buildHelpPage();

    m_pages->addWidget(m_dialerPage);
    m_pages->addWidget(m_helpPage);
    m_pages->setCurrentWidget(m_dialerPage);

    setFocusPolicy(Qt::StrongFocus);
    m_number->setFocus();
}

QWidget *MainWindow::buildHelpPage()
{
    auto *page = new FixedWidthPage(kWindowWidth, this);
    page->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Ignored); // not counted while hidden
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(16, 10, 16, 10);
    auto *text = new QLabel(page);
    text->setObjectName(QStringLiteral("helpLabel"));
    text->setTextFormat(Qt::RichText);
    text->setWordWrap(true);
    // A wrapping label's sizeHint is clamped to its maximum width; the stacked layout sizes the
    // window from page hints, so this is what actually keeps the help page at window width.
    text->setMaximumWidth(kWindowWidth - 2 * 16);
    text->setText(tr(
        // Plain wrapped lines rather than a table: a table has a minimum column width that would
        // force the window wider than the dialer.
        "<p><b>leansip keys</b></p>"
        "<p><b>0-9 * # +</b> &nbsp;type a number; DTMF during a call<br>"
        "<b>Enter</b> &nbsp;call &middot; answer<br>"
        "<b>Esc</b> &nbsp;hang up &middot; decline &middot; clear &middot; quit when empty<br>"
        "<b>&uarr;</b> &nbsp;recall last number<br>"
        "<b>m</b> &nbsp;mute &middot; unmute the microphone in a call<br>"
        "<b>? F1</b> &nbsp;this help<br>"
        "<b>F2</b> &nbsp;accounts: add, edit, reorder; Enter picks the active one, "
        "Ctrl+S saves after a real registration<br>"
        "<b>Ctrl+Q</b> &nbsp;quit</p>"
        "<p>Drag anywhere to move the window. Esc closes this screen.</p>"));
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
    if (watched == m_number) {
        if (event->type() == QEvent::KeyPress && handleKey(static_cast<QKeyEvent *>(event)))
            return true;
        if (event->type() == QEvent::Resize)
            fitNumberFont(); // the field's width is only known once the layout has run
    }
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
    if (key == Qt::Key_F2) {
        if (isHelpVisible())
            toggleHelp();
        emit configureRequested();
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

    if (state == S::Active && event->modifiers() == Qt::NoModifier && event->key() == Qt::Key_M) {
        m_controller->toggleMute();
        return true;
    }
    if (state == S::Active && isDtmf(event->text())) {
        m_controller->sendDtmf(event->text().at(0));
        // Flash the digit bright, then fall back to the dimmed peer number.
        m_number->setText(event->text());
        setNumberDim(false);
        m_dtmfFlash.start();
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
    m_outgoingCall = true;
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
        m_helpPage->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Ignored);
        m_pages->setCurrentWidget(m_dialerPage);
        m_number->setFocus();
    } else {
        m_helpPage->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred); // let it size the window's height
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

void MainWindow::fitNumberFont()
{
    const QString text = m_number->text().isEmpty() ? m_number->placeholderText() : m_number->text();
    // Fixed slack (not font-relative) so repeated fits of the same text converge on one size.
    const int available = m_number->contentsRect().width() - 8;
    QFont font = m_number->font();
    int pt = kNumberMaxPt;
    for (; pt > kNumberMinPt; --pt) {
        font.setPointSize(pt);
        if (QFontMetrics(font).horizontalAdvance(text) <= available)
            break;
    }
    font.setPointSize(pt);
    if (font.pointSize() != m_number->font().pointSize())
        m_number->setFont(font);
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
        m_number->setPlaceholderText(tr("enter number"));
        if (m_controller->hasPendingDial())
            m_status->setText(tr("Waiting for registration…"));
        break;
    case S::Outgoing:
        m_status->setText(tr("Calling %1…").arg(m_controller->peer()));
        break;
    case S::Incoming:
        m_outgoingCall = false;
        m_status->setText(tr("Incoming: %1").arg(m_controller->peer()));
        break;
    case S::Active:
        // Entering the call (not every refresh): the peer moves to the status line and the
        // field starts collecting DTMF digits.
        if (m_appliedState != S::Active) {
            showPeerNumber();
            m_callClock.start();
            m_elapsedTimer.start();
            tickElapsed();
        }
        break;
    }
    m_appliedState = state;
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
    m_registration->setText(QStringLiteral("<span style=\"color:%1\">●</span>").arg(colour));
    m_registration->setToolTip(detail.isEmpty() ? text : QStringLiteral("%1 — %2").arg(text, detail));
    updateForState(m_controller->callState());
}

void MainWindow::setAccount(const AccountInfo &account)
{
    m_accountLabel->setText(account.label());
    m_accountLabel->setToolTip(account.aor);
}

void MainWindow::showError(const QString &message)
{
    m_status->setText(message.simplified()); // never let stray newlines grow the window
}

void MainWindow::tickElapsed()
{
    // The peer is in the number field during a call; the status just carries the clock.
    m_status->setText(tr("Connected %1").arg(formatElapsed(m_callClock.elapsed())));
}

void MainWindow::showPeerNumber()
{
    m_number->setText(m_controller->peer());
    setNumberDim(true);
}

void MainWindow::updateIndicators()
{
    QStringList tags;
    const QString tag = QStringLiteral("<span style=\"background:%1; color:%2;\">&nbsp;%3&nbsp;</span>");
    if (m_controller->isMuted())
        tags << tag.arg(QLatin1String(kLcdInk), QLatin1String(kLcd), tr("MUTE"));
    if (const int others = m_controller->otherCallCount(); others > 0)
        tags << tag.arg(QLatin1String(kLcdInk), QLatin1String(kLcd), tr("+%n CALL(S)", nullptr, others));
    m_indicators->setText(tags.join(QStringLiteral("&nbsp;")));
}

void MainWindow::setNumberDim(bool dim)
{
    if (m_number->property("dim").toBool() == dim)
        return;
    m_number->setProperty("dim", dim);
    QPalette p = m_number->palette();
    p.setColor(QPalette::Text, QColor(QLatin1String(dim ? kLcdDim : kLcdInk)));
    m_number->setPalette(p);
}

} // namespace leansip
