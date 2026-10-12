#include "core/config_deriver.h"

#include <QtTest>

using namespace leansip;

class TestConfigDeriver : public QObject {
    Q_OBJECT
private slots:
    void dropsConsoleUiAndExistingCtrlLines()
    {
        const QString input = QStringLiteral(
            "poll_method\t\tepoll\n"
            "module\t\t\tstdio.so\n"
            "#module\t\t\tcons.so\n"
            "module\t\t\tcons.so\n"
            "module\t\t\topus.so\n"
            "module_app\t\taccount.so\n"
            "module_app\t\tgtk.so\n"
            "module_app\t\tmenu.so\n"
            "module_app\t\tctrl_tcp.so\n"
            "ctrl_tcp_listen\t\t0.0.0.0:4444 # TCP interface JSON\n");
        const QString out = config_deriver::deriveConfigText(input, QStringLiteral("127.0.0.1:41234"));
        const QStringList lines = out.split('\n');

        QVERIFY(lines.contains("poll_method\t\tepoll"));
        QVERIFY(lines.contains("module\t\t\topus.so"));
        QVERIFY(!lines.contains("module\t\t\tstdio.so"));
        QVERIFY(!lines.contains("module\t\t\tcons.so"));
        QVERIFY(!lines.contains("module_app\t\tgtk.so"));
        QVERIFY(lines.contains("#module\t\t\tcons.so")); // comments are left alone
        QVERIFY(!out.contains("0.0.0.0:4444"));
        QCOMPARE(out.count("ctrl_tcp.so"), 1);
        QCOMPARE(out.count("ctrl_tcp_listen"), 1);
        QVERIFY(lines.contains("ctrl_tcp_listen\t\t127.0.0.1:41234"));
        // account/menu already active: not duplicated.
        QCOMPARE(out.count("account.so"), 1);
        QCOMPARE(out.count("menu.so"), 1);
    }

    void addsRequiredAppModulesWhenMissing()
    {
        const QString out = config_deriver::deriveConfigText(QStringLiteral("#module_app\t\tmenu.so\n"),
                                                             QStringLiteral("127.0.0.1:1"));
        QVERIFY(out.contains("\nmodule_app\t\taccount.so\n"));
        QVERIFY(out.contains("\nmodule_app\t\tmenu.so\n"));
        QVERIFY(out.contains("\nmodule_app\t\tctrl_tcp.so\n"));
        QVERIFY(out.endsWith('\n'));
    }

    void emptyConfigStillYieldsControlModule()
    {
        const QString out = config_deriver::deriveConfigText(QString(), QStringLiteral("127.0.0.1:2"));
        QVERIFY(out.contains("ctrl_tcp_listen\t\t127.0.0.1:2"));
    }

    void deriveDirectoryWritesConfigAndLinks()
    {
        QTemporaryDir user;
        QTemporaryDir target;
        QVERIFY(user.isValid() && target.isValid());
        writeFile(user.filePath("config"), "module\t\t\tstdio.so\nmodule_app\t\tmenu.so\n");
        writeFile(user.filePath("accounts"), "<sip:a@b>;auth_pass=secret\n");
        writeFile(user.filePath("uuid"), "1234\n");

        const QString dir = target.filePath("derived");
        const auto r = config_deriver::deriveDirectory(user.path(), dir, QStringLiteral("127.0.0.1:5"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.dir, dir);

        QFile cfg(dir + "/config");
        QVERIFY(cfg.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(cfg.readAll());
        QVERIFY(!text.contains("stdio.so"));
        QVERIFY(text.contains("127.0.0.1:5"));

        QFileInfo accounts(dir + "/accounts");
        QVERIFY(accounts.isSymLink());
        QCOMPARE(accounts.symLinkTarget(), QFileInfo(user.filePath("accounts")).canonicalFilePath());
        QVERIFY(QFileInfo(dir + "/uuid").isSymLink());
        QVERIFY(!QFileInfo::exists(dir + "/contacts")); // absent in source: not linked

        const auto perms = QFileInfo(dir).permissions();
        QVERIFY(!(perms & QFileDevice::ReadGroup));
        QVERIFY(!(perms & QFileDevice::ReadOther));

        // Re-deriving must replace the existing links rather than fail.
        const auto again = config_deriver::deriveDirectory(user.path(), dir, QStringLiteral("127.0.0.1:6"));
        QVERIFY2(again.ok, qPrintable(again.error));
    }

    void deriveDirectoryFailsWithoutUserConfig()
    {
        QTemporaryDir user;
        QTemporaryDir target;
        const auto r = config_deriver::deriveDirectory(user.path(), target.path(), QStringLiteral("127.0.0.1:7"));
        QVERIFY(!r.ok);
        QVERIFY(r.error.contains("run baresip once"));
    }

    void deriveDirectoryFailsWithoutUserDir()
    {
        QTemporaryDir target;
        const auto r = config_deriver::deriveDirectory("/nonexistent/leansip", target.path(), QStringLiteral("127.0.0.1:8"));
        QVERIFY(!r.ok);
    }

private:
    static void writeFile(const QString &path, const QByteArray &data)
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(data);
    }
};

QTEST_APPLESS_MAIN(TestConfigDeriver)
#include "tst_config_deriver.moc"
