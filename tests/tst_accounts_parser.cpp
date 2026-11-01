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
};

QTEST_APPLESS_MAIN(TestAccountsParser)
#include "tst_accounts_parser.moc"
