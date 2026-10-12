#include "core/baresip_client.h"
#include "fake_baresip_server.h"

#include <QSignalSpy>
#include <QtTest>

using namespace leansip;

class TestBaresipClient : public QObject {
    Q_OBJECT
private slots:
    void connectsAndSendsFramedCommand()
    {
        FakeBaresipServer server;
        QVERIFY(server.listen());
        BaresipClient client;
        QSignalSpy connected(&client, &BaresipClient::connected);
        client.connectToHost("127.0.0.1", server.port());
        QVERIFY(connected.wait(2000));
        QVERIFY(client.isConnected());

        const QString token = client.sendCommand("dial", "123");
        QVERIFY(!token.isEmpty());
        QTRY_COMPARE(server.received.size(), 1);
        QCOMPARE(server.lastCommand(), "dial");
        QCOMPARE(server.lastParams(), "123");
        QCOMPARE(server.lastToken(), token);

        // Tokens are unique per command.
        const QString token2 = client.sendCommand("hangup");
        QVERIFY(token2 != token);
        QTRY_COMPARE(server.received.size(), 2);
        QCOMPARE(server.lastParams(), QString());
    }

    void sendWithoutConnectionReturnsEmptyToken()
    {
        BaresipClient client;
        QCOMPARE(client.sendCommand("dial", "1"), QString());
    }

    void receivesResponsesAndEvents()
    {
        FakeBaresipServer server;
        QVERIFY(server.listen());
        BaresipClient client;
        QSignalSpy connected(&client, &BaresipClient::connected);
        client.connectToHost("127.0.0.1", server.port());
        QVERIFY(connected.wait(2000));
        QTRY_VERIFY(server.hasClient());

        QSignalSpy responses(&client, &BaresipClient::responseReceived);
        QSignalSpy events(&client, &BaresipClient::eventReceived);

        server.sendResponse("42", true, "fine");
        server.sendEvent("REGISTER_OK", {{"accountaor", "sip:a@b"}, {"param", "200 OK"}});
        QTRY_COMPARE(responses.size(), 1);
        QTRY_COMPARE(events.size(), 1);

        const auto resp = responses.first().first().value<BaresipResponse>();
        QCOMPARE(resp.token, "42");
        QVERIFY(resp.ok);
        QCOMPARE(resp.data, "fine");
        const auto ev = events.first().first().value<BaresipEvent>();
        QCOMPARE(ev.type, "REGISTER_OK");
        QCOMPARE(ev.param, "200 OK");
    }

    void reassemblesSplitFrames()
    {
        FakeBaresipServer server;
        QVERIFY(server.listen());
        BaresipClient client;
        QSignalSpy connected(&client, &BaresipClient::connected);
        client.connectToHost("127.0.0.1", server.port());
        QVERIFY(connected.wait(2000));
        QTRY_VERIFY(server.hasClient());
        QSignalSpy events(&client, &BaresipClient::eventReceived);

        const QByteArray frame = netstring::encode(R"({"event":true,"class":"call","type":"CALL_CLOSED","param":"bye"})");
        server.sendRaw(frame.left(10));
        QTest::qWait(50);
        QCOMPARE(events.size(), 0);
        server.sendRaw(frame.mid(10));
        QTRY_COMPARE(events.size(), 1);
        QCOMPARE(events.first().first().value<BaresipEvent>().param, "bye");
    }

    void badFramingRaisesProtocolErrorAndDisconnects()
    {
        FakeBaresipServer server;
        QVERIFY(server.listen());
        BaresipClient client;
        QSignalSpy connected(&client, &BaresipClient::connected);
        client.connectToHost("127.0.0.1", server.port());
        QVERIFY(connected.wait(2000));
        QTRY_VERIFY(server.hasClient());
        QSignalSpy protocolError(&client, &BaresipClient::protocolError);
        QSignalSpy disconnected(&client, &BaresipClient::disconnected);

        server.sendRaw("not a netstring at all");
        QTRY_COMPARE(protocolError.size(), 1);
        QTRY_VERIFY(!client.isConnected());
    }

    void badJsonIsReportedButKeepsConnection()
    {
        FakeBaresipServer server;
        QVERIFY(server.listen());
        BaresipClient client;
        QSignalSpy connected(&client, &BaresipClient::connected);
        client.connectToHost("127.0.0.1", server.port());
        QVERIFY(connected.wait(2000));
        QTRY_VERIFY(server.hasClient());
        QSignalSpy protocolError(&client, &BaresipClient::protocolError);

        server.sendRaw(netstring::encode("{not json"));
        QTRY_COMPARE(protocolError.size(), 1);
        QVERIFY(client.isConnected());
    }

    void remoteCloseEmitsDisconnected()
    {
        FakeBaresipServer server;
        QVERIFY(server.listen());
        BaresipClient client;
        QSignalSpy connected(&client, &BaresipClient::connected);
        client.connectToHost("127.0.0.1", server.port());
        QVERIFY(connected.wait(2000));
        QTRY_VERIFY(server.hasClient());
        QSignalSpy disconnected(&client, &BaresipClient::disconnected);

        server.dropClient();
        QTRY_COMPARE(disconnected.size(), 1);
        QVERIFY(!client.isConnected());
    }

    void retriesUntilServerAppears()
    {
        // Reserve a port, release it, start connecting, then listen on it shortly after.
        QTcpServer probe;
        QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
        const quint16 port = probe.serverPort();
        probe.close();

        BaresipClient client;
        QSignalSpy connected(&client, &BaresipClient::connected);
        QSignalSpy failed(&client, &BaresipClient::connectionFailed);
        client.connectToHost("127.0.0.1", port, 3000);

        QTcpServer late;
        QTimer::singleShot(400, &late, [&] { QVERIFY(late.listen(QHostAddress::LocalHost, port)); });

        QVERIFY(connected.wait(3000));
        QCOMPARE(failed.size(), 0);
    }

    void givesUpAfterTimeout()
    {
        QTcpServer probe;
        QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
        const quint16 port = probe.serverPort();
        probe.close();

        BaresipClient client;
        QSignalSpy failed(&client, &BaresipClient::connectionFailed);
        client.connectToHost("127.0.0.1", port, 500);
        QVERIFY(failed.wait(3000));
        QVERIFY(!client.isConnected());
    }
};

QTEST_GUILESS_MAIN(TestBaresipClient)
#include "tst_baresip_client.moc"
