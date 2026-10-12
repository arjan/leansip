#include "core/accounts_parser.h"

#include <QtTest>

using namespace leansip;

class TestAccountsParser : public QObject {
    Q_OBJECT
private slots:
    void fullLineWithDisplayName()
    {
        const auto a = accounts::parseLine(
            R"("Alice Test" <sip:alice@example.com;transport=udp>;auth_user=alice_auth;auth_pass=hunter2;outbound=sip:1.2.3.4)");
        QVERIFY(a.has_value());
        QCOMPARE(a->displayName, "Alice Test");
        QCOMPARE(a->aor, "sip:alice@example.com");
        QCOMPARE(a->user, "alice");
        QCOMPARE(a->domain, "example.com");
        QCOMPARE(a->authUser, "alice_auth");
        QCOMPARE(a->label(), "Alice Test");
    }

    void angleBracketsWithoutDisplayName()
    {
        const auto a = accounts::parseLine("<sip:bob@pbx.local>;auth_pass=x");
        QVERIFY(a.has_value());
        QVERIFY(a->displayName.isEmpty());
        QCOMPARE(a->aor, "sip:bob@pbx.local");
        QCOMPARE(a->authUser, "bob"); // falls back to user
        QCOMPARE(a->label(), "sip:bob@pbx.local");
    }

    void bareUri()
    {
        const auto a = accounts::parseLine("sip:carol@host;auth_user=c");
        QVERIFY(a.has_value());
        QCOMPARE(a->user, "carol");
        QCOMPARE(a->domain, "host");
        QCOMPARE(a->authUser, "c");
    }

    void sipsScheme()
    {
        const auto a = accounts::parseLine("<sips:dave@secure.example>");
        QVERIFY(a.has_value());
        QCOMPARE(a->aor, "sips:dave@secure.example");
    }

    void rejectsCommentsBlankAndGarbage()
    {
        QVERIFY(!accounts::parseLine("# <sip:x@y>").has_value());
        QVERIFY(!accounts::parseLine("   ").has_value());
        QVERIFY(!accounts::parseLine("http://example.com").has_value());
        QVERIFY(!accounts::parseLine("<sip:nouser>").has_value());
        QVERIFY(!accounts::parseLine("<sip:@host>").has_value());
        QVERIFY(!accounts::parseLine("\"unterminated <sip:a@b>").has_value());
        QVERIFY(!accounts::parseLine("<sip:a@b").has_value());
    }

    void firstAccountWins()
    {
        const QString contents = QStringLiteral(
            "# comment\n"
            "\n"
            "<sip:first@a>;auth_pass=1\n"
            "<sip:second@b>;auth_pass=2\n");
        const auto a = accounts::parseFirst(contents);
        QVERIFY(a.has_value());
        QCOMPARE(a->user, "first");
    }

    void recordRoundTripPreservesUnknownParams()
    {
        const QString line = QStringLiteral(
            R"("Alice Test" <sip:alice@example.com;transport=tcp>;auth_user=alice_auth;auth_pass=hunter2;outbound=sip:1.2.3.4;audio_codecs=opus/48000/2,PCMU/8000/1;regint=600)");
        const auto r = AccountRecord::fromLine(line);
        QVERIFY(r.has_value());
        QCOMPARE(r->displayName, "Alice Test");
        QCOMPARE(r->user, "alice");
        QCOMPARE(r->domain, "example.com");
        QCOMPARE(r->transport, "tcp");
        QCOMPARE(r->authUser, "alice_auth");
        QCOMPARE(r->password, "hunter2");
        QCOMPARE(r->outbound, "sip:1.2.3.4");
        QCOMPARE(r->extraParams, ";audio_codecs=opus/48000/2,PCMU/8000/1;regint=600");
        QCOMPARE(r->aor(), "sip:alice@example.com");
        QCOMPARE(r->toLine(), line);
        QCOMPARE(r->info().label(), "Alice Test");

        // Editing a modelled field keeps everything else intact.
        AccountRecord edited = *r;
        edited.password = "new";
        QCOMPARE(edited.toLine(),
                 R"("Alice Test" <sip:alice@example.com;transport=tcp>;auth_user=alice_auth;auth_pass=new;outbound=sip:1.2.3.4;audio_codecs=opus/48000/2,PCMU/8000/1;regint=600)");
    }

    void recordMinimalLine()
    {
        AccountRecord r;
        r.user = "bob";
        r.domain = "pbx.local";
        QCOMPARE(r.toLine(), "<sip:bob@pbx.local>");
        const auto back = AccountRecord::fromLine(r.toLine());
        QVERIFY(back.has_value());
        QCOMPARE(*back, r);
        QCOMPARE(back->info().authUser, "bob");
    }

    void fileParseAndSerialize()
    {
        const QString contents = QStringLiteral(
            "# baresip accounts\n"
            "\n"
            "<sip:first@a>;auth_pass=1\n"
            "# a comment between accounts is dropped\n"
            "\"Second\" <sip:second@b>;auth_pass=2\n"
            "not an account line\n");
        const AccountsFile f = accounts::parseFile(contents);
        QCOMPARE(f.preamble, (QStringList{"# baresip accounts", ""}));
        QCOMPARE(f.accounts.size(), 2);
        QCOMPARE(f.accounts[0].user, "first");
        QCOMPARE(f.accounts[1].displayName, "Second");
        QCOMPARE(accounts::serialize(f),
                 "# baresip accounts\n\n<sip:first@a>;auth_pass=1\n\"Second\" <sip:second@b>;auth_pass=2\n");

        // Reordering is just list order.
        AccountsFile swapped = f;
        swapped.accounts.swapItemsAt(0, 1);
        QVERIFY(accounts::serialize(swapped).startsWith("# baresip accounts\n\n\"Second\""));

        QCOMPARE(accounts::parseFile(QString()), AccountsFile{});
    }

    void writeFileIsAtomicAndPrivate()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("sub/accounts");
        AccountsFile f;
        f.preamble = {"# header"};
        AccountRecord r;
        r.user = "u";
        r.domain = "d";
        r.password = "p";
        f.accounts.append(r);

        QString error;
        QVERIFY2(accounts::writeFile(path, f, &error), qPrintable(error));
        QFile written(path);
        QVERIFY(written.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(written.readAll()), "# header\n<sip:u@d>;auth_pass=p\n");
        const auto perms = QFileInfo(path).permissions();
        QVERIFY(perms & QFileDevice::ReadOwner);
        QVERIFY(!(perms & (QFileDevice::ReadGroup | QFileDevice::ReadOther | QFileDevice::WriteGroup | QFileDevice::WriteOther)));
        QVERIFY(QDir(dir.filePath("sub")).entryList(QDir::Files | QDir::Hidden).size() == 1); // no temp file left

        // Overwrite keeps working and reading back gives the same model.
        f.accounts[0].password = "q";
        QVERIFY(accounts::writeFile(path, f, &error));
        AccountsFile back;
        QVERIFY(accounts::readFile(path, &back, &error));
        QCOMPARE(back, f);

        AccountsFile missing;
        QVERIFY(accounts::readFile(dir.filePath("nope"), &missing, &error));
        QCOMPARE(missing, AccountsFile{});

        QVERIFY(!accounts::writeFile("/proc/leansip/accounts", f, &error));
        QVERIFY(!error.isEmpty());
    }

    void validateRecordRules()
    {
        AccountRecord r;
        r.user = "alice";
        r.domain = "example.com";
        QString why;
        QVERIFY(accounts::validateRecord(r, &why));

        auto rejects = [&](auto mutate, const char *expect) {
            AccountRecord bad = r;
            mutate(bad);
            QString e;
            const bool ok = accounts::validateRecord(bad, &e);
            QVERIFY2(!ok, expect);
            QVERIFY2(e.contains(QLatin1String(expect)), qPrintable(e));
        };
        rejects([](AccountRecord &b) { b.user.clear(); }, "user is required");
        rejects([](AccountRecord &b) { b.domain.clear(); }, "domain is required");
        rejects([](AccountRecord &b) { b.password = "a b"; }, "password must not");
        rejects([](AccountRecord &b) { b.user = "x;y"; }, "user must not");
        rejects([](AccountRecord &b) { b.domain = "a@b"; }, "@");
        rejects([](AccountRecord &b) { b.displayName = "say \"hi\""; }, "quotes");
        rejects([](AccountRecord &b) { b.transport = "sctp"; }, "transport");
        rejects([](AccountRecord &b) { b.outbound = "1.2.3.4"; }, "sip:");
        r.transport = "tls";
        r.outbound = "sip:proxy";
        QVERIFY(accounts::validateRecord(r, &why));
    }

    void loadFromFixture()
    {
        const QString dir = qEnvironmentVariable("LEANSIP_FIXTURES");
        QVERIFY(!dir.isEmpty());
        const auto r = accounts::loadFirst(dir + "/accounts.sample");
        QVERIFY2(r.account.has_value(), qPrintable(r.error));
        QCOMPARE(r.account->displayName, "Fixture Phone");
        QCOMPARE(r.account->aor, "sip:phone1@fixture.example");
    }

    void loadMissingFile()
    {
        const auto r = accounts::loadFirst("/nonexistent/leansip/accounts");
        QVERIFY(!r.account.has_value());
        QVERIFY(r.error.contains("cannot open"));
    }

    void loadFileWithoutAccounts()
    {
        QTemporaryFile f;
        QVERIFY(f.open());
        f.write("# only comments\n\n");
        f.close();
        const auto r = accounts::loadFirst(f.fileName());
        QVERIFY(!r.account.has_value());
        QVERIFY(r.error.contains("no valid account"));
    }
};

QTEST_APPLESS_MAIN(TestAccountsParser)
#include "tst_accounts_parser.moc"
