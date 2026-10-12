#include "netstring.h"

#include <QString>

namespace leansip::netstring {

QByteArray encode(const QByteArray &payload)
{
    QByteArray out = QByteArray::number(payload.size());
    out += ':';
    out += payload;
    out += ',';
    return out;
}

QList<QByteArray> Decoder::feed(const QByteArray &chunk)
{
    QList<QByteArray> out;
    if (m_failed)
        return out;
    m_buffer += chunk;
    while (!m_failed) {
        auto payload = tryExtract();
        if (!payload)
            break;
        out.append(*payload);
    }
    return out;
}

void Decoder::reset()
{
    m_buffer.clear();
    m_failed = false;
    m_error.clear();
}

void Decoder::fail(const QString &why)
{
    m_failed = true;
    m_error = why;
    m_buffer.clear();
}

std::optional<QByteArray> Decoder::tryExtract()
{
    const qsizetype colon = m_buffer.indexOf(':');
    if (colon < 0) {
        // No colon yet: a valid length prefix is at most 7 digits for kMaxPayload.
        if (m_buffer.size() > 8)
            fail(QStringLiteral("missing ':' after length"));
        return std::nullopt;
    }
    if (colon == 0) {
        fail(QStringLiteral("empty length"));
        return std::nullopt;
    }
    const QByteArray lenStr = m_buffer.left(colon);
    if (lenStr.size() > 1 && lenStr[0] == '0') {
        fail(QStringLiteral("leading zero in length"));
        return std::nullopt;
    }
    bool ok = false;
    const qlonglong len = lenStr.toLongLong(&ok);
    if (!ok || len < 0) {
        fail(QStringLiteral("invalid length '%1'").arg(QString::fromLatin1(lenStr)));
        return std::nullopt;
    }
    if (len > kMaxPayload) {
        fail(QStringLiteral("payload too large (%1 bytes)").arg(len));
        return std::nullopt;
    }
    const qsizetype total = colon + 1 + static_cast<qsizetype>(len) + 1;
    if (m_buffer.size() < total)
        return std::nullopt;
    if (m_buffer[total - 1] != ',') {
        fail(QStringLiteral("missing ',' terminator"));
        return std::nullopt;
    }
    QByteArray payload = m_buffer.mid(colon + 1, static_cast<qsizetype>(len));
    m_buffer.remove(0, total);
    return payload;
}

} // namespace leansip::netstring
