#pragma once

#include "core/account_validator.h"
#include "core/accounts_parser.h"

#include <QComboBox>
#include <QDialog>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>

namespace leansip {

// Keyboard-driven account editor. The list order is the file order; the first entry is the
// account the dialer uses. Save hands the working copy to the validator, which registers it
// for real; the dialog closes only when that succeeds.
class AccountsDialog : public QDialog {
    Q_OBJECT
public:
    AccountsDialog(AccountsFile initial, AccountValidator *validator, QWidget *parent = nullptr);

    AccountsFile working() const { return m_working; }
    // A restart would drop a live call, so saving is refused while one is active.
    void setCallActive(bool active);

    // Accessors for tests.
    QListWidget *list() const { return m_list; }
    QLineEdit *displayNameEdit() const { return m_displayName; }
    QLineEdit *userEdit() const { return m_user; }
    QLineEdit *domainEdit() const { return m_domain; }
    QComboBox *transportBox() const { return m_transport; }
    QLineEdit *authUserEdit() const { return m_authUser; }
    QLineEdit *passwordEdit() const { return m_password; }
    QLineEdit *outboundEdit() const { return m_outbound; }
    QLabel *statusLabel() const { return m_status; }
    bool canSave() const;

signals:
    // Emitted once the validator confirmed registration and the user's file was written.
    void saved(const leansip::AccountsFile &file);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    void buildUi();
    void refreshList();
    void loadFields(int index);
    void storeFields();
    void selectRow(int index);
    void addAccount();
    void removeAccount();
    void moveAccount(int delta);
    void makeActive();
    void save();
    void cancel();
    void togglePasswordEcho();
    void setFormEnabled(bool enabled);
    void showStatus(const QString &text);
    QString rowText(int index) const;

    AccountsFile m_working;
    AccountValidator *m_validator;
    QHash<QString, bool> m_results; // aor -> last registration result
    bool m_callActive = false;
    bool m_loading = false;

    QListWidget *m_list = nullptr;
    QLineEdit *m_displayName = nullptr;
    QLineEdit *m_user = nullptr;
    QLineEdit *m_domain = nullptr;
    QComboBox *m_transport = nullptr;
    QLineEdit *m_authUser = nullptr;
    QLineEdit *m_password = nullptr;
    QLineEdit *m_outbound = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_hint = nullptr;
    QWidget *m_form = nullptr;
};

} // namespace leansip
