// sipctl: command-line client for leansipd, meant for scripts and coding agents.
// Every command prints one JSON object per line; exit codes: 0 ok, 1 failed or timed out,
// 2 usage error, 3 leansipd not running.

#include "core/call_controller.h"
#include "core/daemon_client.h"
#include "core/profile.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QThread>

#include <cstdio>

using namespace leansip;

namespace {

enum Exit { Ok = 0, Failed = 1, Usage = 2, NotRunning = 3 };

struct CommandHelp {
    const char *name;
    const char *synopsis;
    const char *summary;
    const char *details;
};

// Single source for `sipctl help`, `sipctl help <command>` and `sipctl <command> --help`.
const CommandHelp kCommands[] = {
    {"status", "status", "daemon state; exit 3 when not running",
     R"(Prints the daemon's state as JSON in "data":
  baresipDir, runtimeDir, socket, log   where things are
  pid, baresipPid                       processes
  baresipUp                             connected to baresip
  clients                               connected clients (this sipctl included)
  uiOwned, uiAttached                   started by the dialer; dialer still connected
  sipTrace                              baresip runs with -s (SIP messages in the log)
  seq                                   last event number
Not running: {"ok":true,"running":false,...} and exit code 3.)"},
    {"start", "start [--sip-trace]", "start leansipd for this account (no-op if running)",
     R"(Starts leansipd in the background and returns once it accepts connections.
A daemon started here keeps running until `sipctl stop`.

If the leansip dialer already runs for this account, its daemon is reused. That daemon
belongs to the dialer and stops when the dialer closes; "status.uiOwned" is then true and
a "note" says so.

--sip-trace  run baresip with -s: every SIP message goes to the log file ("status.log").
             Only takes effect when this call starts the daemon.

Output: {"ok":true,"started":<bool>,"status":{...}})"},
    {"stop", "stop", "stop leansipd and its baresip (ends calls, unregisters)",
     R"(Shuts the daemon down. Every client loses its connection and every call ends.
If the dialer is open, it starts a fresh daemon of its own right away.)"},
    {"dial", "dial <number|sip-uri>", "place a call",
     R"(Dials from the first account of the baresip directory. Accepts digits, * # +,
tel: and sip:/sips: URIs, or user@host[:port]. Anything else (spaces, ';', quotes) is
rejected before it reaches baresip.

Output: {"ok":true,"seq":12,"data":"call id: 35fa5b711b6ec554"}
Then:   sipctl wait CALL_ESTABLISHED,CALL_CLOSED --since 12 --timeout 30

Every dial is a real call on a real account and may cost money.)"},
    {"answer", "answer", "answer the ringing incoming call",
     R"(Sends baresip's `accept`. Wait for the call first:
  sipctl wait CALL_INCOMING --timeout 60 && sipctl answer)"},
    {"hangup", "hangup", "hang up the current call (or reject a ringing one)", R"(Sends baresip's `hangup`.)"},
    {"dtmf", "dtmf <digits>", "send DTMF digits in the established call",
     R"(Digits 0-9 * # A-D, sent one at a time 200 ms apart as RFC 4733 events.
The far end (if it is another leansipd) reports them as CALL_DTMF_START/CALL_DTMF_END.
Output: {"ok":true,"seq":N,"data":"<digits>"})"},
    {"calls", "calls", "list active calls", R"(baresip's `listcalls`, ANSI colours stripped, as text in "data".)"},
    {"reginfo", "reginfo", "registration state of the accounts",
     R"(baresip's `reginfo` as text in "data". A registered account's line contains "OK".)"},
    {"raw", "raw <command> [params...]", "send any baresip ctrl command",
     R"(For everything sipctl has no wrapper for. Params are joined with spaces.
Useful ones:
  raw callstat          SDP, codecs, RTP/RTCP statistics of the current call
  raw help              all commands this baresip knows
  raw hold / raw resume
  raw uafind <aor>      select the account used by the next dial
`quit` is refused because it would stop baresip for every client; use `sipctl stop`.)"},
    {"events", "events [--since N] [--follow]", "print journalled events, optionally keep streaming",
     R"(Prints one JSON event per line: every event after sequence number N (default 0,
i.e. all the daemon still keeps; it keeps the last 2000). With --follow it keeps
printing new events until interrupted.

Every event has "event":true, "class", "type", "seq" and "ts" (ms since epoch); call
events also have "accountaor", "direction", "peeruri", "id" (call id) and "param".)"},
    {"wait", "wait <TYPE[,TYPE...]> [--since N] [--timeout S]", "block until one of these events arrives, print it",
     R"(Exit 0 and print the first matching event; exit 1 with {"ok":false,"error":"timeout"}
after S seconds (default 30).

Without --since only events after `wait` connected count. With --since N, events already
journalled after N match too. Use the "seq" of the command you are waiting on, so an event
that fires before `wait` starts is not missed:
  seq=$(sipctl dial 940 | jq .seq)
  sipctl wait CALL_ESTABLISHED,CALL_CLOSED --since "$seq" --timeout 30

Common types: CALL_INCOMING CALL_RINGING CALL_PROGRESS CALL_ESTABLISHED CALL_CLOSED
CALL_DTMF_START CALL_DTMF_END REGISTER_OK REGISTER_FAIL, and leansipd's own
BACKEND_UP / BACKEND_DOWN (baresip (re)started or went away).)"},
    {"help", "help [command]", "this overview, or details on one command", ""},
};

const char *kOverviewTail = R"(
Global options:
  -d, --baresip-dir <dir>   which account: the baresip directory (default ~/.baresip).
                            Each directory has its own daemon.
  -h, --help                this help; `sipctl <command> --help` for one command

Output: one JSON object per line on stdout. Replies look like
  {"ok":true,"seq":12,"data":...}
where "seq" is the event counter when the command arrived (see `sipctl help wait`).
Exit codes: 0 ok, 1 failed or timed out, 2 usage error, 3 leansipd not running.

Example:
  sipctl -d ~/.baresip2 start
  seq=$(sipctl -d ~/.baresip2 dial 940 | jq .seq)
  sipctl -d ~/.baresip2 wait CALL_ESTABLISHED,CALL_CLOSED --since "$seq"
  sipctl -d ~/.baresip2 dtmf 1
  sipctl -d ~/.baresip2 hangup

Full reference: docs/sipctl.md in the leansip source tree.)";

const CommandHelp *findCommand(const QString &name)
{
    for (const CommandHelp &c : kCommands) {
        if (name == QLatin1String(c.name))
            return &c;
    }
    return nullptr;
}

QString overview()
{
    QString out = QStringLiteral("sipctl: control leansipd, the daemon that shares baresip between the leansip\n"
                                 "dialer, scripts and agents.\n\nUsage: sipctl [-d DIR] <command> [args] [options]\n\nCommands:\n");
    for (const CommandHelp &c : kCommands)
        out += QStringLiteral("  %1%2\n").arg(QString::fromUtf8(c.synopsis).leftJustified(48), QString::fromUtf8(c.summary));
    return out + QString::fromUtf8(kOverviewTail);
}

QString commandHelp(const CommandHelp &c)
{
    QString out = QStringLiteral("Usage: sipctl [-d DIR] %1\n\n%2.\n").arg(QString::fromUtf8(c.synopsis), QString::fromUtf8(c.summary));
    if (*c.details)
        out += QStringLiteral("\n%1\n").arg(QString::fromUtf8(c.details));
    return out;
}

void printText(const QString &text, FILE *to = stdout)
{
    std::fputs(qPrintable(text), to);
    if (!text.endsWith(u'\n'))
        std::fputc('\n', to);
}

void printJson(const QJsonObject &obj)
{
    std::printf("%s\n", QJsonDocument(obj).toJson(QJsonDocument::Compact).constData());
    std::fflush(stdout);
}

QString stripAnsi(QString text)
{
    static const QRegularExpression ansi(QStringLiteral("\x1b\\[[0-9;]*[A-Za-z]"));
    return text.remove(ansi).trimmed();
}

int fail(const QString &why, int code = Failed)
{
    printJson({{QStringLiteral("ok"), false}, {QStringLiteral("error"), why}});
    return code;
}

// Data that is itself JSON (daemon commands) is embedded as JSON, everything else as text.
QJsonValue dataValue(const QString &data)
{
    const QJsonDocument doc = QJsonDocument::fromJson(data.toUtf8());
    if (doc.isObject())
        return doc.object();
    if (doc.isArray())
        return doc.array();
    return stripAnsi(data);
}

int printReply(const daemon::Reply &reply)
{
    printJson({{QStringLiteral("ok"), reply.ok}, {QStringLiteral("seq"), reply.seq}, {QStringLiteral("data"), dataValue(reply.data)}});
    return reply.ok ? Ok : Failed;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("sipctl"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    QCommandLineParser parser;
    parser.addVersionOption();
    const QCommandLineOption helpOpt({QStringLiteral("h"), QStringLiteral("help")}, QStringLiteral("help"));
    const QCommandLineOption dirOpt({QStringLiteral("d"), QStringLiteral("baresip-dir")},
                                    QStringLiteral("baresip config directory (selects the account/daemon)"),
                                    QStringLiteral("dir"), profile::defaultBaresipDir());
    const QCommandLineOption sinceOpt(QStringLiteral("since"), QStringLiteral("event sequence number"), QStringLiteral("seq"));
    const QCommandLineOption timeoutOpt(QStringLiteral("timeout"), QStringLiteral("seconds (wait; default 30)"),
                                        QStringLiteral("s"), QStringLiteral("30"));
    const QCommandLineOption followOpt(QStringLiteral("follow"), QStringLiteral("keep streaming events"));
    const QCommandLineOption traceOpt(QStringLiteral("sip-trace"), QStringLiteral("start: log SIP messages"));
    parser.addOptions({helpOpt, dirOpt, sinceOpt, timeoutOpt, followOpt, traceOpt});
    if (!parser.parse(QCoreApplication::arguments())) {
        printText(QStringLiteral("sipctl: %1\nRun `sipctl help` for usage.").arg(parser.errorText()), stderr);
        return Usage;
    }
    if (parser.isSet(QStringLiteral("version"))) {
        printText(QStringLiteral("sipctl %1").arg(QCoreApplication::applicationVersion()));
        return Ok;
    }

    const QStringList args = parser.positionalArguments();
    // `sipctl help [cmd]`, `sipctl --help`, `sipctl <cmd> --help`
    const bool helpCommand = !args.isEmpty() && args.first() == QLatin1String("help");
    if (helpCommand || parser.isSet(helpOpt)) {
        const QString topic = helpCommand ? args.value(1) : args.value(0);
        if (topic.isEmpty()) {
            printText(overview());
            return Ok;
        }
        const CommandHelp *c = findCommand(topic);
        if (!c) {
            printText(QStringLiteral("sipctl: no command '%1'\n\n%2").arg(topic, overview()), stderr);
            return Usage;
        }
        printText(commandHelp(*c));
        return Ok;
    }
    if (args.isEmpty()) {
        printText(overview(), stderr);
        return Usage;
    }
    const QString command = args.first();
    const QStringList rest = args.mid(1);
    const QString baresipDir = QDir(parser.value(dirOpt)).absolutePath();
    const QString socketPath = profile::socketPath(baresipDir);

    bool sinceOk = true;
    const qint64 since = parser.isSet(sinceOpt) ? parser.value(sinceOpt).toLongLong(&sinceOk) : -1;
    if (!sinceOk)
        return fail(QStringLiteral("--since expects a number"), Usage);

    if (command == QLatin1String("start")) {
        QString error;
        bool started = false;
        const QStringList daemonArgs = parser.isSet(traceOpt) ? QStringList{QStringLiteral("--sip-trace")} : QStringList{};
        if (!daemon::ensureRunning(baresipDir, daemonArgs, &error, &started))
            return fail(error);
        daemon::Connection c;
        if (!c.open(socketPath, 2000, &error))
            return fail(error, NotRunning);
        // Return only once baresip is reachable, so the next command does not race its startup.
        std::optional<daemon::Reply> status;
        const QDeadlineTimer upDeadline(5000);
        for (;;) {
            status = c.request(QStringLiteral("leansipd_status"), {}, 2000, &error);
            if (!status)
                return fail(error);
            if (dataValue(status->data).toObject().value(QStringLiteral("baresipUp")).toBool() || upDeadline.hasExpired())
                break;
            QThread::msleep(100);
        }
        QJsonObject out{{QStringLiteral("ok"), true}, {QStringLiteral("started"), started},
                        {QStringLiteral("status"), dataValue(status->data)}};
        if (!dataValue(status->data).toObject().value(QStringLiteral("baresipUp")).toBool())
            out[QStringLiteral("warning")] = QStringLiteral("baresip is not up yet; check the log");
        if (dataValue(status->data).toObject().value(QStringLiteral("uiOwned")).toBool())
            out[QStringLiteral("note")] = QStringLiteral("this daemon belongs to the open leansip dialer and stops when it closes");
        if (!started && parser.isSet(traceOpt) && !dataValue(status->data).toObject().value(QStringLiteral("sipTrace")).toBool())
            out[QStringLiteral("warning")] = QStringLiteral("already running without --sip-trace; stop it first to trace");
        printJson(out);
        return Ok;
    }

    if (!findCommand(command))
        return fail(QStringLiteral("unknown command '%1'; see `sipctl help`").arg(command), Usage);

    daemon::Connection c;
    QString error;
    if (!QFileInfo::exists(socketPath))
        error = QStringLiteral("leansipd is not running for %1").arg(baresipDir);
    if (!error.isEmpty() || !c.open(socketPath, 1000, &error)) {
        if (command == QLatin1String("status")) {
            printJson({{QStringLiteral("ok"), true}, {QStringLiteral("running"), false}, {QStringLiteral("socket"), socketPath}});
            return NotRunning;
        }
        return fail(error + QStringLiteral("; run `sipctl -d %1 start` first").arg(baresipDir), NotRunning);
    }

    auto simple = [&](const QString &cmd, const QString &params = {}, int timeoutMs = 5000) {
        const auto reply = c.request(cmd, params, timeoutMs, &error);
        return reply ? printReply(*reply) : fail(error);
    };

    if (command == QLatin1String("status"))
        return simple(QStringLiteral("leansipd_status"));
    if (command == QLatin1String("stop"))
        return simple(QStringLiteral("leansipd_shutdown"));
    if (command == QLatin1String("answer"))
        return simple(QStringLiteral("accept"));
    if (command == QLatin1String("hangup"))
        return simple(QStringLiteral("hangup"));
    if (command == QLatin1String("calls"))
        return simple(QStringLiteral("listcalls"));
    if (command == QLatin1String("reginfo"))
        return simple(QStringLiteral("reginfo"));

    if (command == QLatin1String("dial")) {
        if (rest.size() != 1)
            return fail(QStringLiteral("usage: sipctl dial <number|sip-uri>"), Usage);
        QString target = rest.first().trimmed();
        if (target.startsWith(QLatin1String("tel:"), Qt::CaseInsensitive))
            target = target.mid(4);
        // Same whitelist as the dialer: the string goes verbatim to baresip's command parser.
        if (!CallController::isValidNumber(target))
            return fail(QStringLiteral("invalid number or URI '%1'").arg(target), Usage);
        return simple(QStringLiteral("dial"), target);
    }

    if (command == QLatin1String("dtmf")) {
        static const QRegularExpression digitsRe(QStringLiteral("^[0-9*#A-D]+$"));
        if (rest.size() != 1 || !digitsRe.match(rest.first()).hasMatch())
            return fail(QStringLiteral("usage: sipctl dtmf <digits 0-9*#A-D>"), Usage);
        // One digit at a time, spaced so each gets its own RFC 4733 event.
        const QString digits = rest.first();
        qint64 seq = 0;
        for (qsizetype i = 0; i < digits.size(); ++i) {
            if (i > 0)
                QThread::msleep(200);
            const auto reply = c.request(QStringLiteral("sndcode"), QString(digits.at(i)), 5000, &error);
            if (!reply)
                return fail(error);
            if (!reply->ok)
                return printReply(*reply);
            if (i == 0)
                seq = reply->seq;
        }
        printJson({{QStringLiteral("ok"), true}, {QStringLiteral("seq"), seq}, {QStringLiteral("data"), digits}});
        return Ok;
    }

    if (command == QLatin1String("raw")) {
        if (rest.isEmpty())
            return fail(QStringLiteral("usage: sipctl raw <command> [params...]"), Usage);
        return simple(rest.first(), rest.mid(1).join(u' '), 15000);
    }

    if (command == QLatin1String("events") || command == QLatin1String("wait")) {
        const bool waiting = command == QLatin1String("wait");
        QStringList types;
        if (waiting) {
            if (rest.size() != 1)
                return fail(QStringLiteral("usage: sipctl wait <TYPE[,TYPE...]> [--since N] [--timeout S]"), Usage);
            types = rest.first().split(u',', Qt::SkipEmptyParts);
        }
        auto matches = [&](const QJsonObject &ev) {
            return !waiting || types.contains(ev.value(QStringLiteral("type")).toString());
        };

        // Subscribed from here on (we are connected); the journal covers what came before.
        qint64 lastSeen = since;
        if (since >= 0 || !waiting) {
            const auto reply = c.request(QStringLiteral("leansipd_events"), QString::number(qMax<qint64>(since, 0)), 5000, &error);
            if (!reply || !reply->ok)
                return fail(reply ? reply->data : error);
            const QJsonArray journal = QJsonDocument::fromJson(reply->data.toUtf8()).array();
            for (const QJsonValue &v : journal) {
                const QJsonObject ev = v.toObject();
                lastSeen = qMax(lastSeen, ev.value(QStringLiteral("seq")).toInteger());
                if (!matches(ev))
                    continue;
                printJson(ev);
                if (waiting)
                    return Ok;
            }
            if (!waiting && !parser.isSet(followOpt))
                return Ok;
        }

        bool timeoutOk = false;
        const double seconds = parser.value(timeoutOpt).toDouble(&timeoutOk);
        if (!timeoutOk || seconds < 0)
            return fail(QStringLiteral("--timeout expects seconds"), Usage);
        const QDeadlineTimer deadline = waiting ? QDeadlineTimer(static_cast<qint64>(seconds * 1000))
                                                : QDeadlineTimer(QDeadlineTimer::Forever);
        while (!deadline.hasExpired()) {
            const int slice = deadline.isForever() ? 1000 : static_cast<int>(qMin<qint64>(deadline.remainingTime(), 1000));
            const auto ev = c.nextEvent(slice);
            if (!ev) {
                if (!c.isOpen())
                    return fail(QStringLiteral("leansipd closed the connection"));
                continue;
            }
            const qint64 seq = ev->value(QStringLiteral("seq")).toInteger();
            if (seq <= lastSeen)
                continue; // already printed from the journal
            lastSeen = seq;
            if (!matches(*ev))
                continue;
            printJson(*ev);
            if (waiting)
                return Ok;
        }
        printJson({{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("timeout")},
                   {QStringLiteral("waitedFor"), QJsonArray::fromStringList(types)}});
        return Failed;
    }

    return fail(QStringLiteral("unknown command '%1'; see `sipctl help`").arg(command), Usage);
}
