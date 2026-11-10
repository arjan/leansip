#include "core/baresip_import.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

using namespace leansip;

namespace {

void writeFile(const QString &path, const QByteArray &text)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(text);
}

// What a fresh baresip writes, trimmed to the keys the tests use.
const char *kBaresipDefaults = R"(# baresip config
jitter_buffer_type	fixed		# off, fixed, adaptive
jitter_buffer_delay	5-10		# frames
opus_bitrate		28000 # 6000-510000
module			stdio.so
module			g711.so
module			alsa.so
)";

} // namespace

class TestBaresipImport : public QObject {
    Q_OBJECT
private slots:
    void parseConfigDropsCommentsAndCollapsesSpace()
    {
        const auto c = baresip_import::parseConfig("# x\n\n  audio_player\talsa,default   # trailing\n"
                                                   "module a.so\nmodule  b.so\nlonely\n");
        QCOMPARE(c.value("audio_player"), QStringList{"alsa,default"});
        QCOMPARE(c.value("module"), (QStringList{"a.so", "b.so"}));
        QVERIFY(!c.contains("lonely"));
        QVERIFY(!c.contains("#"));
    }

    void keepsOnlyTheUsersOwnChoices()
    {
        QTemporaryDir home;
        writeFile(home.filePath(".baresip/accounts"),
                  "# comment\n\"Alice\" <sip:alice@pbx.example:5060;transport=udp>;auth_user=alice;auth_pass=s3cret;"
                  "outbound=\"sip:proxy.example\";regint=3600;answermode=manual;audio_codecs=G722/16000/1;"
                  "regint2=60\n");
        writeFile(home.filePath(".baresip/config"),
                  "jitter_buffer_type fixed\n"     // baresip default: out
                  "jitter_buffer_delay 5-15\n"     // leansip template: out
                  "opus_bitrate 32000\n"           // own choice: in
                  "module stdio.so\nmodule g711.so\nmodule alsa.so\nmodule aufile.so\n" // = default once stdio/aufile go
                  "module_path /usr/lib/baresip/modules\n" // managed: out
                  "video_size 640x480\n"           // video: out
                  "auplay_srate 16000\n");         // own choice: in
        const auto plan = baresip_import::discover(home.path(), kBaresipDefaults);
        QVERIFY2(plan.skipped.isEmpty(), qPrintable(plan.skipped.join('\n')));
        QCOMPARE(plan.profiles.size(), 1);
        const settings::Profile &p = plan.profiles.first().profile;
        QCOMPARE(p.name, "baresip");
        QCOMPARE(plan.defaultProfile, "baresip");
        QCOMPARE(p.displayName, "Alice");
        QCOMPARE(p.aor, "sip:alice@pbx.example:5060");
        QCOMPARE(p.transport, "udp");
        QVERIFY(p.authUser.isEmpty()); // same as the user: baresip's default
        QCOMPARE(p.password, "s3cret");
        QCOMPARE(p.outbound, "sip:proxy.example");
        QCOMPARE(p.accountParams, (QMap<QString, QString>{{"audio_codecs", "G722/16000/1"}, {"regint2", "60"}}));
        QCOMPARE(p.baresip, (settings::BaresipSettings{{"opus_bitrate", {"32000"}}, {"auplay_srate", {"16000"}}}));
    }

    void moduleListThatDiffersIsKeptWithoutHandledModules()
    {
        QTemporaryDir home;
        writeFile(home.filePath(".baresip/accounts"), "<sip:a@b>;auth_pass=x\n");
        writeFile(home.filePath(".baresip/config"), "module stdio.so\nmodule g711.so\nmodule event.so\n");
        const auto plan = baresip_import::discover(home.path(), kBaresipDefaults);
        QCOMPARE(plan.profiles.size(), 1);
        QCOMPARE(plan.profiles.first().profile.baresip.value("module"), (QStringList{"g711.so", "event.so"}));
    }

    void namesProfilesAfterTheirDirectories()
    {
        QTemporaryDir home;
        writeFile(home.filePath(".baresip-work/accounts"), "<sip:w@x>;auth_pass=1\n<sip:w2@x>;auth_pass=2\n");
        writeFile(home.filePath(".baresip2/accounts"), "<sip:b@x>;auth_pass=3\nnot an account\n");
        writeFile(home.filePath(".baresip-empty/accounts"), "# only comments\n");
        writeFile(home.filePath(".baresipnoaccounts/config"), "module g711.so\n");
        writeFile(home.filePath(".other/accounts"), "<sip:o@x>\n");
        const auto plan = baresip_import::discover(home.path(), {});
        QStringList names;
        for (const auto &ip : plan.profiles)
            names << ip.profile.name;
        QCOMPARE(names, (QStringList{"baresip-work", "baresip-work-2", "baresip2"}));
        QCOMPARE(plan.defaultProfile, "baresip-work"); // no plain ~/.baresip: the first one
        QCOMPARE(plan.skipped.size(), 2);
        QVERIFY(plan.skipped.at(0).contains("~/.baresip-empty") || plan.skipped.at(0).contains(".baresip-empty"));
        QVERIFY(plan.skipped.at(1).contains("line 2"));
    }

    void tomlParsesBackToThePlan()
    {
        QTemporaryDir home;
        writeFile(home.filePath(".baresip/accounts"),
                  "\"Bob B\" <sips:bob@x.example;transport=tls>;auth_user=b0b;auth_pass=p\\w;audio_codecs=opus/48000/2\n");
        writeFile(home.filePath(".baresip/config"), "module g711.so\nmodule opus.so\nopus_bitrate 1000\n");
        const auto plan = baresip_import::discover(home.path(), kBaresipDefaults);
        QCOMPARE(plan.profiles.size(), 1);
        QString error;
        const auto file = settings::parse(baresip_import::toToml(plan), &error);
        QVERIFY2(file, qPrintable(error));
        QCOMPARE(file->defaultProfile, "baresip");
        const settings::Profile &want = plan.profiles.first().profile;
        const settings::Profile got = file->profiles.value("baresip");
        QCOMPARE(got.aor, want.aor);
        QCOMPARE(got.transport, "tls");
        QCOMPARE(got.authUser, "b0b");
        QCOMPARE(got.password, "p\\w");
        QCOMPARE(got.accountParams, want.accountParams);
        QCOMPARE(got.baresip, want.baresip);
    }

    // leansip refuses ; in an outbound proxy: the account is reported, not half-imported.
    void accountLeansipCannotExpressIsSkippedWithTheReason()
    {
        QTemporaryDir home;
        writeFile(home.filePath(".baresip/accounts"), "<sip:a@b>;auth_pass=x;outbound=\"sip:p.example;lr\"\n");
        const auto plan = baresip_import::discover(home.path(), {});
        QVERIFY(plan.profiles.isEmpty());
        QCOMPARE(plan.skipped.size(), 1);
        QVERIFY2(plan.skipped.first().contains("line 1") && plan.skipped.first().contains("outbound"),
                 qPrintable(plan.skipped.first()));
    }

    void summaryNeverShowsThePassword()
    {
        QTemporaryDir home;
        writeFile(home.filePath(".baresip/accounts"), "<sip:a@b>;auth_pass=TopSecret123\n");
        const auto plan = baresip_import::discover(home.path(), {});
        const QString s = baresip_import::summary(plan);
        QVERIFY(s.contains("sip:a@b"));
        QVERIFY(!s.contains("TopSecret123"));
    }

    void writeMakesAPrivateNewFileOnly()
    {
        QTemporaryDir home;
        writeFile(home.filePath(".baresip/accounts"), "<sip:a@b>;auth_pass=x\n");
        const QString toml = baresip_import::toToml(baresip_import::discover(home.path(), {}));
        const QString path = home.filePath("cfg/leansip/leansip.toml");
        QString error;
        QVERIFY2(baresip_import::write(path, toml, &error), qPrintable(error));
        QCOMPARE(QFileInfo(path).permissions() & 0x0077, QFileDevice::Permissions{});
        QCOMPARE(QFileInfo(QFileInfo(path).absolutePath()).permissions() & 0x0077, QFileDevice::Permissions{});
        QString loadError;
        QVERIFY2(settings::load(path, &loadError), qPrintable(loadError));

        QVERIFY(!baresip_import::write(path, toml, &error)); // never replaces a file
        QVERIFY(error.contains("already exists"));
        QVERIFY(!baresip_import::write(home.filePath("other.toml"), "not = [toml", &error));
        QVERIFY(!QFileInfo::exists(home.filePath("other.toml")));
    }
};

QTEST_GUILESS_MAIN(TestBaresipImport)
#include "tst_baresip_import.moc"
