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
