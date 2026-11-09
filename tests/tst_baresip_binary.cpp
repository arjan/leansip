#include "core/baresip_binary.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

using namespace leansip;

namespace {

void writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(data);
}

QByteArray readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

} // namespace

// extractEmbedded() reads through QFile, so a plain directory stands in for the resources.
class TestBaresipBinary : public QObject {
    Q_OBJECT
private slots:
    void extractsBinaryAndSounds()
    {
        QTemporaryDir src, cache;
        writeFile(src.filePath("baresip"), "#!/bin/sh\necho v1\n");
        writeFile(src.filePath("sounds/ring.wav"), "RIFF-ring");

        const auto l = baresip_binary::extractEmbedded(src.path(), cache.path());
        QVERIFY2(l.error.isEmpty(), qPrintable(l.error));
        QVERIFY(l.program.startsWith(cache.path() + "/baresip-"));
        QCOMPARE(readFile(l.program), QByteArray("#!/bin/sh\necho v1\n"));
        QVERIFY(QFileInfo(l.program).isExecutable());
        QCOMPARE(QFileInfo(l.program).permissions() & (QFileDevice::ReadOther | QFileDevice::WriteGroup | QFileDevice::WriteOther),
                 QFileDevice::Permissions());
        QCOMPARE(readFile(QDir(l.soundsDir).filePath("ring.wav")), QByteArray("RIFF-ring"));
        // Nothing left behind from the atomic writes.
        QCOMPARE(QDir(QFileInfo(l.program).path()).entryList(QDir::Files), QStringList{"baresip"});
    }

    void rewritesAModifiedCopy()
    {
        QTemporaryDir src, cache;
        writeFile(src.filePath("baresip"), "good");
        const auto first = baresip_binary::extractEmbedded(src.path(), cache.path());
        QVERIFY(first.error.isEmpty());
        writeFile(first.program, "tampered");

        const auto again = baresip_binary::extractEmbedded(src.path(), cache.path());
        QVERIFY(again.error.isEmpty());
        QCOMPARE(again.program, first.program);
        QCOMPARE(readFile(again.program), QByteArray("good"));
        QVERIFY(QFileInfo(again.program).isExecutable());
    }

    void anUpgradeGetsItsOwnDirectory()
    {
        QTemporaryDir src, cache;
        writeFile(src.filePath("baresip"), "v1");
        const auto v1 = baresip_binary::extractEmbedded(src.path(), cache.path());
        writeFile(src.filePath("baresip"), "v2");
        const auto v2 = baresip_binary::extractEmbedded(src.path(), cache.path());
        QVERIFY(v1.error.isEmpty() && v2.error.isEmpty());
        QVERIFY(v1.program != v2.program);
        QCOMPARE(readFile(v1.program), QByteArray("v1")); // a running older daemon keeps its copy
        QCOMPARE(readFile(v2.program), QByteArray("v2"));
    }

    void failsWithoutAnEmbeddedBinary()
    {
        QTemporaryDir src, cache;
        QVERIFY(!baresip_binary::extractEmbedded(src.path(), cache.path()).error.isEmpty());
    }

    void environmentOverrides()
    {
        qputenv("LEANSIP_BARESIP", "/opt/other/baresip");
        const auto l = baresip_binary::locate();
        qunsetenv("LEANSIP_BARESIP");
        QCOMPARE(l.program, QStringLiteral("/opt/other/baresip"));
        QVERIFY(l.error.isEmpty());
    }

    void defaultsToPath()
    {
        qunsetenv("LEANSIP_BARESIP");
        QCOMPARE(baresip_binary::locate().program, QStringLiteral("baresip"));
    }
};

QTEST_GUILESS_MAIN(TestBaresipBinary)
#include "tst_baresip_binary.moc"
