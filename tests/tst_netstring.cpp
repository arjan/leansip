#include "core/netstring.h"

#include <QtTest>

using namespace leansip::netstring;

class TestNetstring : public QObject {
    Q_OBJECT
private slots:
    void encodeBasic()
    {
        QCOMPARE(encode("hello"), QByteArray("5:hello,"));
        QCOMPARE(encode(""), QByteArray("0:,"));
    }

    void decodeSingleFrame()
    {
        Decoder d;
        const auto out = d.feed("5:hello,");
        QCOMPARE(out.size(), 1);
        QCOMPARE(out.first(), QByteArray("hello"));
        QVERIFY(!d.failed());
    }

    void decodeMultipleFramesInOneChunk()
    {
        Decoder d;
        const auto out = d.feed("1:a,2:bc,0:,");
        QCOMPARE(out, (QList<QByteArray>{"a", "bc", ""}));
    }

    void decodePartialFrames()
    {
        Decoder d;
        QVERIFY(d.feed("1").isEmpty());
        QVERIFY(d.feed("1:hello").isEmpty());
        QVERIFY(d.feed(" wor").isEmpty());
        const auto out = d.feed("ld,3:abc,");
        QCOMPARE(out, (QList<QByteArray>{"hello world", "abc"}));
    }

    void decodePayloadContainingDelimiters()
    {
        Decoder d;
        const QByteArray payload = R"({"a":"1:2,3"})";
        QCOMPARE(d.feed(encode(payload)), QList<QByteArray>{payload});
    }

    void roundTripBinary()
    {
        QByteArray payload;
        for (int i = 0; i < 256; ++i)
            payload.append(static_cast<char>(i));
        Decoder d;
        QCOMPARE(d.feed(encode(payload)), QList<QByteArray>{payload});
    }

    void rejectsMissingComma()
    {
        Decoder d;
        QVERIFY(d.feed("2:ab;").isEmpty());
        QVERIFY(d.failed());
        QVERIFY(d.lastError().contains("','"));
    }

    void rejectsLeadingZero()
    {
        Decoder d;
        d.feed("05:hello,");
        QVERIFY(d.failed());
    }

    void rejectsNonNumericLength()
    {
        Decoder d;
        d.feed("ab:x,");
        QVERIFY(d.failed());
    }

    void rejectsAbsurdLength()
    {
        Decoder d;
        d.feed("99999999999:");
        QVERIFY(d.failed());
    }

    void rejectsGarbageWithoutColon()
    {
        Decoder d;
        d.feed("this is not a netstring");
        QVERIFY(d.failed());
    }

    void stopsAfterFailureUntilReset()
    {
        Decoder d;
        d.feed("x:");
        QVERIFY(d.failed());
        QVERIFY(d.feed("1:a,").isEmpty());
        d.reset();
        QVERIFY(!d.failed());
        QCOMPARE(d.feed("1:a,"), QList<QByteArray>{"a"});
    }
};

QTEST_APPLESS_MAIN(TestNetstring)
#include "tst_netstring.moc"
