// leansipctl: command-line client for leansipd, meant for scripts and coding agents.
// Every command prints one JSON object per line; exit codes: 0 ok, 1 failed or timed out,
// 2 usage error, 3 leansipd not running.

#include "core/call_analysis.h"
#include "core/call_controller.h"
#include "core/daemon_client.h"
#include "core/elevenlabs.h"
#include "core/profile.h"
#include "core/settings.h"
#include "core/speech_segmenter.h"
#include "core/wav.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QThread>

#include <cerrno>
#include <cmath>
#include <csignal>
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

// Single source for `leansipctl help`, `leansipctl help <command>` and `leansipctl <command> --help`.
const CommandHelp kCommands[] = {
    {"status", "status", "daemon state; exit 3 when not running",
     R"(Prints the daemon's state as JSON in "data":
  profile, aor, config                  which profile of which config file
  confirmDial                           dial needs --yes (profile setting)
  runtimeDir, socket, log               where things are
  pid, baresipPid, baresipUp            processes; connected to baresip
  clients                               connected clients (this leansipctl included)
  uiOwned, uiAttached                   started by the dialer; dialer still connected
  sipTrace, recording, recordingsDir    options the daemon runs with
  maxCallSeconds                        limit for calls leansipctl places or answers
  playing                               call a clip is playing into, if any
  seq                                   last event number
Not running: {"ok":true,"running":false,...} and exit code 3.)"},
    {"profiles", "profiles", "list the profiles in leansip.toml and which ones run",
     R"(Needs no daemon. Prints {"ok":true,"config":<path>,"default":<name>,"profiles":[...]}
with name, aor, displayName, label, shortcut, confirmDial and running for each profile. Passwords never
appear.)"},
    {"start", "start [--sip-trace] [--record] [--max-call-seconds S]", "start leansipd for this profile (no-op if running)",
     R"(Starts leansipd in the background and returns once baresip is reachable (up to 5 s
after the daemon is up; a password_cmd may make the daemon itself take longer).
A daemon started here keeps running until `leansipctl stop`.

If the leansip dialer already runs for this profile, its daemon is reused. That daemon
belongs to the dialer and stops when the dialer closes; "status.uiOwned" is then true and
a "note" says so.

--sip-trace           every SIP message goes to the log ("status.log")
--record              record every call, both directions, as WAV (needed by listen/analyze levels);
                      the profile setting record = true does the same
--max-call-seconds S  hang up calls leansipctl placed or answered after S seconds (default 600, 0 = off)
These only take effect when this call starts the daemon; otherwise a "warning" says so.

Output: {"ok":true,"started":<bool>,"status":{...}})"},
    {"stop", "stop", "stop leansipd and its baresip (ends calls, unregisters)",
     R"(Shuts the daemon down and returns once it has exited (up to 10 s), so a following
`start` can bind the same SIP port. If the dialer is open, it starts a new daemon for itself.)"},
    {"dial", "dial <number|sip-uri> [--yes]", "place a call",
     R"(Accepts digits, * # +, tel: and sip:/sips: URIs, or user@host[:port]. Anything else
(spaces, ';', quotes) is rejected before it reaches baresip.

--yes  required when the profile has confirm_dial = true (a real trunk, say). The same goes
       for `raw` commands that dial. This guards against mistakes; it is not access control.

Output: {"ok":true,"seq":12,"call":"35fa5b711b6ec554","data":"call id: 35fa5b711b6ec554"}
Then:   leansipctl wait CALL_ESTABLISHED,CALL_CLOSED --call <id> --since 12 --timeout 30

Every dial is a real call on a real account and may cost money.)"},
    {"answer", "answer [--call ID]", "answer a ringing incoming call",
     R"(Sends baresip's `accept`. Wait for the call first:
  leansipctl wait CALL_INCOMING --timeout 60   # prints the event; its "id" is the call)"},
    {"hangup", "hangup [--call ID]", "hang up a call (or reject a ringing one)",
     R"(Without --call: baresip's current call. With several calls, pass --call.)"},
    {"dtmf", "dtmf <digits> [--call ID]", "send DTMF digits in an established call",
     R"(Digits 0-9 * # A-D, sent one at a time 200 ms apart as RFC 4733 events.
The far end (if it is another leansipd) reports them as CALL_DTMF_START/CALL_DTMF_END.)"},
    {"mute", "mute [--call ID]", "toggle the microphone of a call", R"(baresip's `mute`; replies "call muted" or "call un-muted".)"},
    {"hold", "hold [--call ID]", "put a call on hold", R"(baresip's `hold` (a re-INVITE).)"},
    {"resume", "resume [--call ID]", "take a call off hold",
     R"(baresip 1.1.0 replies "Invalid argument" even when it resumes the call; leansipctl reports
ok when `calls` no longer shows the call on hold.)"},
    {"calls", "calls", "list active calls", R"(baresip's `listcalls` as text in "data". ">" marks baresip's current call.)"},
    {"reginfo", "reginfo", "registration state of the accounts",
     R"(baresip's `reginfo` as text in "data". A registered account's line contains "OK".)"},
    {"say", "say <text> [--call ID] [--wait] [--out FILE]", "speak text into a call (ElevenLabs TTS)",
     R"(Needs ELEVENLABS_API_KEY. Optional: ELEVENLABS_VOICE_ID (default JBFqnCBsd6RMkjVDRZzb),
ELEVENLABS_TTS_MODEL (default eleven_multilingual_v2).

The speech replaces the call's microphone/source while it plays; the source is switched back
afterwards. --wait returns when it has finished playing (PLAY_ENDED). --out keeps the
synthesized audio (16 kHz mono WAV).

Output: {"ok":true,"seq":N,"call":"<id>","seconds":2.4,...})"},
    {"play", "play <file.wav> [--call ID] [--wait]", "play a WAV file into a call",
     R"(16-bit PCM WAV, mono or stereo, any rate: it is converted to the call's audio format.
Like say, it replaces the call's source while playing. --wait returns at PLAY_ENDED.)"},
    {"listen", "listen [--call ID] [--timeout S] [--silence MS] [--max S] [--threshold DB] [--no-stt] [--language L] [--out FILE]",
     "wait for the far end to say something and transcribe it (ElevenLabs STT)",
     R"(Needs a daemon started with --record, and ELEVENLABS_API_KEY unless --no-stt.
Optional: ELEVENLABS_STT_MODEL (default scribe_v2).

Listens to what the call receives from now on: waits up to --timeout seconds (default 30) for
speech louder than --threshold dBFS (default -45), then records until --silence ms of quiet
(default 1200) or --max seconds (default 30), and transcribes that utterance.

Output: {"ok":true,"text":"Welcome to ...","language":"eng","seconds":3.1,"audio":"<wav>",
         "truncated":false}
No speech before the timeout: {"ok":false,"error":"no speech ..."}, exit 1.
--no-stt skips transcription and just returns the utterance's audio file.)"},
    {"transcribe", "transcribe <file.wav> [--language L]", "transcribe a WAV file (ElevenLabs STT)",
     R"(Needs ELEVENLABS_API_KEY. The audio is converted to 16 kHz mono before upload.
Output: {"ok":true,"text":"...","language":"eng","words":[...]})"},
    {"recordings", "recordings", "recorded calls and their WAV files",
     R"(Needs a daemon started with --record. "data" maps call ids to {"sent","received"} paths.
The files live in the runtime directory (cleared at logout); copy what you want to keep.)"},
    {"analyze", "analyze [--call ID]", "summarise a call: timing, close reason, DTMF, RTCP, audio levels",
     R"(Default: the most recent call. From the daemon's event journal (last 2000 events):
  timing        ringingAfterMs, answeredAfterMs, talkMs, totalMs
  closeReason   e.g. "486 Busy Here", "Connection reset by peer"
  dtmfReceived  digits the far end sent
  audio         codec, sample rate, channels
  rtcp          last RTCP statistics (rtt, rx/tx jitter and loss), max jitter
  levels        with --record: rmsDbfs, peakDbfs, silentPercent per direction
  warnings      e.g. one-way audio, packet loss, never answered)"},
    {"raw", "raw <command> [params...] [--call ID]", "send any baresip ctrl command",
     R"(For everything leansipctl has no wrapper for. Params are joined with spaces; with --call the
command runs right after selecting that call. `raw help` lists baresip's commands (call
commands only appear while a call is active). `quit` is refused; use `leansipctl stop`.
`raw leansipd_restart` reloads the profile from leansip.toml and restarts baresip.)"},
    {"events", "events [--since N] [--follow] [--call ID] [--account AOR] [--param TEXT]",
     "print journalled events, optionally keep streaming",
     R"(One JSON event per line: every event after sequence number N (default 0: all the daemon
keeps, the last 2000). --follow keeps printing new events. --call/--account/--param filter
(param: substring match).)"},
    {"wait", "wait <TYPE[,TYPE...]> [--since N] [--timeout S] [--call ID] [--account AOR] [--param TEXT]",
     "block until a matching event arrives, print it",
     R"(Exit 0 and print the first matching event; exit 1 with {"ok":false,"error":"timeout"}
after S seconds (default 30).

Without --since only events after `wait` connected count. With --since N, events already
journalled after N match too. Use the "seq" of the command you are waiting on, so an event
that fires before `wait` starts is not missed:
  seq=$(leansipctl dial 940 | jq .seq)
  leansipctl wait CALL_ESTABLISHED,CALL_CLOSED --since "$seq" --timeout 30

Types: CALL_INCOMING CALL_RINGING CALL_PROGRESS CALL_ESTABLISHED CALL_CLOSED CALL_DTMF_START
CALL_DTMF_END CALL_AUDIO REGISTER_OK REGISTER_FAIL, and leansipd's BACKEND_UP BACKEND_DOWN
PLAY_STARTED PLAY_ENDED RECORDING_STARTED RECORDING_DONE AUDIO_TAP CALL_LIMIT.)"},
    {"help", "help [command]", "this overview, or details on one command", ""},
};

const char *kOverviewTail = R"(
Global options:
  -p, --profile <name>      which profile of leansip.toml (default: $LEANSIP_PROFILE, then
                            default_profile, then the only one). Each profile has its own daemon.
  --config <file>           default: $LEANSIP_CONFIG or ~/.config/leansip/leansip.toml
  --call <id>               the call a command is for (ids are in events and dial replies)
  -h, --help                this help; `leansipctl <command> --help` for one command

Output: one JSON object per line on stdout. Replies look like
  {"ok":true,"seq":12,"data":...}
where "seq" is the event counter when the command arrived (see `leansipctl help wait`).
Exit codes: 0 ok, 1 failed or timed out, 2 usage error, 3 leansipd not running.

Example:
  leansipctl -p dev2 start --record
  seq=$(leansipctl -p dev2 dial 940 | jq .seq)
  leansipctl -p dev2 wait CALL_ESTABLISHED,CALL_CLOSED --since "$seq"
  leansipctl -p dev2 listen                  # what did the bot say?
  leansipctl -p dev2 say "I'd like to speak to support" --wait
  leansipctl -p dev2 hangup
  leansipctl -p dev2 analyze

Full reference: docs/leansipctl.md in the leansip source tree.)";

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
    QString out = QStringLiteral("leansipctl: control leansipd, the daemon that shares baresip between the leansip\n"
                                 "dialer, scripts and agents.\n\nUsage: leansipctl [-d DIR] <command> [args] [options]\n\nCommands:\n");
    for (const CommandHelp &c : kCommands) {
        const QString synopsis = QString::fromUtf8(c.synopsis).section(u' ', 0, 1);
        out += QStringLiteral("  %1%2\n").arg(synopsis.leftJustified(24), QString::fromUtf8(c.summary));
    }
    return out + QString::fromUtf8(kOverviewTail);
}

QString commandHelp(const CommandHelp &c)
{
    QString out = QStringLiteral("Usage: leansipctl [-d DIR] %1\n\n%2.\n").arg(QString::fromUtf8(c.synopsis), QString::fromUtf8(c.summary));
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

int printReply(const daemon::Reply &reply, QJsonObject extra = {})
{
    extra[QStringLiteral("ok")] = reply.ok;
    extra[QStringLiteral("seq")] = reply.seq;
    extra[QStringLiteral("data")] = dataValue(reply.data);
    printJson(extra);
    return reply.ok ? Ok : Failed;
}

QString callIdFrom(const QString &data)
{
    static const QRegularExpression re(QStringLiteral(R"(call id:\s*(\S+))"));
    const auto m = re.match(data);
    return m.hasMatch() ? m.captured(1) : QString();
}

QString currentCallId(const QString &listcalls)
{
    static const QRegularExpression re(QStringLiteral(R"(^>\s*\[line \d+, id (\S+?)\])"), QRegularExpression::MultilineOption);
    const auto m = re.match(stripAnsi(listcalls));
    return m.hasMatch() ? m.captured(1) : QString();
}

QString compact(const QJsonArray &array)
{
    return QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
}

struct Session {
    daemon::Connection c;
    QString error;

    std::optional<daemon::Reply> request(const QString &command, const QString &params = {}, int timeoutMs = 5000)
    {
        return c.request(command, params, timeoutMs, &error);
    }

    // Runs the steps back to back in leansipd. `results` gets one {command, ok, data} per step
    // that ran; the reply's data is the last of them, `ok` is true when every step succeeded.
    std::optional<daemon::Reply> sequence(const QList<QPair<QString, QString>> &steps, QJsonArray *results = nullptr)
    {
        QJsonArray array;
        for (const auto &s : steps)
            array.append(QJsonArray{s.first, s.second});
        auto reply = request(QStringLiteral("leansipd_seq"), compact(array), 10000);
        if (!reply)
            return reply;
        const QJsonArray r = QJsonDocument::fromJson(reply->data.toUtf8()).array();
        if (results)
            *results = r;
        if (!r.isEmpty())
            reply->data = r.last().toObject().value(QStringLiteral("data")).toString();
        return reply;
    }

    // `command` for `callId` (when given), otherwise for baresip's current call.
    std::optional<daemon::Reply> forCall(const QString &callId, const QString &command, const QString &params = {})
    {
        if (callId.isEmpty())
            return request(command, params, 10000);
        return sequence({{QStringLiteral("callfind"), callId}, {command, params}});
    }

    QJsonArray events(qint64 since)
    {
        const auto reply = request(QStringLiteral("leansipd_events"), QString::number(since));
        return reply && reply->ok ? QJsonDocument::fromJson(reply->data.toUtf8()).array() : QJsonArray{};
    }

    QJsonObject recordings()
    {
        const auto reply = request(QStringLiteral("leansipd_recordings"));
        return reply && reply->ok ? QJsonDocument::fromJson(reply->data.toUtf8()).object() : QJsonObject{};
    }

    QString resolveCall(const QString &given)
    {
        if (!given.isEmpty())
            return given;
        const auto reply = request(QStringLiteral("listcalls"));
        return reply && reply->ok ? currentCallId(reply->data) : QString();
    }
};

struct EventFilter {
    QStringList types;
    QString call;
    QString account;
    QString param;

    bool matches(const QJsonObject &ev) const
    {
        if (!types.isEmpty() && !types.contains(ev.value(QStringLiteral("type")).toString()))
            return false;
        if (!call.isEmpty() && ev.value(QStringLiteral("id")).toString() != call)
            return false;
        if (!account.isEmpty() && ev.value(QStringLiteral("accountaor")).toString() != account)
            return false;
        if (!param.isEmpty() && !ev.value(QStringLiteral("param")).toString().contains(param, Qt::CaseInsensitive))
            return false;
        return true;
    }
};

// Blocks until an event matching `filter` arrives: with since >= 0 also one already journalled
// after `since`, otherwise only a new one.
std::optional<QJsonObject> waitFor(Session &s, const EventFilter &filter, qint64 since, int timeoutMs)
{
    qint64 lastSeen = since;
    if (since >= 0) {
        for (const QJsonValue &v : s.events(since)) {
            const QJsonObject ev = v.toObject();
            lastSeen = std::max(lastSeen, ev.value(QStringLiteral("seq")).toInteger());
            if (filter.matches(ev))
                return ev;
        }
    }
    const QDeadlineTimer deadline(timeoutMs);
    while (!deadline.hasExpired()) {
        const auto ev = s.c.nextEvent(static_cast<int>(qMin<qint64>(deadline.remainingTime(), 1000)));
        if (!ev) {
            if (!s.c.isOpen())
                return std::nullopt;
            continue;
        }
        const qint64 seq = ev->value(QStringLiteral("seq")).toInteger();
        if (seq <= lastSeen)
            continue;
        lastSeen = seq;
        if (filter.matches(*ev))
            return ev;
    }
    return std::nullopt;
}

QString mediaPath(const QString &profileName, const QString &stem)
{
    const QString dir = QDir(profile::runtimeDir(profileName)).filePath(QStringLiteral("media"));
    QDir().mkpath(dir);
    return QDir(dir).filePath(QStringLiteral("%1-%2-%3.wav")
                                  .arg(stem)
                                  .arg(QCoreApplication::applicationPid())
                                  .arg(QDateTime::currentMSecsSinceEpoch()));
}

// Uploads a 16 kHz mono copy (small, and all speech recognition needs).
std::optional<QJsonObject> transcribeFile(const QString &path, const QString &language, QString *error)
{
    const auto config = elevenlabs::Config::fromEnvironment(error);
    if (!config)
        return std::nullopt;
    const auto pcm = wav::readFile(path, error);
    if (!pcm)
        return std::nullopt;
    const QString upload = QDir(QDir::tempPath()).filePath(QStringLiteral("leansipctl-stt-%1.wav").arg(QCoreApplication::applicationPid()));
    if (!wav::writeFile(upload, wav::convert(*pcm, 16000, 1), error))
        return std::nullopt;
    const auto t = elevenlabs::transcribe(*config, upload, error, language);
    QFile::remove(upload);
    if (!t)
        return std::nullopt;
    return QJsonObject{{QStringLiteral("text"), t->text},
                       {QStringLiteral("language"), t->languageCode},
                       {QStringLiteral("languageProbability"), t->languageProbability},
                       {QStringLiteral("words"), t->words}};
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("leansipctl"));
    QCoreApplication::setApplicationVersion(QStringLiteral(LEANSIP_VERSION));

    QCommandLineParser parser;
    parser.addVersionOption();
    auto valueOpt = [](const char *name, const char *desc, const char *value, const QString &def = {}) {
        return QCommandLineOption(QString::fromLatin1(name), QString::fromLatin1(desc), QString::fromLatin1(value), def);
    };
    const QCommandLineOption helpOpt({QStringLiteral("h"), QStringLiteral("help")}, QStringLiteral("help"));
    const QCommandLineOption profileOpt({QStringLiteral("p"), QStringLiteral("profile")},
                                        QStringLiteral("profile (selects the account/daemon)"), QStringLiteral("name"));
    const QCommandLineOption configOpt = valueOpt("config", "config file", "file", settings::defaultPath());
    const QCommandLineOption yesOpt(QStringLiteral("yes"), QStringLiteral("dial on a confirm_dial profile"));
    const QCommandLineOption sinceOpt = valueOpt("since", "event sequence number", "seq");
    const QCommandLineOption timeoutOpt = valueOpt("timeout", "seconds", "s", QStringLiteral("30"));
    const QCommandLineOption followOpt(QStringLiteral("follow"), QStringLiteral("keep streaming events"));
    const QCommandLineOption traceOpt(QStringLiteral("sip-trace"), QStringLiteral("start: log SIP messages"));
    const QCommandLineOption recordOpt(QStringLiteral("record"), QStringLiteral("start: record calls"));
    const QCommandLineOption maxCallOpt = valueOpt("max-call-seconds", "start: call limit", "s");
    const QCommandLineOption callOpt = valueOpt("call", "call id", "id");
    const QCommandLineOption accountOpt = valueOpt("account", "filter: account aor", "aor");
    const QCommandLineOption paramOpt = valueOpt("param", "filter: param substring", "text");
    const QCommandLineOption waitOpt(QStringLiteral("wait"), QStringLiteral("say/play: wait until played"));
    const QCommandLineOption outOpt = valueOpt("out", "keep audio here", "file");
    const QCommandLineOption languageOpt = valueOpt("language", "STT language code", "code");
    const QCommandLineOption silenceOpt = valueOpt("silence", "listen: ms of quiet that ends speech", "ms", QStringLiteral("1200"));
    const QCommandLineOption maxOpt = valueOpt("max", "listen: max utterance seconds", "s", QStringLiteral("30"));
    const QCommandLineOption thresholdOpt = valueOpt("threshold", "listen: speech level dBFS", "db", QStringLiteral("-45"));
    const QCommandLineOption noSttOpt(QStringLiteral("no-stt"), QStringLiteral("listen: skip transcription"));
    parser.addOptions({helpOpt, profileOpt, configOpt, yesOpt, sinceOpt, timeoutOpt, followOpt, traceOpt, recordOpt, maxCallOpt, callOpt,
                       accountOpt, paramOpt, waitOpt, outOpt, languageOpt, silenceOpt, maxOpt, thresholdOpt, noSttOpt});
    if (!parser.parse(QCoreApplication::arguments())) {
        printText(QStringLiteral("leansipctl: %1\nRun `leansipctl help` for usage.").arg(parser.errorText()), stderr);
        return Usage;
    }
    if (parser.isSet(QStringLiteral("version"))) {
        printText(QStringLiteral("leansipctl %1").arg(QCoreApplication::applicationVersion()));
        return Ok;
    }

    const QStringList args = parser.positionalArguments();
    // `leansipctl help [cmd]`, `leansipctl --help`, `leansipctl <cmd> --help`
    const bool helpCommand = !args.isEmpty() && args.first() == QLatin1String("help");
    if (helpCommand || parser.isSet(helpOpt)) {
        const QString topic = helpCommand ? args.value(1) : args.value(0);
        if (topic.isEmpty()) {
            printText(overview());
            return Ok;
        }
        const CommandHelp *c = findCommand(topic);
        if (!c) {
            printText(QStringLiteral("leansipctl: no command '%1'\n\n%2").arg(topic, overview()), stderr);
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
    if (!findCommand(command) && command != QLatin1String("start"))
        return fail(QStringLiteral("unknown command '%1'; see `leansipctl help`").arg(command), Usage);

    // transcribe needs neither a profile nor a daemon.
    if (command == QLatin1String("transcribe")) {
        if (rest.size() != 1)
            return fail(QStringLiteral("usage: leansipctl transcribe <file.wav>"), Usage);
        QString error;
        const auto t = transcribeFile(rest.first(), parser.value(languageOpt), &error);
        if (!t)
            return fail(error);
        QJsonObject out = *t;
        out[QStringLiteral("ok")] = true;
        printJson(out);
        return Ok;
    }

    const QString configPath = QDir(parser.value(configOpt)).absolutePath();
    QString configError;
    const auto tomlFile = settings::load(configPath, &configError);
    if (!tomlFile)
        return fail(configError, Usage);
    if (command == QLatin1String("profiles")) {
        QJsonArray list;
        for (const settings::Profile &p : tomlFile->profiles) {
            list.append(QJsonObject{{QStringLiteral("name"), p.name},
                                    {QStringLiteral("aor"), p.aor},
                                    {QStringLiteral("displayName"), p.displayName},
                                    {QStringLiteral("label"), p.label},
                                    {QStringLiteral("shortcut"), p.shortcut >= 0 ? QJsonValue(p.shortcut) : QJsonValue()},
                                    {QStringLiteral("confirmDial"), p.confirmDial},
                                    {QStringLiteral("running"), daemon::isRunning(profile::socketPath(p.name), 200)}});
        }
        printJson({{QStringLiteral("ok"), true},
                   {QStringLiteral("config"), configPath},
                   {QStringLiteral("default"), tomlFile->defaultProfile},
                   {QStringLiteral("profiles"), list}});
        return Ok;
    }
    const auto selected = settings::select(*tomlFile, parser.value(profileOpt), &configError);
    if (!selected)
        return fail(configError, Usage);
    const settings::Profile &prof = *selected;
    const QString socketPath = profile::socketPath(prof.name);
    auto dialAllowed = [&] { return !prof.confirmDial || parser.isSet(yesOpt); };
    const QString confirmHint =
        QStringLiteral("profile %1 has confirm_dial = true (%2); add --yes to really place this call")
            .arg(prof.name, prof.aor);
    const QString callArg = parser.value(callOpt);

    bool sinceOk = true;
    const qint64 since = parser.isSet(sinceOpt) ? parser.value(sinceOpt).toLongLong(&sinceOk) : -1;
    if (!sinceOk)
        return fail(QStringLiteral("--since expects a number"), Usage);
    bool timeoutOk = false;
    const double timeoutSeconds = parser.value(timeoutOpt).toDouble(&timeoutOk);
    if (!timeoutOk || timeoutSeconds < 0)
        return fail(QStringLiteral("--timeout expects seconds"), Usage);
    const int timeoutMs = static_cast<int>(timeoutSeconds * 1000);

    if (command == QLatin1String("start")) {
        QStringList daemonArgs;
        if (parser.isSet(traceOpt))
            daemonArgs << QStringLiteral("--sip-trace");
        if (parser.isSet(recordOpt))
            daemonArgs << QStringLiteral("--record");
        if (parser.isSet(maxCallOpt))
            daemonArgs << QStringLiteral("--max-call-seconds") << parser.value(maxCallOpt);
        QString error;
        bool started = false;
        if (!daemon::ensureRunning(prof.name, configPath, daemonArgs, &error, &started))
            return fail(error);
        Session s;
        if (!s.c.open(socketPath, 2000, &s.error))
            return fail(s.error, NotRunning);
        // Return only once baresip is reachable, so the next command does not race its startup.
        std::optional<daemon::Reply> status;
        const QDeadlineTimer upDeadline(5000);
        for (;;) {
            status = s.request(QStringLiteral("leansipd_status"));
            if (!status)
                return fail(s.error);
            if (dataValue(status->data).toObject().value(QStringLiteral("baresipUp")).toBool() || upDeadline.hasExpired())
                break;
            QThread::msleep(100);
        }
        const QJsonObject st = dataValue(status->data).toObject();
        QJsonObject out{{QStringLiteral("ok"), true}, {QStringLiteral("started"), started}, {QStringLiteral("status"), st}};
        QStringList warnings;
        if (!st.value(QStringLiteral("baresipUp")).toBool())
            warnings << QStringLiteral("baresip is not up yet; check the log");
        if (!started && ((parser.isSet(traceOpt) && !st.value(QStringLiteral("sipTrace")).toBool())
                         || (parser.isSet(recordOpt) && !st.value(QStringLiteral("recording")).toBool())
                         || (parser.isSet(maxCallOpt)
                             && st.value(QStringLiteral("maxCallSeconds")).toInt() != parser.value(maxCallOpt).toInt())))
            warnings << QStringLiteral("already running with other options; `leansipctl stop` first to change them");
        if (!warnings.isEmpty())
            out[QStringLiteral("warning")] = warnings.join(QStringLiteral("; "));
        if (st.value(QStringLiteral("uiOwned")).toBool())
            out[QStringLiteral("note")] = QStringLiteral("this daemon belongs to the open leansip dialer and stops when it closes");
        printJson(out);
        return Ok;
    }

    Session s;
    if (!QFileInfo::exists(socketPath))
        s.error = QStringLiteral("leansipd is not running for profile %1").arg(prof.name);
    if (!s.error.isEmpty() || !s.c.open(socketPath, 1000, &s.error)) {
        if (command == QLatin1String("status")) {
            printJson({{QStringLiteral("ok"), true}, {QStringLiteral("running"), false}, {QStringLiteral("socket"), socketPath}});
            return NotRunning;
        }
        return fail(s.error + QStringLiteral("; run `leansipctl -p %1 start` first").arg(prof.name), NotRunning);
    }
    auto replyOrFail = [&](const std::optional<daemon::Reply> &reply, QJsonObject extra = {}) {
        return reply ? printReply(*reply, std::move(extra)) : fail(s.error);
    };

    if (command == QLatin1String("status"))
        return replyOrFail(s.request(QStringLiteral("leansipd_status")));
    if (command == QLatin1String("stop")) {
        const auto status = s.request(QStringLiteral("leansipd_status"));
        const qint64 pid = status ? dataValue(status->data).toObject().value(QStringLiteral("pid")).toInteger() : 0;
        const auto reply = s.request(QStringLiteral("leansipd_shutdown"));
        if (!reply || !reply->ok)
            return replyOrFail(reply);
        // The daemon exits after its baresip has, so its SIP port is free once it is gone.
        const QDeadlineTimer deadline(10000);
        while (daemon::processAlive(pid) && !deadline.hasExpired())
            QThread::msleep(50);
        if (daemon::processAlive(pid))
            return fail(QStringLiteral("leansipd (pid %1) did not exit within 10 s").arg(pid));
        return printReply(*reply);
    }
    if (command == QLatin1String("calls"))
        return replyOrFail(s.request(QStringLiteral("listcalls")));
    if (command == QLatin1String("reginfo"))
        return replyOrFail(s.request(QStringLiteral("reginfo")));
    if (command == QLatin1String("recordings"))
        return replyOrFail(s.request(QStringLiteral("leansipd_recordings")));

    if (command == QLatin1String("answer") || command == QLatin1String("hangup") || command == QLatin1String("mute")
        || command == QLatin1String("hold")) {
        const QString baresipCommand = command == QLatin1String("answer") ? QStringLiteral("accept") : command;
        return replyOrFail(s.forCall(callArg, baresipCommand));
    }
    if (command == QLatin1String("resume")) {
        auto reply = s.forCall(callArg, QStringLiteral("resume"));
        if (!reply)
            return fail(s.error);
        if (!reply->ok && reply->data.contains(QLatin1String("Invalid argument"))) {
            // baresip 1.1.0 reports this even when it resumed; check the call list instead.
            QThread::msleep(300);
            const auto calls = s.request(QStringLiteral("listcalls"));
            if (calls && calls->ok) {
                const QString id = callArg.isEmpty() ? currentCallId(calls->data) : callArg;
                bool held = false;
                for (const QString &line : stripAnsi(calls->data).split(u'\n'))
                    held = held || (!id.isEmpty() && line.contains(id) && line.contains(QLatin1String("on hold")));
                if (!held && !id.isEmpty()) {
                    reply->ok = true;
                    reply->data = QStringLiteral("resumed");
                }
            }
        }
        return printReply(*reply);
    }

    if (command == QLatin1String("dial")) {
        if (rest.size() != 1)
            return fail(QStringLiteral("usage: leansipctl dial <number|sip-uri> [--yes]"), Usage);
        QString target = rest.first().trimmed();
        if (target.startsWith(QLatin1String("tel:"), Qt::CaseInsensitive))
            target = target.mid(4);
        // Same whitelist as the dialer: the string goes verbatim to baresip's command parser.
        if (!CallController::isValidNumber(target))
            return fail(QStringLiteral("invalid number or URI '%1'").arg(target), Usage);
        if (!dialAllowed())
            return fail(confirmHint, Usage);
        const auto reply = s.request(QStringLiteral("dial"), target, 10000);
        if (!reply)
            return fail(s.error);
        QJsonObject extra;
        if (const QString id = callIdFrom(reply->data); !id.isEmpty())
            extra[QStringLiteral("call")] = id;
        return printReply(*reply, extra);
    }

    if (command == QLatin1String("dtmf")) {
        static const QRegularExpression digitsRe(QStringLiteral("^[0-9*#A-D]+$"));
        if (rest.size() != 1 || !digitsRe.match(rest.first()).hasMatch())
            return fail(QStringLiteral("usage: leansipctl dtmf <digits 0-9*#A-D> [--call ID]"), Usage);
        // One digit at a time, spaced so each gets its own RFC 4733 event.
        const QString digits = rest.first();
        qint64 seq = 0;
        for (qsizetype i = 0; i < digits.size(); ++i) {
            if (i > 0)
                QThread::msleep(200);
            const auto reply = s.forCall(callArg, QStringLiteral("sndcode"), QString(digits.at(i)));
            if (!reply)
                return fail(s.error);
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
            return fail(QStringLiteral("usage: leansipctl raw <command> [params...]"), Usage);
        if (rest.first() == QLatin1String("leansipd_hello") || rest.first() == QLatin1String("leansipd_bind"))
            return fail(QStringLiteral("%1 is reserved for the dialer").arg(rest.first()), Usage);
        // dial, dialdir, uadial, ... and sequences containing them.
        if (rest.join(u' ').contains(QLatin1String("dial"), Qt::CaseInsensitive) && !dialAllowed())
            return fail(confirmHint, Usage);
        return replyOrFail(s.forCall(callArg, rest.first(), rest.mid(1).join(u' ')));
    }

    if (command == QLatin1String("say") || command == QLatin1String("play")) {
        const bool say = command == QLatin1String("say");
        const QString usage = say ? QStringLiteral("usage: leansipctl say <text> [--call ID] [--wait]")
                                  : QStringLiteral("usage: leansipctl play <file.wav> [--call ID] [--wait]");
        if (rest.isEmpty() || (!say && rest.size() != 1))
            return fail(usage, Usage);
        QString file;
        bool temporary = false;
        if (say) {
            QString error;
            const auto config = elevenlabs::Config::fromEnvironment(&error);
            if (!config)
                return fail(error);
            const auto pcm = elevenlabs::synthesize(*config, rest.join(u' '), &error);
            if (!pcm)
                return fail(error);
            temporary = !parser.isSet(outOpt);
            file = temporary ? mediaPath(prof.name, QStringLiteral("say")) : QFileInfo(parser.value(outOpt)).absoluteFilePath();
            if (!wav::writeFile(file, wav::fromRaw(*pcm, elevenlabs::kTtsSampleRate, 1), &error))
                return fail(error);
        } else {
            file = QFileInfo(rest.first()).absoluteFilePath();
        }
        const QJsonObject req{{QStringLiteral("file"), file}, {QStringLiteral("call"), callArg}};
        const auto reply = s.request(QStringLiteral("leansipd_play"),
                                     QString::fromUtf8(QJsonDocument(req).toJson(QJsonDocument::Compact)), 10000);
        if (temporary)
            QFile::remove(file); // leansipd has made its own converted copy by the time it replies
        if (!reply)
            return fail(s.error);
        if (!reply->ok)
            return printReply(*reply);
        QJsonObject out = dataValue(reply->data).toObject();
        out[QStringLiteral("ok")] = true;
        out[QStringLiteral("seq")] = reply->seq;
        out[QStringLiteral("call")] = out.take(QStringLiteral("id"));
        if (say && !temporary)
            out[QStringLiteral("audio")] = file;
        if (parser.isSet(waitOpt)) {
            const double seconds = out.value(QStringLiteral("seconds")).toDouble();
            const EventFilter f{{QStringLiteral("PLAY_ENDED")}, out.value(QStringLiteral("call")).toString(), {}, {}};
            const auto ended = waitFor(s, f, reply->seq, static_cast<int>(seconds * 1000) + 10000);
            if (!ended)
                return fail(QStringLiteral("playback did not report its end"));
            out[QStringLiteral("ended")] = ended->value(QStringLiteral("param"));
        }
        printJson(out);
        return Ok;
    }

    if (command == QLatin1String("listen")) {
        if (!parser.isSet(noSttOpt)) {
            QString error;
            if (!elevenlabs::Config::fromEnvironment(&error))
                return fail(error + QStringLiteral(" (or pass --no-stt)"));
        }
        bool ok1 = false, ok2 = false, ok3 = false;
        SpeechSegmenter::Params params;
        params.silenceMs = parser.value(silenceOpt).toInt(&ok1);
        params.maxMs = static_cast<int>(parser.value(maxOpt).toDouble(&ok2) * 1000);
        params.thresholdDbfs = parser.value(thresholdOpt).toDouble(&ok3);
        if (!ok1 || !ok2 || !ok3 || params.silenceMs <= 0 || params.maxMs <= 0)
            return fail(QStringLiteral("--silence, --max and --threshold expect numbers"), Usage);

        const auto status = s.request(QStringLiteral("leansipd_status"));
        if (status && !dataValue(status->data).toObject().value(QStringLiteral("recording")).toBool())
            return fail(QStringLiteral("recording is off: listen needs a daemon started with --record "
                                       "(`leansipctl stop`, then `leansipctl start --record`)"));
        const QString callId = s.resolveCall(callArg);
        if (callId.isEmpty())
            return fail(QStringLiteral("no call to listen to"));
        // The recording appears once the call's audio starts; give it a moment.
        QString received;
        const QDeadlineTimer recDeadline(3000);
        for (;;) {
            received = s.recordings().value(callId).toObject().value(QStringLiteral("received")).toString();
            if (!received.isEmpty() || recDeadline.hasExpired())
                break;
            QThread::msleep(100);
        }
        if (received.isEmpty())
            return fail(QStringLiteral("no recording for call %1 yet (is its audio running?)").arg(callId));

        QFile f(received);
        if (!f.open(QIODevice::ReadOnly))
            return fail(QStringLiteral("cannot open %1: %2").arg(received, f.errorString()));
        QString error;
        const auto header = wav::readHeader(f, f.size(), &error);
        if (!header || header->bitsPerSample != 16 || header->channels < 1)
            return fail(error.isEmpty() ? QStringLiteral("unexpected recording format") : error);
        const qint64 bytesPerFrame = 2LL * header->channels;
        const qint64 frameLen = static_cast<qint64>(header->sampleRate) * params.frameMs / 1000; // per channel, 20 ms
        const qint64 chunkBytes = bytesPerFrame * frameLen;
        // From now on: start at the current end of the file, on a 20 ms boundary.
        qint64 pos = header->dataOffset + ((f.size() - header->dataOffset) / chunkBytes) * chunkBytes;

        SpeechSegmenter seg(params);
        wav::Pcm heard;
        heard.sampleRate = header->sampleRate;
        heard.channels = header->channels;
        const EventFilter closedFilter{{QStringLiteral("CALL_CLOSED")}, callId, {}, {}};
        const QDeadlineTimer waitDeadline(timeoutMs);
        bool callClosed = false;
        while (seg.state() != SpeechSegmenter::State::Done) {
            const qint64 chunk = ((f.size() - pos) / chunkBytes) * chunkBytes;
            if (chunk > 0) {
                f.seek(pos);
                const wav::Pcm part = wav::fromRaw(f.read(chunk), header->sampleRate, header->channels);
                pos += chunk;
                heard.samples += part.samples;
                for (double level : wav::frameLevels(part, params.frameMs)) {
                    if (seg.feed(level) == SpeechSegmenter::State::Done)
                        break;
                }
                continue;
            }
            if (seg.state() == SpeechSegmenter::State::Waiting && waitDeadline.hasExpired())
                return fail(QStringLiteral("no speech within %1 s").arg(timeoutSeconds));
            if (callClosed)
                break;
            // Poll the file every 50 ms; notice the call closing meanwhile.
            if (const auto ev = s.c.nextEvent(50); ev && closedFilter.matches(*ev))
                callClosed = true;
        }
        if (seg.state() == SpeechSegmenter::State::Waiting)
            return fail(QStringLiteral("the call ended before anything was said"));
        const qint64 endFrame = seg.state() == SpeechSegmenter::State::Done ? seg.endFrame() : seg.framesSeen();
        wav::Pcm utterance;
        utterance.sampleRate = heard.sampleRate;
        utterance.channels = heard.channels;
        const qint64 from = seg.startFrame() * frameLen * heard.channels;
        const qint64 to = std::min<qint64>(endFrame * frameLen * heard.channels, heard.samples.size());
        utterance.samples = heard.samples.mid(from, to - from);
        const wav::Pcm mono = wav::convert(utterance, 16000, 1);
        const QString out = parser.isSet(outOpt) ? QFileInfo(parser.value(outOpt)).absoluteFilePath()
                                                 : mediaPath(prof.name, QStringLiteral("heard"));
        if (!wav::writeFile(out, mono, &error))
            return fail(error);
        QJsonObject result{{QStringLiteral("ok"), true},
                           {QStringLiteral("call"), callId},
                           {QStringLiteral("seconds"), std::round(mono.seconds() * 10) / 10},
                           {QStringLiteral("audio"), out},
                           {QStringLiteral("truncated"), seg.truncated()},
                           {QStringLiteral("callClosed"), callClosed}};
        if (!parser.isSet(noSttOpt)) {
            const auto t = transcribeFile(out, parser.value(languageOpt), &error);
            if (!t)
                return fail(error);
            result[QStringLiteral("text")] = t->value(QStringLiteral("text"));
            result[QStringLiteral("language")] = t->value(QStringLiteral("language"));
        }
        printJson(result);
        return Ok;
    }

    if (command == QLatin1String("analyze")) {
        const QJsonArray events = s.events(0);
        const QString callId = callArg.isEmpty() ? analysis::lastCallId(events) : callArg;
        if (callId.isEmpty())
            return fail(QStringLiteral("no call in the event journal"));
        std::optional<wav::Levels> sent, received;
        const QJsonObject files = s.recordings().value(callId).toObject();
        QString error;
        if (const QString p = files.value(QStringLiteral("sent")).toString(); !p.isEmpty()) {
            if (const auto pcm = wav::readFile(p, &error))
                sent = wav::measure(*pcm);
        }
        if (const QString p = files.value(QStringLiteral("received")).toString(); !p.isEmpty()) {
            if (const auto pcm = wav::readFile(p, &error))
                received = wav::measure(*pcm);
        }
        QJsonObject result = analysis::analyzeCall(events, callId, sent, received);
        if (!result.value(QStringLiteral("found")).toBool())
            return fail(QStringLiteral("call %1 is not in the event journal").arg(callId));
        result.remove(QStringLiteral("found"));
        result[QStringLiteral("ok")] = true;
        if (!files.isEmpty())
            result[QStringLiteral("recordings")] = files;
        printJson(result);
        return Ok;
    }

    if (command == QLatin1String("events") || command == QLatin1String("wait")) {
        EventFilter filter{{}, callArg, parser.value(accountOpt), parser.value(paramOpt)};
        if (command == QLatin1String("wait")) {
            if (rest.size() != 1)
                return fail(QStringLiteral("usage: leansipctl wait <TYPE[,TYPE...]> [--since N] [--timeout S]"), Usage);
            filter.types = rest.first().split(u',', Qt::SkipEmptyParts);
            const auto ev = waitFor(s, filter, since, timeoutMs);
            if (ev) {
                printJson(*ev);
                return Ok;
            }
            if (!s.c.isOpen())
                return fail(QStringLiteral("leansipd closed the connection"));
            printJson({{QStringLiteral("ok"), false}, {QStringLiteral("error"), QStringLiteral("timeout")},
                       {QStringLiteral("waitedFor"), QJsonArray::fromStringList(filter.types)}});
            return Failed;
        }
        qint64 lastSeen = std::max<qint64>(since, 0);
        for (const QJsonValue &v : s.events(lastSeen)) {
            const QJsonObject ev = v.toObject();
            lastSeen = std::max(lastSeen, ev.value(QStringLiteral("seq")).toInteger());
            if (filter.matches(ev))
                printJson(ev);
        }
        if (!parser.isSet(followOpt))
            return Ok;
        for (;;) {
            const auto ev = s.c.nextEvent(1000);
            if (!ev) {
                if (!s.c.isOpen())
                    return fail(QStringLiteral("leansipd closed the connection"));
                continue;
            }
            const qint64 seq = ev->value(QStringLiteral("seq")).toInteger();
            if (seq <= lastSeen)
                continue;
            lastSeen = seq;
            if (filter.matches(*ev))
                printJson(*ev);
        }
    }

    return fail(QStringLiteral("unknown command '%1'; see `leansipctl help`").arg(command), Usage);
}
