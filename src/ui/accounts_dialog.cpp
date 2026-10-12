#include "accounts_dialog.h"

#include <QFormLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QVBoxLayout>
#include <QWindow>

namespace leansip {

namespace {
const char *const kTransports[] = {"", "udp", "tcp", "tls"};
}

AccountsDialog::AccountsDialog(AccountsFile initial, AccountValidator *validator, QWidget *parent)
    : QDialog(parent, Qt::Dialog | Qt::FramelessWindowHint)
    , m_working(std::move(initial))
    , m_validator(validator)
{
    buildUi();

    connect(m_validator, &AccountValidator::progress, this, &AccountsDialog::showStatus);
    connect(m_validator, &AccountValidator::accountResult, this, [this](const QString &aor, bool ok, const QString &) {
        m_results.insert(aor, ok);
        refreshList();
    });
    connect(m_validator, &AccountValidator::finished, this, [this](bool ok, const QString &summary) {
        if (ok) {
            showStatus(tr("✓ %1").arg(summary));
            emit saved(m_working);
            accept();
            return;
        }
        showStatus(tr("✗ %1").arg(summary));
        setFormEnabled(true);
    });

    refreshList();
    if (m_working.accounts.isEmpty())
        addAccount();
    else
        selectRow(0);
}

void AccountsDialog::buildUi()
{
    setWindowTitle(tr("leansip accounts"));
    setObjectName(QStringLiteral("accountsDialog"));
    setFixedWidth(380);
    setStyleSheet(QStringLiteral(
        "#accountsDialog { border: 1px solid palette(mid); }"
        "QLabel#hintLabel { color: palette(placeholder-text); }"));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 10, 16, 10);
    root->setSpacing(6);

    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("accountList"));
    m_list->setFixedHeight(m_list->fontMetrics().height() * 4 + 8);
    m_list->installEventFilter(this);
    connect(m_list, &QListWidget::currentRowChanged, this, [this](int row) {
        if (!m_loading)
            loadFields(row);
    });
    root->addWidget(m_list);

    m_form = new QWidget(this);
    auto *form = new QFormLayout(m_form);
    form->setContentsMargins(0, 0, 0, 0);
    form->setHorizontalSpacing(10);
    form->setVerticalSpacing(4);

    auto makeEdit = [this](const char *name, const QString &placeholder) {
        auto *edit = new QLineEdit(m_form);
        edit->setObjectName(QLatin1String(name));
        edit->setPlaceholderText(placeholder);
        edit->installEventFilter(this);
        connect(edit, &QLineEdit::textEdited, this, &AccountsDialog::storeFields);
        return edit;
    };
    m_displayName = makeEdit("displayNameEdit", tr("optional"));
    m_user = makeEdit("userEdit", tr("e.g. 1234"));
    m_domain = makeEdit("domainEdit", tr("sip.example.com"));
    m_transport = new QComboBox(m_form);
    m_transport->setObjectName(QStringLiteral("transportBox"));
    for (const char *t : kTransports)
        m_transport->addItem(*t ? QLatin1String(t) : tr("default"), QLatin1String(t));
    m_transport->installEventFilter(this);
    connect(m_transport, &QComboBox::currentIndexChanged, this, [this] {
        if (!m_loading)
            storeFields();
    });
    m_authUser = makeEdit("authUserEdit", tr("same as user"));
    m_password = makeEdit("passwordEdit", QString());
    m_password->setEchoMode(QLineEdit::Password);
    m_outbound = makeEdit("outboundEdit", tr("sip:proxy.example.com (optional)"));

    form->addRow(tr("Display name"), m_displayName);
    form->addRow(tr("User"), m_user);
    form->addRow(tr("Domain"), m_domain);
    form->addRow(tr("Transport"), m_transport);
    form->addRow(tr("Auth user"), m_authUser);
    form->addRow(tr("Password"), m_password);
    form->addRow(tr("Outbound proxy"), m_outbound);
    root->addWidget(m_form);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("statusLabel"));
    m_status->setAlignment(Qt::AlignCenter);
    m_status->setWordWrap(true);
    m_status->setMinimumHeight(m_status->fontMetrics().height());
    root->addWidget(m_status);

    m_hint = new QLabel(tr("↑↓ select · ⏎ make active · ^↑↓ move · ^N add · Del remove · ^H show password · ^S save · Esc close"), this);
    m_hint->setObjectName(QStringLiteral("hintLabel"));
    m_hint->setWordWrap(true);
    m_hint->setAlignment(Qt::AlignCenter);
    root->addWidget(m_hint);
}

QString AccountsDialog::rowText(int index) const
{
    const AccountRecord &r = m_working.accounts.at(index);
    QString dot = QStringLiteral("○");
    if (const auto it = m_results.constFind(r.aor()); it != m_results.constEnd())
        dot = *it ? QStringLiteral("●") : QStringLiteral("✗");
    const QString label = r.displayName.isEmpty() ? r.aor() : QStringLiteral("%1  (%2)").arg(r.displayName, r.aor());
    return QStringLiteral("%1 %2%3").arg(dot, label, index == 0 ? tr("  — active") : QString());
}

void AccountsDialog::refreshList()
{
    m_loading = true;
    const int current = m_list->currentRow();
    m_list->clear();
    for (int i = 0; i < m_working.accounts.size(); ++i)
        m_list->addItem(rowText(i));
    if (current >= 0 && current < m_list->count())
        m_list->setCurrentRow(current);
    m_loading = false;
}

void AccountsDialog::selectRow(int index)
{
    if (index < 0 || index >= m_list->count())
        return;
    m_list->setCurrentRow(index); // currentRowChanged -> loadFields
    m_list->setFocus();
}

void AccountsDialog::loadFields(int index)
{
    m_loading = true;
    const bool valid = index >= 0 && index < m_working.accounts.size();
    const AccountRecord r = valid ? m_working.accounts.at(index) : AccountRecord{};
    m_displayName->setText(r.displayName);
    m_user->setText(r.user);
    m_domain->setText(r.domain);
    m_transport->setCurrentIndex(std::max(0, m_transport->findData(r.transport)));
    m_authUser->setText(r.authUser);
    m_password->setText(r.password);
    m_outbound->setText(r.outbound);
    m_form->setEnabled(valid && !m_validator->isBusy());
    m_loading = false;
}

void AccountsDialog::storeFields()
{
    const int index = m_list->currentRow();
    if (m_loading || index < 0 || index >= m_working.accounts.size())
        return;
    AccountRecord &r = m_working.accounts[index];
    r.displayName = m_displayName->text().trimmed();
    r.user = m_user->text().trimmed();
    r.domain = m_domain->text().trimmed();
    r.transport = m_transport->currentData().toString();
    r.authUser = m_authUser->text().trimmed();
    r.password = m_password->text();
    r.outbound = m_outbound->text().trimmed();
    m_results.remove(r.aor()); // edited: previous result no longer applies
    m_loading = true;
    m_list->item(index)->setText(rowText(index));
    m_loading = false;
    m_status->clear();
}

void AccountsDialog::addAccount()
{
    if (m_validator->isBusy())
        return;
    m_working.accounts.append(AccountRecord{});
    refreshList();
    selectRow(static_cast<int>(m_working.accounts.size()) - 1);
    m_user->setFocus();
}

void AccountsDialog::removeAccount()
{
    const int index = m_list->currentRow();
    if (m_validator->isBusy() || index < 0 || index >= m_working.accounts.size())
        return;
    m_working.accounts.removeAt(index);
    refreshList();
    if (m_working.accounts.isEmpty())
        loadFields(-1);
    else
        selectRow(std::min(index, static_cast<int>(m_working.accounts.size()) - 1));
}

void AccountsDialog::moveAccount(int delta)
{
    const int index = m_list->currentRow();
    const int target = index + delta;
    if (m_validator->isBusy() || index < 0 || target < 0 || target >= m_working.accounts.size())
        return;
    m_working.accounts.swapItemsAt(index, target);
    refreshList();
    selectRow(target);
}

void AccountsDialog::makeActive()
{
    const int index = m_list->currentRow();
    if (m_validator->isBusy() || index <= 0 || index >= m_working.accounts.size())
        return;
    m_working.accounts.move(index, 0);
    refreshList();
    selectRow(0);
}

bool AccountsDialog::canSave() const
{
    return !m_callActive && !m_validator->isBusy() && !m_working.accounts.isEmpty();
}

void AccountsDialog::setCallActive(bool active)
{
    m_callActive = active;
    if (active)
        showStatus(tr("hang up before saving: saving restarts baresip"));
}

void AccountsDialog::save()
{
    if (m_callActive) {
        showStatus(tr("hang up before saving: saving restarts baresip"));
        return;
    }
    if (!canSave())
        return;
    for (int i = 0; i < m_working.accounts.size(); ++i) {
        QString why;
        if (!accounts::validateRecord(m_working.accounts.at(i), &why)) {
            selectRow(i);
            showStatus(tr("✗ %1").arg(why));
            return;
        }
    }
    m_results.clear();
    refreshList();
    setFormEnabled(false);
    m_validator->validate(m_working);
}

void AccountsDialog::cancel()
{
    if (m_validator->isBusy()) {
        m_validator->abort();
        return;
    }
    reject();
}

void AccountsDialog::togglePasswordEcho()
{
    m_password->setEchoMode(m_password->echoMode() == QLineEdit::Password ? QLineEdit::Normal : QLineEdit::Password);
}

void AccountsDialog::setFormEnabled(bool enabled)
{
    m_form->setEnabled(enabled && m_list->currentRow() >= 0);
    m_list->setEnabled(enabled);
}

void AccountsDialog::showStatus(const QString &text)
{
    m_status->setText(text.simplified());
}

bool AccountsDialog::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() != QEvent::KeyPress)
        return QDialog::eventFilter(watched, event);
    auto *key = static_cast<QKeyEvent *>(event);
    const bool ctrl = key->modifiers().testFlag(Qt::ControlModifier);

    // Dialog-wide chords, wherever the focus is.
    if (ctrl) {
        switch (key->key()) {
        case Qt::Key_S:
            save();
            return true;
        case Qt::Key_N:
            addAccount();
            return true;
        case Qt::Key_H:
            togglePasswordEcho();
            return true;
        case Qt::Key_Up:
            moveAccount(-1);
            return true;
        case Qt::Key_Down:
            moveAccount(+1);
            return true;
        default:
            break;
        }
    }
    if (key->key() == Qt::Key_Escape) {
        cancel();
        return true;
    }
    // List-only keys.
    if (watched == m_list) {
        if (key->key() == Qt::Key_Delete) {
            removeAccount();
            return true;
        }
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            makeActive();
            return true;
        }
    }
    return QDialog::eventFilter(watched, event);
}

void AccountsDialog::keyPressEvent(QKeyEvent *event)
{
    // Focus on the dialog itself (e.g. empty list): route through the same handler.
    if (eventFilter(this, event))
        return;
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
        return; // QDialog would otherwise trigger a default button we don't have
    QDialog::keyPressEvent(event);
}

void AccountsDialog::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && windowHandle())
        windowHandle()->startSystemMove();
    QDialog::mousePressEvent(event);
}

} // namespace leansip
