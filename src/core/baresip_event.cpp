#include "baresip_event.h"

namespace leansip {

std::optional<BaresipEvent> BaresipEvent::fromJson(const QJsonObject &obj)
{
    if (!obj.value(QStringLiteral("event")).toBool(false))
        return std::nullopt;
    BaresipEvent ev;
    ev.klass = obj.value(QStringLiteral("class")).toString();
    ev.type = obj.value(QStringLiteral("type")).toString();
    if (ev.type.isEmpty())
        return std::nullopt;
    ev.accountAor = obj.value(QStringLiteral("accountaor")).toString();
    ev.direction = obj.value(QStringLiteral("direction")).toString();
    ev.peerUri = obj.value(QStringLiteral("peeruri")).toString();
    ev.peerDisplayName = obj.value(QStringLiteral("peerdisplayname")).toString();
    ev.param = obj.value(QStringLiteral("param")).toString();
    ev.callId = obj.value(QStringLiteral("id")).toString();
    ev.sent = obj.value(QStringLiteral("sent")).toString();
    ev.received = obj.value(QStringLiteral("received")).toString();
    return ev;
}

std::optional<BaresipResponse> BaresipResponse::fromJson(const QJsonObject &obj)
{
    if (!obj.value(QStringLiteral("response")).toBool(false))
        return std::nullopt;
    BaresipResponse r;
    r.token = obj.value(QStringLiteral("token")).toString();
    r.ok = obj.value(QStringLiteral("ok")).toBool(false);
    r.data = obj.value(QStringLiteral("data")).toString();
    return r;
}

} // namespace leansip
