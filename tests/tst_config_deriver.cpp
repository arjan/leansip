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
            "audio_path\t\t/home/me/sounds\n"
            "module_app\t\taccount.so\n"
            "module_app\t\tgtk.so\n"
            "module_app\t\tmenu.so\n"
            "module_app\t\tdebug_cmd.so\n"
            "module_app\t\tctrl_tcp.so\n"
            "ctrl_tcp_listen\t\t0.0.0.0:4444 # TCP interface JSON\n");
        const QString out = config_deriver::deriveConfigText(input, QStringLiteral("127.0.0.1:41234"), QStringLiteral("/run/x/sounds"));
        const QStringList lines = out.split('\n');

        QVERIFY(lines.contains("poll_method\t\tepoll"));
        QVERIFY(lines.contains("module\t\t\topus.so"));
        QVERIFY(!lines.contains("module\t\t\tstdio.so"));
        QVERIFY(!lines.contains("module\t\t\tcons.so"));
        QVERIFY(!lines.contains("module_app\t\tgtk.so"));
        QVERIFY(lines.contains("#module\t\t\tcons.so")); // comments are left alone
        QVERIFY(!out.contains("0.0.0.0:4444"));
        QVERIFY(!out.contains("/home/me/sounds"));
        QCOMPARE(out.count("ctrl_tcp.so"), 1);
        QCOMPARE(out.count("ctrl_tcp_listen"), 1);
        QCOMPARE(out.count("audio_path"), 1);
        QVERIFY(lines.contains("ctrl_tcp_listen\t\t127.0.0.1:41234"));
        QVERIFY(lines.contains("audio_path\t\t/run/x/sounds"));
        // account/menu/debug_cmd already active: not duplicated.
        QCOMPARE(out.count("account.so"), 1);
        QCOMPARE(out.count("menu.so"), 1);
        QCOMPARE(out.count("debug_cmd.so"), 1);
    }

    void addsRequiredAppModulesWhenMissing()
    {
        const QString out = config_deriver::deriveConfigText(QStringLiteral("#module_app\t\tmenu.so\n"),
                                                             QStringLiteral("127.0.0.1:1"), QStringLiteral("/s"));
        QVERIFY(out.contains("\nmodule_app\t\taccount.so\n"));
        QVERIFY(out.contains("\nmodule_app\t\tmenu.so\n"));
        QVERIFY(out.contains("\nmodule_app\t\tdebug_cmd.so\n"));
        QVERIFY(out.contains("\nmodule_app\t\tctrl_tcp.so\n"));
        QVERIFY(out.endsWith('\n'));
    }

    void emptyConfigStillYieldsControlModule()
    {
        const QString out = config_deriver::deriveConfigText(QString(), QStringLiteral("127.0.0.1:2"), QStringLiteral("/s"));
        QVERIFY(out.contains("ctrl_tcp_listen\t\t127.0.0.1:2"));
        QVERIFY(out.contains("audio_path\t\t/s"));
    }

    void userAudioPathDefaultsAndOverrides()
    {
        QCOMPARE(config_deriver::userAudioPath("#audio_path\t\t/usr/share/baresip\n"), "/usr/share/baresip");
        QCOMPARE(config_deriver::userAudioPath(QString()), "/usr/share/baresip");
        QCOMPARE(config_deriver::userAudioPath("x 1\naudio_path   /opt/snd  # mine\n"), "/opt/snd");
    }

    void deriveDirectoryWritesConfigLinksAndSounds()
    {
        QTemporaryDir user;
        QTemporaryDir target;
        QTemporaryDir stock;
        QVERIFY(user.isValid() && target.isValid() && stock.isValid());
        writeFile(stock.filePath("ring.wav"), "RIFF");
        writeFile(stock.filePath("notes.txt"), "not a sound");
        writeFile(user.filePath("config"), QStringLiteral("module\t\t\tstdio.so\nmodule_app\t\tmenu.so\naudio_path\t\t%1\n").arg(stock.path()).toUtf8());
        writeFile(user.filePath("accounts"), "<sip:a@b>;auth_pass=secret\n");
        writeFile(user.filePath("uuid"), "1234\n");

        const QString dir = target.filePath("derived");
        const auto r = config_deriver::deriveDirectory(user.path(), dir, QStringLiteral("127.0.0.1:5"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.dir, dir);
        QCOMPARE(r.soundsDir, dir + "/sounds");

        QFile cfg(dir + "/config");
        QVERIFY(cfg.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(cfg.readAll());
        QVERIFY(!text.contains("stdio.so"));
        QVERIFY(text.contains("127.0.0.1:5"));
        QVERIFY(text.contains("audio_path\t\t" + dir + "/sounds"));

        QFileInfo accounts(dir + "/accounts");
        QVERIFY(accounts.isSymLink());
        QCOMPARE(accounts.symLinkTarget(), QFileInfo(user.filePath("accounts")).canonicalFilePath());
        QVERIFY(QFileInfo(dir + "/uuid").isSymLink());
        QVERIFY(!QFileInfo::exists(dir + "/contacts")); // absent in source: not linked

        // Stock sounds are linked into our audio_path; non-WAV files are not.
        QVERIFY(QFileInfo(dir + "/sounds/ring.wav").isSymLink());
        QVERIFY(!QFileInfo::exists(dir + "/sounds/notes.txt"));

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
