// leansipd: owns one baresip for one leansip.toml profile and shares it, through CtrlProxy,
// with the dialer and with command-line/agent clients (leansipctl).

#include "core/baresip_client.h"
#include "core/baresip_process.h"
#include "core/ctrl_proxy.h"
#include "core/daemon_client.h"
#include "core/log_file.h"
#include "core/media_control.h"
#include "core/profile.h"
#include "core/settings.h"
#include "core/unix_signals.h"
#include "core/wav.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>

#include <csignal>
#include <cstdio>
#include <iterator>
#include <memory>

using namespace leansip;

namespace {

// A daemon the dialer started gives up if the dialer never binds to it (it died while starting).
constexpr int kBindGraceMs = 10000;
// Restart delays after baresip exits unexpectedly; the series restarts once it has run a minute.
constexpr int kRestartDelaysMs[] = {1000, 2000, 5000, 10000, 30000};
constexpr int kStableMs = 60000;
// Silence after a clip, so baresip's aufile never reaches the end of the file: at EOF the menu
// module treats it as an audio error and hangs up the call. The source is switched back first.
constexpr double kPlayPaddingSeconds = 3.0;

LogFile *g_log = nullptr;

void logMessage(QtMsgType, const QMessageLogContext &context, const QString &message)
{
    const QString line = context.category && qstrcmp(context.category, "default") != 0
                             ? QStringLiteral("%1: %2").arg(QLatin1String(context.category), message)
                             : message;
    if (g_log)
        g_log->writeLine(line);
    else
        std::fprintf(stderr, "%s\n", qPrintable(line));
}

QString compact(const QJsonObject &obj)
{
    return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

// "> [line 1, id 813e30009de71764]  0:00:17  ESTABLISHED ..." marks baresip's current call.
QString currentCallId(const QString &listcalls)
{
    static const QRegularExpression re(QStringLiteral(R"(^>\s*\[line \d+, id (\S+?)\])"), QRegularExpression::MultilineOption);
    const auto m = re.match(listcalls);
    return m.hasMatch() ? m.captured(1) : QString();
}

QString callIdFromDialReply(const QString &data)
{
    static const QRegularExpression re(QStringLiteral(R"(call id:\s*(\S+))"));
    const auto m = re.match(data);
    return m.hasMatch() ? m.captured(1) : QString();
}

// Reads the profile again and fetches its password: what baresip runs with.
std::optional<config_deriver::Setup> loadSetup(const QString &configPath, const QString &name,
                                               settings::Profile *profileOut, QString *error)
{
    const auto file = settings::load(configPath, error);
    if (!file)
        return std::nullopt;
    const auto profile = settings::select(*file, name, error);
    if (!profile)
        return std::nullopt;
    const auto password = settings::password(*profile, error);
    if (!password)
        return std::nullopt;
    const auto line = settings::accountLine(*profile, *password, error);
    if (!line)
        return std::nullopt;
    if (profileOut)
        *profileOut = *profile;
    return config_deriver::Setup{settings::baresipConfig(profile->baresip), *line, profile->uuid};
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("leansipd"));
    QCoreApplication::setApplicationVersion(QStringLiteral(LEANSIP_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Shares one baresip between the leansip dialer and leansipctl"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption profileOpt({QStringLiteral("p"), QStringLiteral("profile")},
                                        QStringLiteral("Profile in the config file (default: its default_profile)"),
                                        QStringLiteral("name"));
    const QCommandLineOption configOpt(QStringLiteral("config"),
                                       QStringLiteral("Config file (default: $LEANSIP_CONFIG or ~/.config/leansip/leansip.toml)"),
                                       QStringLiteral("file"), settings::defaultPath());
    const QCommandLineOption uiOpt(QStringLiteral("ui-owned"),
                                   QStringLiteral("Started by the dialer: quit (stopping baresip) when the client that "
                                                  "sends leansipd_bind disconnects, even if other clients remain"));
    const QCommandLineOption traceOpt(QStringLiteral("sip-trace"),
                                      QStringLiteral("Run baresip with -s: SIP messages go to the log"));
    const QCommandLineOption recordOpt(QStringLiteral("record"),
                                       QStringLiteral("Record every call, both directions, in the profile's record_dir "
                                                      "(default <runtime dir>/recordings); record = true does the same"));
    const QCommandLineOption maxCallOpt(QStringLiteral("max-call-seconds"),
                                        QStringLiteral("Hang up calls that leansipctl clients placed or answered after this "
                                                       "long (0: no limit; default 600). The dialer's calls are exempt."),
                                        QStringLiteral("s"), QStringLiteral("600"));
    parser.addOptions({profileOpt, configOpt, uiOpt, traceOpt, recordOpt, maxCallOpt});
    parser.process(app);

    const QString configPath = QDir(parser.value(configOpt)).absolutePath();
    QString error;
    // Errors before the log is open go to stderr, which ensureRunning points at the log.
    const auto tomlFile = settings::load(configPath, &error);
    const auto selected = tomlFile ? settings::select(*tomlFile, parser.value(profileOpt), &error) : std::nullopt;
    if (!selected) {
        std::fprintf(stderr, "leansipd: %s\n", qPrintable(error));
        return 1;
    }
    settings::Profile prof = *selected;
    const QString profileName = prof.name;
    const QString runtimeDir = profile::runtimeDir(profileName);
    const QString socketPath = profile::socketPath(profileName);
    const QString mediaDir = QDir(runtimeDir).filePath(QStringLiteral("media"));
    const QString recordingsDir =
        prof.recordDir.isEmpty() ? QDir(runtimeDir).filePath(QStringLiteral("recordings")) : prof.recordDir;
    const bool uiOwned = parser.isSet(uiOpt);
    const bool recording = parser.isSet(recordOpt) || prof.record;
    bool maxOk = false;
    const int maxCallSeconds = parser.value(maxCallOpt).toInt(&maxOk);
    if (!maxOk || maxCallSeconds < 0) {
        std::fputs("leansipd: --max-call-seconds expects a number >= 0\n", stderr);
        return 2;
    }

    for (const QString &dir : {runtimeDir, mediaDir}) {
        if (!QDir().mkpath(dir)) {
            std::fprintf(stderr, "leansipd: cannot create %s\n", qPrintable(dir));
            return 1;
        }
        QFile::setPermissions(dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    }

    // Exactly one daemon per profile. A socket nobody answers on is stale.
    if (daemon::isRunning(socketPath, 500)) {
        std::fprintf(stderr, "leansipd: already running for profile %s\n", qPrintable(profileName));
        return 0;
    }
    QLocalServer::removeServer(socketPath);

    LogFile log(profile::logPath(profileName));
    if (log.open(&error)) {
        g_log = &log;
        qInstallMessageHandler(logMessage);
    } else {
        std::fprintf(stderr, "leansipd: %s; logging to stderr\n", qPrintable(error));
    }

    // Before listening: clients wait for the socket, or for us to exit, while password_cmd runs.
    auto setup = loadSetup(configPath, profileName, &prof, &error);
    if (!setup) {
        qCritical("leansipd: %s", qPrintable(error));
        return 1;
    }

    BaresipClient upstream;
    CtrlProxy proxy(&upstream);
    if (!proxy.listen(socketPath, &error)) {
        qCritical("leansipd: %s", qPrintable(error));
        return 1;
    }

    BaresipProcess process;
    process.setDerivedDir(runtimeDir);
    process.setCaptureOutput(true);
    QObject::connect(&process, &BaresipProcess::outputReceived, &app, [&](const QByteArray &data) { log.write(data); });
    if (parser.isSet(traceOpt))
        process.setExtraArgs({QStringLiteral("-s")});
    std::unique_ptr<RecordingTracker> recordings;
    if (recording) {
        const QString incoming = QDir(recordingsDir).filePath(QStringLiteral(".incoming"));
        if (!QDir().mkpath(incoming)) {
            qCritical("leansipd: cannot create %s", qPrintable(incoming));
            return 1;
        }
        QFile::setPermissions(recordingsDir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        process.setRecordDir(incoming);
        recordings = std::make_unique<RecordingTracker>(incoming, recordingsDir);
    }
    if (!process.start(*setup)) {
        qCritical("leansipd: cannot start baresip: %s", qPrintable(process.lastError()));
        return 1;
    }
    upstream.connectToHost(process.host(), process.port());

    // --- keep baresip running ---------------------------------------------------------------
    int restartAttempt = 0;
    QTimer restartTimer;
    restartTimer.setSingleShot(true);
    QTimer stableTimer;
    stableTimer.setSingleShot(true);
    QObject::connect(&stableTimer, &QTimer::timeout, &app, [&] { restartAttempt = 0; });
    QObject::connect(&restartTimer, &QTimer::timeout, &app, [&] {
        qInfo("leansipd: restarting baresip (attempt %d)", restartAttempt);
        if (process.restart())
            upstream.connectToHost(process.host(), process.port());
        else
            qWarning("leansipd: baresip restart failed: %s", qPrintable(process.lastError()));
    });
    QObject::connect(&process, &BaresipProcess::finished, &app, [&](int code, QProcess::ExitStatus) {
        stableTimer.stop();
        proxy.setBackendUp(false, QStringLiteral("baresip exited (code %1)").arg(code));
        const int delay = kRestartDelaysMs[std::min<size_t>(static_cast<size_t>(restartAttempt), std::size(kRestartDelaysMs) - 1)];
        ++restartAttempt;
        restartTimer.start(delay);
    });
    QObject::connect(&upstream, &BaresipClient::connected, &app, [&] { stableTimer.start(kStableMs); });

    // --- call time limit for leansipctl clients ----------------------------------------------------
    // Calls the dialer placed or answered; everything else is an agent's and gets the time limit.
    QSet<QString> uiCalls;
    QSet<QString> ringing;
    QHash<QString, QTimer *> limitTimers;
    proxy.setResponseObserver([&](bool fromUi, const QString &command, const QString &params, bool ok, const QString &data) {
        if (!fromUi || !ok)
            return;
        if (command == QLatin1String("dial") || command == QLatin1String("dialdir")) {
            if (const QString id = callIdFromDialReply(data); !id.isEmpty())
                uiCalls.insert(id);
        } else if (command == QLatin1String("callfind")) {
            uiCalls.insert(params.trimmed());
        } else if (command == QLatin1String("accept") && ringing.size() == 1) {
            uiCalls.insert(*ringing.cbegin());
        }
    });

    // --- playback ---------------------------------------------------------------------------
    struct Playback {
        QString callId;
        QString restoreSource;
        QString file;
        QTimer *timer = nullptr; // deleteLater: it may be the sender that ends the playback
    };
    std::unique_ptr<Playback> playing;
    int playCounter = 0;
    auto endPlayback = [&](const QString &reason) {
        if (!playing)
            return;
        std::unique_ptr<Playback> p = std::move(playing);
        p->timer->stop();
        p->timer->deleteLater();
        QList<CtrlProxy::Step> steps;
        if (reason.isEmpty())
            steps.append({QStringLiteral("callfind"), p->callId});
        steps.append({QStringLiteral("ausrc"), p->restoreSource});
        const QString callId = p->callId;
        const QString file = p->file;
        proxy.runSequence(steps, [&proxy, callId, file, reason](const QList<CtrlProxy::StepResult> &results) {
            const bool restored = !results.isEmpty() && results.last().ok && results.size() == (reason.isEmpty() ? 2 : 1);
            if (!restored)
                qWarning("leansipd: could not switch the audio source back after playback");
            QFile::remove(file);
            proxy.publishEvent({{QStringLiteral("class"), QStringLiteral("leansipd")},
                                {QStringLiteral("type"), QStringLiteral("PLAY_ENDED")},
                                {QStringLiteral("id"), callId},
                                {QStringLiteral("param"), reason.isEmpty() ? QStringLiteral("done") : reason}});
        });
    };

    // leansipd_play {"file": "/abs/clip.wav", "call": "<id or empty>"}
    proxy.addAsyncCommand(QStringLiteral("leansipd_play"), [&](const QString &params, CtrlProxy::Reply reply) {
        const QJsonObject req = QJsonDocument::fromJson(params.toUtf8()).object();
        const QString file = req.value(QStringLiteral("file")).toString();
        const QString wantedCall = req.value(QStringLiteral("call")).toString();
        if (playing) {
            reply(false, QStringLiteral("already playing into call %1").arg(playing->callId));
            return;
        }
        QString err;
        const auto clip = wav::readFile(file, &err);
        if (!clip) {
            reply(false, err);
            return;
        }
        QList<CtrlProxy::Step> probe;
        if (!wantedCall.isEmpty())
            probe.append({QStringLiteral("callfind"), wantedCall});
        probe.append({QStringLiteral("listcalls"), {}});
        probe.append({QStringLiteral("audio_debug"), {}});
        proxy.runSequence(probe, [&, clip = *clip, reply, wantedCall, probeSize = probe.size()](const QList<CtrlProxy::StepResult> &results) {
            if (results.size() != probeSize || !results.last().ok) {
                reply(false, results.isEmpty() ? QStringLiteral("baresip is not connected")
                                               : QStringLiteral("%1: %2").arg(results.last().command, results.last().data.trimmed()));
                return;
            }
            const QString callId = wantedCall.isEmpty() ? currentCallId(results.at(results.size() - 2).data) : wantedCall;
            const auto format = parseAudioDebug(results.last().data);
            if (callId.isEmpty() || !format) {
                reply(false, QStringLiteral("no call with an audio stream to play into"));
                return;
            }
            // aufile does not resample: the clip must match the encoder's rate and channels.
            wav::Pcm out = wav::convert(clip, format->sampleRate, format->channels);
            const double seconds = out.seconds();
            wav::appendSilence(out, kPlayPaddingSeconds);
            const QString path = QDir(mediaDir).filePath(QStringLiteral("play-%1.wav").arg(++playCounter));
            QString werr;
            if (!wav::writeFile(path, out, &werr)) {
                reply(false, werr);
                return;
            }
            const QString restore = format->source;
            proxy.runSequence({{QStringLiteral("callfind"), callId}, {QStringLiteral("ausrc"), QStringLiteral("aufile,") + path}},
                              [&, reply, callId, restore, path, seconds, format = *format](const QList<CtrlProxy::StepResult> &r) {
                                  if (r.size() != 2 || !r.last().ok) {
                                      QFile::remove(path);
                                      reply(false, r.isEmpty() ? QStringLiteral("baresip is not connected") : r.last().data.trimmed());
                                      return;
                                  }
                                  playing = std::make_unique<Playback>();
                                  playing->callId = callId;
                                  playing->restoreSource = restore;
                                  playing->file = path;
                                  playing->timer = new QTimer(&app);
                                  playing->timer->setSingleShot(true);
                                  QObject::connect(playing->timer, &QTimer::timeout, &app, [&] { endPlayback({}); });
                                  playing->timer->start(static_cast<int>(seconds * 1000) + 150);
                                  const QJsonObject info{{QStringLiteral("id"), callId},
                                                         {QStringLiteral("seconds"), seconds},
                                                         {QStringLiteral("codec"), format.codec},
                                                         {QStringLiteral("srate"), format.sampleRate},
                                                         {QStringLiteral("channels"), format.channels}};
                                  QJsonObject ev = info;
                                  ev[QStringLiteral("class")] = QStringLiteral("leansipd");
                                  ev[QStringLiteral("type")] = QStringLiteral("PLAY_STARTED");
                                  proxy.publishEvent(ev);
                                  reply(true, compact(info));
                              });
        });
    });
    proxy.addCommand(QStringLiteral("leansipd_play_stop"), [&](const QString &) {
        if (!playing)
            return CtrlProxy::Result{false, QStringLiteral("nothing is playing")};
        endPlayback({});
        return CtrlProxy::Result{true, QStringLiteral("stopped")};
    });

    // --- per-call bookkeeping from baresip's events --------------------------------------------
    QObject::connect(&upstream, &BaresipClient::rawEventReceived, &app, [&](const QJsonObject &ev) {
        const QString id = ev.value(QStringLiteral("id")).toString();
        const QString type = ev.value(QStringLiteral("type")).toString();
        if (id.isEmpty() || ev.value(QStringLiteral("class")).toString() != QLatin1String("call"))
            return;
        if (recordings) {
            if (const auto files = recordings->onCallEvent(id)) {
                proxy.publishEvent({{QStringLiteral("class"), QStringLiteral("leansipd")},
                                    {QStringLiteral("type"), QStringLiteral("RECORDING_STARTED")},
                                    {QStringLiteral("id"), id},
                                    {QStringLiteral("sent"), files->sent},
                                    {QStringLiteral("received"), files->received}});
            }
        }
        if (type == QLatin1String("CALL_INCOMING")) {
            ringing.insert(id);
        } else if (type == QLatin1String("CALL_ESTABLISHED")) {
            ringing.remove(id);
            // Record the stream format: analyze reports it, and play needs it.
            proxy.runSequence({{QStringLiteral("callfind"), id}, {QStringLiteral("audio_debug"), {}}},
                              [&proxy, id](const QList<CtrlProxy::StepResult> &r) {
                                  if (r.size() != 2 || !r.last().ok)
                                      return;
                                  const auto f = parseAudioDebug(r.last().data);
                                  if (!f)
                                      return;
                                  proxy.publishEvent({{QStringLiteral("class"), QStringLiteral("call")},
                                                      {QStringLiteral("type"), QStringLiteral("CALL_AUDIO")},
                                                      {QStringLiteral("id"), id},
                                                      {QStringLiteral("codec"), f->codec},
                                                      {QStringLiteral("srate"), f->sampleRate},
                                                      {QStringLiteral("channels"), f->channels}});
                              });
            if (maxCallSeconds > 0 && !uiCalls.contains(id) && !limitTimers.contains(id)) {
                auto *timer = new QTimer(&app);
                timer->setSingleShot(true);
                limitTimers.insert(id, timer);
                QObject::connect(timer, &QTimer::timeout, &app, [&, id] {
                    if (uiCalls.contains(id))
                        return;
                    qInfo("leansipd: call %s reached --max-call-seconds, hanging up", qPrintable(id));
                    proxy.publishEvent({{QStringLiteral("class"), QStringLiteral("leansipd")},
                                        {QStringLiteral("type"), QStringLiteral("CALL_LIMIT")},
                                        {QStringLiteral("id"), id},
                                        {QStringLiteral("param"), QStringLiteral("%1 s").arg(maxCallSeconds)}});
                    proxy.runSequence({{QStringLiteral("callfind"), id}, {QStringLiteral("hangup"), {}}}, {});
                });
                timer->start(maxCallSeconds * 1000);
            }
        } else if (type == QLatin1String("CALL_CLOSED")) {
            ringing.remove(id);
            uiCalls.remove(id);
            if (QTimer *t = limitTimers.take(id))
                t->deleteLater();
            if (playing && playing->callId == id)
                endPlayback(QStringLiteral("call closed"));
            if (recordings) {
                if (const auto files = recordings->filesFor(id)) {
                    proxy.publishEvent({{QStringLiteral("class"), QStringLiteral("leansipd")},
                                        {QStringLiteral("type"), QStringLiteral("RECORDING_DONE")},
                                        {QStringLiteral("id"), id},
                                        {QStringLiteral("sent"), files->sent},
                                        {QStringLiteral("received"), files->received}});
                }
            }
        }
    });

    // --- daemon commands ----------------------------------------------------------------------
    proxy.addCommand(QStringLiteral("leansipd_status"), [&](const QString &) {
        QJsonObject s;
        s[QStringLiteral("profile")] = profileName;
        s[QStringLiteral("aor")] = prof.aor;
        s[QStringLiteral("config")] = configPath;
        s[QStringLiteral("confirmDial")] = prof.confirmDial;
        s[QStringLiteral("runtimeDir")] = runtimeDir;
        s[QStringLiteral("socket")] = socketPath;
        s[QStringLiteral("log")] = log.path();
        s[QStringLiteral("pid")] = QCoreApplication::applicationPid();
        s[QStringLiteral("baresipPid")] = process.pid();
        s[QStringLiteral("baresipUp")] = proxy.backendUp();
        s[QStringLiteral("clients")] = proxy.clientCount();
        s[QStringLiteral("uiOwned")] = uiOwned;
        s[QStringLiteral("uiAttached")] = proxy.hasOwner();
        s[QStringLiteral("sipTrace")] = parser.isSet(traceOpt);
        s[QStringLiteral("recording")] = recording;
        s[QStringLiteral("recordingsDir")] = recording ? recordingsDir : QString();
        s[QStringLiteral("maxCallSeconds")] = maxCallSeconds;
        s[QStringLiteral("playing")] = playing ? playing->callId : QString();
        s[QStringLiteral("seq")] = proxy.lastSeq();
        return CtrlProxy::Result{true, compact(s)};
    });
    proxy.addCommand(QStringLiteral("leansipd_recordings"), [&](const QString &) {
        if (!recordings)
            return CtrlProxy::Result{false, QStringLiteral("recording is off: start the daemon with --record, "
                                                           "or set record = true in the profile")};
        return CtrlProxy::Result{true, compact(recordings->toJson())};
    });
    // Picks up edits to leansip.toml: account, password and baresip settings. A config that does
    // not load leaves baresip running as it is. Blocks while password_cmd runs.
    proxy.addCommand(QStringLiteral("leansipd_restart"), [&](const QString &) {
        QString why;
        settings::Profile reloaded;
        const auto next = loadSetup(configPath, profileName, &reloaded, &why);
        if (!next)
            return CtrlProxy::Result{false, why};
        prof = reloaded;
        restartTimer.stop();
        proxy.setBackendUp(false, QStringLiteral("restarting baresip"));
        upstream.disconnectFromHost();
        process.stop();
        if (!process.start(*next))
            return CtrlProxy::Result{false, process.lastError()};
        upstream.connectToHost(process.host(), process.port());
        return CtrlProxy::Result{true, QStringLiteral("restarted")};
    });
    proxy.addCommand(QStringLiteral("leansipd_shutdown"), [&](const QString &) {
        QTimer::singleShot(0, &app, &QCoreApplication::quit); // reply first
        return CtrlProxy::Result{true, QStringLiteral("shutting down")};
    });

    // The dialer that started us owns us: when its connection closes (quit or crash), so do we,
    // even if leansipctl/agent clients are still connected.
    proxy.setOwnerBindingAllowed(uiOwned);
    QObject::connect(&proxy, &CtrlProxy::ownerGone, &app, [] {
        qInfo("leansipd: the dialer that started us closed, exiting");
        QCoreApplication::quit();
    });
    if (uiOwned) {
        QTimer::singleShot(kBindGraceMs, &app, [&] {
            if (!proxy.hasOwner()) {
                qInfo("leansipd: the dialer never attached, exiting");
                QCoreApplication::quit();
            }
        });
    }

    // A daemon outlives its terminal: ignore SIGHUP, stop cleanly on SIGINT/SIGTERM.
    std::signal(SIGHUP, SIG_IGN);
    UnixSignalBridge signalBridge;
    if (signalBridge.install({SIGINT, SIGTERM}))
        QObject::connect(&signalBridge, &UnixSignalBridge::signalReceived, &app, [] { QCoreApplication::quit(); });

    QObject::connect(&app, &QCoreApplication::aboutToQuit, &app, [&] {
        restartTimer.stop();
        QObject::disconnect(&process, &BaresipProcess::finished, nullptr, nullptr);
        proxy.close();
        if (upstream.isConnected()) {
            upstream.sendCommand(QStringLiteral("quit"));
            upstream.disconnectFromHost();
        }
        process.stop();
        QFile::remove(QDir(runtimeDir).filePath(QStringLiteral("accounts"))); // holds the password
        if (playing)
            QFile::remove(playing->file);
        qInfo("leansipd: stopped");
        g_log = nullptr;
    });

    qInfo("leansipd: serving profile %s (%s) on %s", qPrintable(profileName), qPrintable(prof.aor), qPrintable(socketPath));
    return app.exec();
}
