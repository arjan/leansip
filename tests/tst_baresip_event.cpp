#include "core/baresip_event.h"

#include <QJsonDocument>
#include <QtTest>

using namespace leansip;

class TestBaresipEvent : public QObject {
    Q_OBJECT
private slots:
    void parsesEvent()
    {
        const auto obj = QJsonDocument::fromJson(R"({"event":true,"class":"call","type":"CALL_INCOMING",
            "accountaor":"sip:me@h","direction":"incoming","peeruri":"sip:them@h",
            "peerdisplayname":"Them","param":"x"})").object();
        const auto ev = BaresipEvent::fromJson(obj);
        QVERIFY(ev.has_value());
        QCOMPARE(ev->klass, "call");
        QCOMPARE(ev->type, "CALL_INCOMING");
        QCOMPARE(ev->accountAor, "sip:me@h");
        QCOMPARE(ev->direction, "incoming");
        QCOMPARE(ev->peerUri, "sip:them@h");
        QCOMPARE(ev->peerDisplayName, "Them");
        QCOMPARE(ev->param, "x");
        QVERIFY(!BaresipResponse::fromJson(obj).has_value());
    }

    void parsesResponse()
    {
        const auto obj = QJsonDocument::fromJson(R"({"response":true,"ok":true,"data":"out","token":"7"})").object();
        const auto r = BaresipResponse::fromJson(obj);
        QVERIFY(r.has_value());
        QCOMPARE(r->token, "7");
        QVERIFY(r->ok);
        QCOMPARE(r->data, "out");
        QVERIFY(!BaresipEvent::fromJson(obj).has_value());
    }

    void rejectsEventWithoutType()
    {
        const auto obj = QJsonDocument::fromJson(R"({"event":true,"class":"call"})").object();
        QVERIFY(!BaresipEvent::fromJson(obj).has_value());
    }

    void rejectsUnrelatedObject()
    {
        const auto obj = QJsonDocument::fromJson(R"({"hello":"world"})").object();
        QVERIFY(!BaresipEvent::fromJson(obj).has_value());
        QVERIFY(!BaresipResponse::fromJson(obj).has_value());
    }
};

QTEST_APPLESS_MAIN(TestBaresipEvent)
#include "tst_baresip_event.moc"
