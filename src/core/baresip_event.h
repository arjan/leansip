#pragma once

#include <QJsonObject>
#include <QString>
#include <optional>

namespace leansip {

// One event message from baresip's ctrl_tcp module.
struct BaresipEvent {
    QString klass;           // "call", "register", "application", ...
    QString type;            // "CALL_INCOMING", "REGISTER_OK", ...
    QString accountAor;
    QString direction;       // "incoming" / "outgoing"
    QString peerUri;
    QString peerDisplayName;
    QString param;           // type-specific: close reason, register status text, ...
    QString callId;          // "id": the call an event is about (empty for non-call events)

    static std::optional<BaresipEvent> fromJson(const QJsonObject &obj);
};

// Reply to a command, matched by token.
struct BaresipResponse {
    QString token;
    bool ok = false;
    QString data;

    static std::optional<BaresipResponse> fromJson(const QJsonObject &obj);
};

} // namespace leansip
