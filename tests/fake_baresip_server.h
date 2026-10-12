#pragma once

#include "core/netstring.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>

// In-process stand-in for baresip's ctrl_tcp: records commands, lets tests push events/responses.
class FakeBaresipServer : public QObject {
    Q_OBJECT
public:
    explicit FakeBaresipServer(QObject *parent = nullptr)
        : QObject(parent)
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            m_client = m_server.nextPendingConnection();
            m_decoder.reset();
            connect(m_client, &QTcpSocket::readyRead, this, [this] {
                for (const QByteArray &frame : m_decoder.feed(m_client->readAll())) {
                    const QJsonObject obj = QJsonDocument::fromJson(frame).object();
                    received.append(obj);
                    if (autoRespond) {
                        sendResponse(obj.value("token").toString(), true, QStringLiteral("auto"));
                    }
                }
            });
        });
    }

    bool listen() { return m_server.listen(QHostAddress::LocalHost, 0); }
    quint16 port() const { return m_server.serverPort(); }
    bool hasClient() const { return m_client && m_client->state() == QAbstractSocket::ConnectedState; }

    void sendRaw(const QByteArray &bytes)
    {
        m_client->write(bytes);
        m_client->flush();
    }
    void sendJson(const QJsonObject &obj)
    {
        sendRaw(leansip::netstring::encode(QJsonDocument(obj).toJson(QJsonDocument::Compact)));
    }
    void sendEvent(const QString &type, const QJsonObject &extra = {})
    {
        QJsonObject obj = extra;
        obj["event"] = true;
        obj["class"] = extra.value("class").toString(QStringLiteral("call"));
        obj["type"] = type;
        sendJson(obj);
    }
    void sendResponse(const QString &token, bool ok, const QString &data = {})
    {
        QJsonObject obj;
        obj["response"] = true;
        obj["ok"] = ok;
        obj["data"] = data;
        obj["token"] = token;
        sendJson(obj);
    }
    void dropClient()
    {
        if (m_client)
            m_client->disconnectFromHost();
    }
    void close() { m_server.close(); }

    QString lastCommand() const { return received.isEmpty() ? QString() : received.last().value("command").toString(); }
    QString lastParams() const { return received.isEmpty() ? QString() : received.last().value("params").toString(); }
    QString lastToken() const { return received.isEmpty() ? QString() : received.last().value("token").toString(); }

    QList<QJsonObject> received;
    bool autoRespond = false;

private:
    QTcpServer m_server;
    QTcpSocket *m_client = nullptr;
    leansip::netstring::Decoder m_decoder;
};
