#pragma once

#include "baresip_event.h"
#include "netstring.h"

#include <QElapsedTimer>
#include <QObject>
#include <QTcpSocket>
#include <QTimer>

namespace leansip {

// JSON-over-netstring client for baresip's ctrl_tcp module.
class BaresipClient : public QObject {
    Q_OBJECT
public:
    explicit BaresipClient(QObject *parent = nullptr);

    // Keeps retrying until connected or `timeoutMs` elapsed (baresip takes a moment to bind).
    void connectToHost(const QString &host, quint16 port, int timeoutMs = 5000);
    void disconnectFromHost();
    bool isConnected() const;

    // Returns the token used to match the response. Returns empty if not connected.
    QString sendCommand(const QString &command, const QString &params = {});

signals:
    void connected();
    void disconnected();
    void connectionFailed(const QString &error);
    void responseReceived(const leansip::BaresipResponse &response);
    void eventReceived(const leansip::BaresipEvent &event);
    void protocolError(const QString &error);

private:
    void attemptConnect();
    void onSocketError(QAbstractSocket::SocketError error);
    void onReadyRead();
    void handleFrame(const QByteArray &payload);

    QTcpSocket m_socket;
    netstring::Decoder m_decoder;
    QTimer m_retryTimer;
    QElapsedTimer m_connectClock;
    QString m_host;
    quint16 m_port = 0;
    int m_timeoutMs = 0;
    bool m_connecting = false;
    bool m_wasConnected = false;
    quint64 m_nextToken = 1;
};

} // namespace leansip

Q_DECLARE_METATYPE(leansip::BaresipEvent)
Q_DECLARE_METATYPE(leansip::BaresipResponse)
