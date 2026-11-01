#include "core/settings.h"

#include <QtTest>

using namespace leansip;

class TestSettings : public QObject {
    Q_OBJECT
private slots:
    void init()
    {
        qunsetenv("LEANSIP_PROFILE");
        qunsetenv("LEANSIP_CONFIG");
    }

    void parsesProfiles()
    {
        QString error;
        const auto f = settings::parse(QStringLiteral(R"(
default_profile = "work"

[baresip]
audio_player = "pulse,"
jitter_buffer_delay = "5-15"

[profiles.work]
display_name = "Work Phone"
aor = "sip:phone1@pbx.example.com:5060"
transport = "UDP"
auth_user = "p1"
password = "s3cret"
outbound = "sip:127.0.0.1:5060"
uuid = "a1b2c3d4-0000-4000-8000-000000000001"
confirm_dial = true
record = true
record_dir = "~/calls"
account_params = { audio_codecs = "opus/48000/2,PCMA/8000/1", regint = 0 }

[profiles.work.baresip]
jitter_buffer_delay = "5-10"
module_app = ["account.so", "menu.so", "netroam.so"]
opus_inbandfec = false

[profiles.lab]
aor = "sip:lab@127.0.0.1"
)"), &error);
        QVERIFY2(f, qPrintable(error));
        QCOMPARE(f->defaultProfile, "work");
        QCOMPARE(f->profiles.size(), 2);

        const settings::Profile &w = f->profiles.value("work");
        QCOMPARE(w.name, "work");
        QCOMPARE(w.displayName, "Work Phone");
        QCOMPARE(w.transport, "udp");
        QCOMPARE(w.password, "s3cret");
        QVERIFY(w.confirmDial);
        QVERIFY(w.record);
        QCOMPARE(w.recordDir, QDir::home().filePath("calls"));
        // The profile's own baresip settings win over the shared ones, key by key.
        QCOMPARE(w.baresip.value("audio_player"), QStringList{"pulse,"});
        QCOMPARE(w.baresip.value("jitter_buffer_delay"), QStringList{"5-10"});
        QCOMPARE(w.baresip.value("module_app"), (QStringList{"account.so", "menu.so", "netroam.so"}));
        QCOMPARE(w.baresip.value("opus_inbandfec"), QStringList{"no"});

        const AccountInfo info = w.account().info();
        QCOMPARE(info.aor, "sip:phone1@pbx.example.com:5060");
        QCOMPARE(info.label(), "Work Phone");
        QCOMPARE(info.authUser, "p1");

        const settings::Profile &lab = f->profiles.value("lab");
        QVERIFY(!lab.confirmDial);
        QCOMPARE(lab.baresip.value("jitter_buffer_delay"), QStringList{"5-15"});
    }

    void accountLineCarriesEverything()
    {
        QString error;
        const auto f = settings::parse(QStringLiteral(R"(
[profiles.p]
display_name = "Desk"
aor = "sip:phone1@voiceworks.com"
transport = "udp"
auth_user = "phone1"
outbound = "sip:127.0.0.1:5060"
account_params = { audio_codecs = "G722/16000/1", regint = 0 }
)"), &error);
        QVERIFY2(f, qPrintable(error));
        const auto line = settings::accountLine(f->profiles.value("p"), "pw", &error);
        QVERIFY2(line, qPrintable(error));
        QCOMPARE(*line, "\"Desk\" <sip:phone1@voiceworks.com;transport=udp>;auth_user=phone1;auth_pass=pw;"
                        "outbound=sip:127.0.0.1:5060;audio_codecs=G722/16000/1;regint=0");

        // A password that would break the line is refused, whatever produced it.
        QVERIFY(!settings::accountLine(f->profiles.value("p"), "a;b", &error));
        QVERIFY(error.contains("password"));
        QVERIFY(!error.contains("a;b"));
    }

    void labelAndShortcut()
    {
        QString error;
        const auto f = settings::parse(QStringLiteral(R"(
[profiles.desk]
display_name = "Arjan"
label = "Desk"
shortcut = 1
aor = "sip:phone1@pbx.example.com"

[profiles.lab]
aor = "sip:lab@pbx.example.com"
shortcut = 0

[profiles.plain]
aor = "sip:plain@pbx.example.com"
)"), &error);
        QVERIFY2(f, qPrintable(error));
        const settings::Profile &desk = f->profiles.value("desk");
        QCOMPARE(desk.label, "Desk");
        QCOMPARE(desk.shortcut, 1);
        // The label is the dialer's name for it; the SIP display name is unchanged.
        QCOMPARE(desk.info().label(), "Desk");
        QCOMPARE(desk.info().displayName, "Arjan");
        QCOMPARE(desk.account().info().label(), "Arjan");
        QVERIFY(!settings::accountLine(desk, "pw", &error)->contains("Desk"));
        QCOMPARE(f->profiles.value("lab").info().label(), "sip:lab@pbx.example.com");
        QCOMPARE(f->profiles.value("plain").shortcut, -1);

        QCOMPARE(settings::byShortcut(*f, 1)->name, "desk");
        QCOMPARE(settings::byShortcut(*f, 0)->name, "lab");
        QVERIFY(!settings::byShortcut(*f, 2));
        QVERIFY(!settings::byShortcut(*f, -1)); // "none" is not a shortcut
    }

    void rejectsMistakes_data()
    {
        QTest::addColumn<QString>("toml");
        QTest::addColumn<QString>("expected");
        const QString aor = QStringLiteral("aor = \"sip:a@b\"\n");
        QTest::newRow("syntax") << "[profiles.x" << "line 1";
        QTest::newRow("no profiles") << "default_profile = \"x\"" << "no profiles";
        QTest::newRow("unknown top key") << "colour = 1\n[profiles.x]\n" + aor << "colour: unknown key";
        QTest::newRow("typo in profile") << "[profiles.x]\n" + aor + "pasword = \"y\"" << "profiles.x.pasword: unknown key";
        QTest::newRow("missing aor") << "[profiles.x]\npassword = \"y\"" << "aor is required";
        QTest::newRow("bad aor") << "[profiles.x]\naor = \"alice\"" << "profiles.x.aor";
        QTest::newRow("aor with params") << "[profiles.x]\naor = \"sip:a@b;transport=tcp\"" << "profiles.x.aor";
        QTest::newRow("both passwords") << "[profiles.x]\n" + aor + "password = \"a\"\npassword_cmd = \"b\"" << "not both";
        QTest::newRow("bad transport") << "[profiles.x]\n" + aor + "transport = \"sctp\"" << "transport";
        QTest::newRow("wrong type") << "[profiles.x]\n" + aor + "confirm_dial = \"yes\"" << "expected true or false";
        QTest::newRow("bad name") << "[profiles.\"a/b\"]\n" + aor << "profile names";
        QTest::newRow("unknown default") << "default_profile = \"y\"\n[profiles.x]\n" + aor << "no profile named 'y'";
        QTest::newRow("newline in setting") << "[profiles.x]\n" + aor + "[profiles.x.baresip]\nsip_listen = \"a\\nmodule evil.so\""
                                            << "single line";
        QTest::newRow("bad setting name") << "[profiles.x]\n" + aor + "[profiles.x.baresip]\n\"a b\" = 1" << "setting name";
        QTest::newRow("nested setting") << "[profiles.x]\n" + aor + "[profiles.x.baresip.deep]\na = 1" << "expected a string";
        QTest::newRow("param injection") << "[profiles.x]\n" + aor + "account_params = { mwi = \"no;auth_pass=x\" }" << "must not contain";
        QTest::newRow("owned param") << "[profiles.x]\n" + aor + "account_params = { auth_pass = \"x\" }" << "not in account_params";
        QTest::newRow("relative record_dir") << "[profiles.x]\n" + aor + "record_dir = \"calls\"" << "absolute";
        QTest::newRow("bad uuid") << "[profiles.x]\n" + aor + "uuid = \"not a uuid\"" << "UUID";
        QTest::newRow("shortcut too big") << "[profiles.x]\n" + aor + "shortcut = 10" << "profiles.x.shortcut: expected a digit";
        QTest::newRow("shortcut negative") << "[profiles.x]\n" + aor + "shortcut = -1" << "expected a digit";
        QTest::newRow("shortcut string") << "[profiles.x]\n" + aor + "shortcut = \"1\"" << "expected a digit";
        QTest::newRow("shortcut twice") << "[profiles.x]\n" + aor + "shortcut = 1\n[profiles.y]\n" + aor + "shortcut = 1"
                                        << "already the shortcut of profile x";
        QTest::newRow("label newline") << "[profiles.x]\n" + aor + "label = \"a\\nb\"" << "single line";
    }

    void rejectsMistakes()
    {
        QFETCH(QString, toml);
        QFETCH(QString, expected);
        QString error;
        QVERIFY(!settings::parse(toml, &error));
        QVERIFY2(error.contains(expected), qPrintable(error));
    }

    void selectPicksTheRightProfile()
    {
        QString error;
        auto f = settings::parse("default_profile = \"b\"\n[profiles.a]\naor = \"sip:a@x\"\n[profiles.b]\naor = \"sip:b@x\"\n", &error);
        QVERIFY(f);
        f->path = "/tmp/leansip.toml";
        QCOMPARE(settings::select(*f, {}, &error)->name, "b");
        QCOMPARE(settings::select(*f, "a", &error)->name, "a");
        qputenv("LEANSIP_PROFILE", "a");
        QCOMPARE(settings::select(*f, {}, &error)->name, "a");
        QCOMPARE(settings::select(*f, "b", &error)->name, "b"); // explicit wins over the environment
        qunsetenv("LEANSIP_PROFILE");

        QVERIFY(!settings::select(*f, "c", &error));
        QVERIFY(error.contains("have: a, b"));

        f->defaultProfile.clear();
        QVERIFY(!settings::select(*f, {}, &error));
        QVERIFY(error.contains("--profile"));
        f->profiles.remove("a");
        QCOMPARE(settings::select(*f, {}, &error)->name, "b"); // the only one
    }

    void loadInsistsOnAPrivateFile()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("leansip.toml");
        QString error;
        QVERIFY(!settings::load(path, &error));
        QVERIFY(error.contains("no config file"));

        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("[profiles.x]\naor = \"sip:a@b\"\npassword = \"pw\"\n");
        f.close();
        QVERIFY(f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadGroup));
        QVERIFY(!settings::load(path, &error));
        QVERIFY2(error.contains("chmod 600"), qPrintable(error));
        QVERIFY(error.contains("0640"));

        QVERIFY(f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner));
        const auto loaded = settings::load(path, &error);
        QVERIFY2(loaded, qPrintable(error));
        QCOMPARE(loaded->path, path);
        QCOMPARE(loaded->profiles.value("x").password, "pw");
    }

    void defaultPathFollowsEnvironment()
    {
        QVERIFY(settings::defaultPath().endsWith("/leansip/leansip.toml"));
        qputenv("LEANSIP_CONFIG", "/etc/x.toml");
        QCOMPARE(settings::defaultPath(), "/etc/x.toml");
    }

    void passwordFromCommand()
    {
        settings::Profile p;
        p.name = "x";
        QString error;
        QCOMPARE(*settings::password(p, &error), QString()); // no password at all is allowed
        p.password = "literal";
        QCOMPARE(*settings::password(p, &error), "literal");

        p.password.clear();
        p.passwordCmd = "printf 'from-cmd\\nsecond line\\n'";
        QCOMPARE(*settings::password(p, &error), "from-cmd");

        p.passwordCmd = "echo leaked; exit 3";
        QVERIFY(!settings::password(p, &error));
        QVERIFY(error.contains("exit code 3"));
        QVERIFY(!error.contains("leaked")); // its output never ends up in messages

        p.passwordCmd = "true";
        QVERIFY(!settings::password(p, &error));
        QVERIFY(error.contains("printed nothing"));

        p.passwordCmd = "sleep 5";
        QVERIFY(!settings::password(p, &error, 200));
        QVERIFY(error.contains("did not finish"));
    }

    void baresipConfigAppliesOverrides()
    {
        const QString base = settings::baresipConfig({});
        QCOMPARE(base, settings::baresipTemplate());
        QVERIFY(base.contains("\nmodule\t\t\tg722.so\n"));

        const QString out = settings::baresipConfig({
            {"jitter_buffer_delay", {"5-10"}},
            {"module", {"g711.so", "aufile.so"}},
            {"opus_bitrate", {}},
            {"sip_listen", {"127.0.0.1:5070"}},
        });
        const QStringList lines = out.split('\n');
        QVERIFY(lines.contains("jitter_buffer_delay\t\t5-10"));
        QVERIFY(!out.contains("5-15"));
        // Every module line is replaced, in the place of the first one.
        QVERIFY(!out.contains("g722.so"));
        QCOMPARE(out.count("\nmodule\t\t"), 2);
        QVERIFY(lines.indexOf("module\t\taufile.so") == lines.indexOf("module\t\tg711.so") + 1);
        QVERIFY(lines.indexOf("module\t\tg711.so") > lines.indexOf("module_path\t\t/usr/lib/baresip/modules"));
        QVERIFY(out.contains("module_app\t\tmenu.so")); // a different key
        QVERIFY(!out.contains("opus_bitrate")); // an empty list removes the key
        QVERIFY(lines.contains("sip_listen\t\t127.0.0.1:5070")); // new keys are appended
        QVERIFY(out.endsWith('\n'));
    }
};

QTEST_GUILESS_MAIN(TestSettings)
#include "tst_settings.moc"
