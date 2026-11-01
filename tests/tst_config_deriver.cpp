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

    void dropsAMissingCaFile()
    {
        const QString out = config_deriver::deriveConfigText(
            QStringLiteral("sip_cafile\t\t/nonexistent/ca.crt\nsip_tos\t\t160\n"), QStringLiteral("127.0.0.1:1"),
            QStringLiteral("/s"));
        QVERIFY(!out.contains("sip_cafile"));
        QVERIFY(out.contains("sip_tos"));
        QTemporaryFile ca;
        QVERIFY(ca.open());
        const QString kept = config_deriver::deriveConfigText(QStringLiteral("sip_cafile\t\t%1\n").arg(ca.fileName()),
                                                              QStringLiteral("127.0.0.1:1"), QStringLiteral("/s"));
        QVERIFY(kept.contains("sip_cafile\t\t" + ca.fileName()));
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

    void writeDirectoryWritesConfigAccountAndSounds()
    {
        QTemporaryDir target;
        QTemporaryDir stock;
        QVERIFY(target.isValid() && stock.isValid());
        writeFile(stock.filePath("ring.wav"), "RIFF");
        writeFile(stock.filePath("notes.txt"), "not a sound");
        config_deriver::Setup setup;
        setup.config = QStringLiteral("module\t\t\tstdio.so\nmodule_app\t\tmenu.so\naudio_path\t\t%1\n").arg(stock.path());
        setup.accountLine = QStringLiteral("<sip:a@b>;auth_pass=secret");
        setup.uuid = QStringLiteral("1234-abcd");

        const QString dir = target.filePath("derived");
        const auto r = config_deriver::writeDirectory(setup, dir, QStringLiteral("127.0.0.1:5"));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.dir, dir);
        QCOMPARE(r.soundsDir, dir + "/sounds");

        const QString text = readFile(dir + "/config");
        QVERIFY(!text.contains("stdio.so"));
        QVERIFY(text.contains("127.0.0.1:5"));
        QVERIFY(text.contains("audio_path\t\t" + dir + "/sounds"));
        QCOMPARE(readFile(dir + "/accounts"), "<sip:a@b>;auth_pass=secret\n");
        QCOMPARE(readFile(dir + "/uuid"), "1234-abcd\n");

        // The password is only ever readable by us.
        QCOMPARE(QFileInfo(dir + "/accounts").permissions() & 0x0077, QFileDevice::Permissions());
        const auto perms = QFileInfo(dir).permissions();
        QVERIFY(!(perms & QFileDevice::ReadGroup));
        QVERIFY(!(perms & QFileDevice::ReadOther));

        // Stock sounds are linked into our audio_path; non-WAV files are not.
        QVERIFY(QFileInfo(dir + "/sounds/ring.wav").isSymLink());
        QVERIFY(!QFileInfo::exists(dir + "/sounds/notes.txt"));

        // Writing again replaces everything, and never writes through a link that is in the way.
        QTemporaryDir elsewhere;
        const QString victim = elsewhere.filePath("victim");
        writeFile(victim, "keep me");
        QFile::remove(dir + "/accounts");
        QVERIFY(QFile::link(victim, dir + "/accounts"));
        setup.accountLine = QStringLiteral("<sip:c@d>");
        setup.uuid.clear();
        const auto again = config_deriver::writeDirectory(setup, dir, QStringLiteral("127.0.0.1:6"));
        QVERIFY2(again.ok, qPrintable(again.error));
        QVERIFY(!QFileInfo(dir + "/accounts").isSymLink());
        QCOMPARE(readFile(dir + "/accounts"), "<sip:c@d>\n");
        QCOMPARE(readFile(victim), "keep me");
        QVERIFY(readFile(dir + "/config").contains("127.0.0.1:6"));
    }

    void writeDirectoryFailsWhereItCannotWrite()
    {
        const auto r = config_deriver::writeDirectory({}, "/proc/leansip/x", QStringLiteral("127.0.0.1:8"));
        QVERIFY(!r.ok);
        QVERIFY(!r.error.isEmpty());
    }

private:
    static QString readFile(const QString &path)
    {
        QFile f(path);
        return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
    }

    static void writeFile(const QString &path, const QByteArray &data)
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(data);
    }
};

QTEST_APPLESS_MAIN(TestConfigDeriver)
#include "tst_config_deriver.moc"
