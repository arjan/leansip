#include "core/ctrl_proxy.h"
#include "fake_baresip_server.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace leansip;

// Upstream fake baresip <- BaresipClient <- CtrlProxy <- two BaresipClients over the Unix socket.
struct ProxyRig {
    QTemporaryDir dir;
    FakeBaresipServer server;
    BaresipClient upstream;
    CtrlProxy proxy{&upstream};
    BaresipClient a;
    BaresipClient b;

    QString socketPath() const { return dir.filePath("ctrl.sock"); }

    bool start(bool connectUpstream = true)
    {
        QString error;
        if (!dir.isValid() || !server.listen() || !proxy.listen(socketPath(), &error))
            return false;
        if (connectUpstream) {
            upstream.connectToHost("127.0.0.1", server.port());
            if (!QTest::qWaitFor([&] { return proxy.backendUp() && server.hasClient(); }, 2000))
                return false;
        }
        QSignalSpy ca(&a, &BaresipClient::connected);
        QSignalSpy cb(&b, &BaresipClient::connected);
        a.connectToServer(socketPath());
        b.connectToServer(socketPath());
        return (ca.size() > 0 || ca.wait(2000)) && (cb.size() > 0 || cb.wait(2000))
            && QTest::qWaitFor([&] { return proxy.clientCount() == 2; }, 2000);
    }
};

class TestCtrlProxy : public QObject {
    Q_OBJECT
private slots:
    void routesEachResponseToTheClientThatAsked()
    {
        ProxyRig rig;
        QVERIFY(rig.start());
        QSignalSpy ra(&rig.a, &BaresipClient::responseReceived);
        QSignalSpy rb(&rig.b, &BaresipClient::responseReceived);

        // Both clients use token "1": the proxy must not confuse them.
        const QString ta = rig.a.sendCommand("dial", "100");
        QTRY_COMPARE(rig.server.received.size(), 1);
        const QString tb = rig.b.sendCommand("dial", "200");
        QTRY_COMPARE(rig.server.received.size(), 2);
        QCOMPARE(ta, tb);
        const QString upA = rig.server.received.at(0).value("token").toString();
        const QString upB = rig.server.received.at(1).value("token").toString();
        QVERIFY(upA != upB);
        QCOMPARE(rig.server.received.at(1).value("params").toString(), "200");

        rig.server.sendResponse(upB, true, "for b");
        rig.server.sendResponse(upA, false, "for a");
        QTRY_COMPARE(ra.size(), 1);
        QTRY_COMPARE(rb.size(), 1);
        const auto respA = ra.first().first().value<BaresipResponse>();
        const auto respB = rb.first().first().value<BaresipResponse>();
        QCOMPARE(respA.token, ta);
        QCOMPARE(respA.data, "for a");
        QVERIFY(!respA.ok);
        QCOMPARE(respB.data, "for b");
        QVERIFY(respB.ok);
    }

    void broadcastsEventsWithSeqAndJournalsThem()
    {
        ProxyRig rig;
        QVERIFY(rig.start());
        QSignalSpy ea(&rig.a, &BaresipClient::rawEventReceived);
        QSignalSpy eb(&rig.b, &BaresipClient::rawEventReceived);
        const qint64 before = rig.proxy.lastSeq(); // BACKEND_UP already journalled

        rig.server.sendEvent("CALL_INCOMING", {{"peeruri", "sip:x@y"}, {"id", "abc"}});
        QTRY_COMPARE(ea.size(), 1);
        QTRY_COMPARE(eb.size(), 1);
        const QJsonObject ev = ea.first().first().toJsonObject();
        QCOMPARE(ev.value("type").toString(), "CALL_INCOMING");
        QCOMPARE(ev.value("id").toString(), "abc"); // fields BaresipEvent ignores survive
        QCOMPARE(ev.value("seq").toInteger(), before + 1);
        QVERIFY(ev.value("ts").toInteger() > 0);

        const QJsonArray journal = rig.proxy.eventsSince(before);
        QCOMPARE(journal.size(), 1);
        QCOMPARE(journal.first().toObject().value("type").toString(), "CALL_INCOMING");
        QCOMPARE(rig.proxy.eventsSince(0).size(), static_cast<qsizetype>(before + 1));
    }

    void eventsCommandReturnsTheJournal()
    {
        ProxyRig rig;
        QVERIFY(rig.start());
        rig.server.sendEvent("REGISTER_OK", {{"class", "register"}});
        QTRY_COMPARE(rig.proxy.lastSeq(), 2);
        QSignalSpy ra(&rig.a, &BaresipClient::responseReceived);
        rig.a.sendCommand("leansipd_events", "1");
        QTRY_COMPARE(ra.size(), 1);
        const auto resp = ra.first().first().value<BaresipResponse>();
        QVERIFY(resp.ok);
        const QJsonArray events = QJsonDocument::fromJson(resp.data.toUtf8()).array();
        QCOMPARE(events.size(), 1);
        QCOMPARE(events.first().toObject().value("type").toString(), "REGISTER_OK");

        rig.a.sendCommand("leansipd_events", "abc");
        QTRY_COMPARE(ra.size(), 2);
        QVERIFY(!ra.last().first().value<BaresipResponse>().ok);
    }

    void refusesQuitAndUnknownLocalCommandsButRunsHandlers()
    {
        ProxyRig rig;
        QVERIFY(rig.start());
        QString seenParams;
        rig.proxy.addCommand("leansipd_echo", [&](const QString &params) {
            seenParams = params;
            return CtrlProxy::Result{true, "echo " + params};
        });
        QSignalSpy ra(&rig.a, &BaresipClient::responseReceived);

        rig.a.sendCommand("quit");
        rig.a.sendCommand("leansipd_nope");
        rig.a.sendCommand("leansipd_echo", "hi");
        QTRY_COMPARE(ra.size(), 3);
        QVERIFY(!ra.at(0).first().value<BaresipResponse>().ok);
        QVERIFY(ra.at(0).first().value<BaresipResponse>().data.contains("leansipd_shutdown"));
        QVERIFY(!ra.at(1).first().value<BaresipResponse>().ok);
        QVERIFY(ra.at(2).first().value<BaresipResponse>().ok);
        QCOMPARE(ra.at(2).first().value<BaresipResponse>().data, "echo hi");
        QCOMPARE(seenParams, "hi");
        QTest::qWait(50);
        QVERIFY(rig.server.received.isEmpty()); // none of it reached baresip
    }

    void commandsFailWhileBaresipIsAwayAndPendingOnesAreFailed()
    {
        ProxyRig rig;
        QVERIFY(rig.start(/*connectUpstream=*/false));
        QSignalSpy ra(&rig.a, &BaresipClient::responseReceived);
        rig.a.sendCommand("reginfo");
        QTRY_COMPARE(ra.size(), 1);
        QVERIFY(!ra.first().first().value<BaresipResponse>().ok);
        QVERIFY(ra.first().first().value<BaresipResponse>().data.contains("not connected"));

        QSignalSpy ea(&rig.a, &BaresipClient::rawEventReceived);
        rig.upstream.connectToHost("127.0.0.1", rig.server.port());
        QTRY_VERIFY(rig.proxy.backendUp());
        QTRY_COMPARE(ea.size(), 1);
        QCOMPARE(ea.first().first().toJsonObject().value("type").toString(), "BACKEND_UP");

        // A command in flight when baresip goes away is answered, not left hanging.
        rig.a.sendCommand("dial", "1");
        QTRY_COMPARE(rig.server.received.size(), 1);
        rig.proxy.setBackendUp(false, "restarting baresip");
        QTRY_COMPARE(ra.size(), 2);
        QCOMPARE(ra.last().first().value<BaresipResponse>().data, "restarting baresip");
        QTRY_COMPARE(ea.size(), 2);
        QCOMPARE(ea.last().first().toJsonObject().value("type").toString(), "BACKEND_DOWN");
        QCOMPARE(ea.last().first().toJsonObject().value("param").toString(), "restarting baresip");
    }

    void tracksClientCount()
    {
        ProxyRig rig;
        QVERIFY(rig.start());
        QSignalSpy count(&rig.proxy, &CtrlProxy::clientCountChanged);
        rig.a.disconnectFromHost();
        QTRY_COMPARE(rig.proxy.clientCount(), 1);
        QCOMPARE(count.last().first().toInt(), 1);
        rig.b.disconnectFromHost();
        QTRY_COMPARE(rig.proxy.clientCount(), 0);
    }

    void bindIsRefusedUnlessAllowed()
    {
        ProxyRig rig;
        QVERIFY(rig.start());
        QSignalSpy ra(&rig.a, &BaresipClient::responseReceived);
        QSignalSpy gone(&rig.proxy, &CtrlProxy::ownerGone);
        rig.a.sendCommand("leansipd_bind");
        QTRY_COMPARE(ra.size(), 1);
        QVERIFY(!ra.first().first().value<BaresipResponse>().ok);
        QVERIFY(!rig.proxy.hasOwner());
        rig.a.disconnectFromHost();
        QTRY_COMPARE(rig.proxy.clientCount(), 1);
        QCOMPARE(gone.size(), 0);
    }

    void onlyTheOwnerLeavingFiresOwnerGone()
    {
        ProxyRig rig;
        QVERIFY(rig.start());
        rig.proxy.setOwnerBindingAllowed(true);
        QSignalSpy ra(&rig.a, &BaresipClient::responseReceived);
        QSignalSpy rb(&rig.b, &BaresipClient::responseReceived);
        QSignalSpy bound(&rig.proxy, &CtrlProxy::ownerBound);
        QSignalSpy gone(&rig.proxy, &CtrlProxy::ownerGone);

        rig.a.sendCommand("leansipd_bind");
        QTRY_COMPARE(ra.size(), 1);
        QVERIFY(ra.first().first().value<BaresipResponse>().ok);
        QCOMPARE(bound.size(), 1);
        QVERIFY(rig.proxy.hasOwner());

        rig.b.sendCommand("leansipd_bind"); // a second client cannot take over
        QTRY_COMPARE(rb.size(), 1);
        QVERIFY(!rb.first().first().value<BaresipResponse>().ok);

        rig.b.disconnectFromHost(); // a non-owner leaving changes nothing
        QTRY_COMPARE(rig.proxy.clientCount(), 1);
        QCOMPARE(gone.size(), 0);

        rig.a.disconnectFromHost();
        QTRY_COMPARE(gone.size(), 1);
        QVERIFY(!rig.proxy.hasOwner());
    }

    void garbageFromAClientDropsOnlyThatClient()
    {
        ProxyRig rig;
        QVERIFY(rig.start());
        QLocalSocket raw;
        raw.connectToServer(rig.socketPath());
        QVERIFY(raw.waitForConnected(1000));
        QTRY_COMPARE(rig.proxy.clientCount(), 3);
        raw.write("xyz:not a netstring");
        raw.flush();
        QTRY_COMPARE(rig.proxy.clientCount(), 2);
        QVERIFY(rig.a.isConnected());
    }
};

QTEST_GUILESS_MAIN(TestCtrlProxy)
#include "tst_ctrl_proxy.moc"
