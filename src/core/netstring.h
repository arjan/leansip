#pragma once

#include <QByteArray>
#include <QList>
#include <optional>

namespace leansip {

// Netstring framing as used by baresip's ctrl_tcp module: "<len>:<payload>,".
namespace netstring {

QByteArray encode(const QByteArray &payload);

// Incremental decoder: feed arbitrary chunks, pull out complete payloads.
class Decoder {
public:
    // Appends data and returns all payloads that became complete.
    // On a framing error the decoder enters a failed state and returns nothing.
    QList<QByteArray> feed(const QByteArray &chunk);

    bool failed() const { return m_failed; }
    QString lastError() const { return m_error; }
    void reset();

    // Guard against a hostile/buggy peer sending an absurd length.
    static constexpr qsizetype kMaxPayload = 1 << 20;

private:
    std::optional<QByteArray> tryExtract();
    void fail(const QString &why);

    QByteArray m_buffer;
    bool m_failed = false;
    QString m_error;
};

} // namespace netstring
} // namespace leansip
