# leansipctl

`leansipctl` controls `leansipd`, the daemon that runs baresip for one `leansip.toml` profile and
shares it between the leansip dialer, scripts and coding agents. Every client sees the same
calls and events: an agent can follow, speak into or listen to a call you have open in the
dialer, and the dialer shows (and can hang up) calls an agent placed.

`leansipctl help` lists the commands; `leansipctl help <command>` or `leansipctl <command> --help` shows
one in detail. This page covers the same ground plus what needs more room.

## Profiles and daemons

A profile is one SIP account in `~/.config/leansip/leansip.toml` (format:
[configuration.md](configuration.md)). `-p` picks one; without it leansipctl uses `$LEANSIP_PROFILE`, then the file's
`default_profile`, then the only profile. `--config` or `$LEANSIP_CONFIG` points at another file.

```
leansipctl profiles            # name, aor, confirmDial, running; never passwords
leansipctl -p dev2 status
```

Each profile gets its own daemon, socket, log and recordings under
`$XDG_RUNTIME_DIR/leansip-<profile>/`. After editing the file, `leansipctl raw leansipd_restart`
reloads the profile; with an error in the file it replies `ok: false` and baresip keeps running
as it was.

A profile with `confirm_dial = true` (a real trunk, say) only dials with `--yes`, and the same
holds for `raw` commands that contain "dial". This guards against an agent picking the wrong
profile; it is not access control, since any process of yours can talk to the socket.

### Who stops the daemon

| Started by | Stops when |
|---|---|
| the leansip dialer | the dialer closes or crashes, even if leansipctl clients are still connected |
| `leansipctl start` | `leansipctl stop` |

`leansipctl start` on a profile whose dialer is open reuses the dialer's daemon; the reply then
has `"uiOwned": true` and a `note`. Close the dialer first and run `leansipctl start` if the agent
needs a daemon of its own. Options such as `--record` only apply when `start` launches the
daemon; on a running one the reply carries a `warning`.

`leansipctl stop` returns once the daemon has exited, so a following `start` can bind the same SIP
port. If baresip crashes, leansipd restarts it (after 1, 2, 5, 10, then 30 s) and clients see
`BACKEND_DOWN` and `BACKEND_UP`.

## Call time limit

Calls that leansipctl (or any client other than the dialer) placed or answered are hung up after
`--max-call-seconds` (default 600; `start --max-call-seconds 0` turns it off). The event is
`CALL_LIMIT`. Destinations are not restricted (only `confirm_dial` profiles want `--yes`):
every `dial` on a real account is a real call that may cost money or reach a person.

## Output and exit codes

stdout gets one JSON object per line. Command replies:

```
{"ok":true,"seq":12,"call":"35fa5b711b6ec554","data":"call id: 35fa5b711b6ec554"}
{"ok":false,"seq":12,"data":"could not find UA for 123"}
```

`data` is baresip's reply text with ANSI colours removed, or a JSON value for the daemon's own
commands. Errors that never reached baresip look like `{"ok":false,"error":"..."}`.

| Exit | Meaning |
|---|---|
| 0 | ok |
| 1 | refused or failed, or `wait`/`listen` timed out |
| 2 | usage error (unknown command, bad number, bad option, bad leansip.toml, `--yes` missing) |
| 3 | no leansipd for this profile; run `leansipctl start` |

## Targeting a call

baresip has one "current call" for all clients. Commands that act on a call (`answer`,
`hangup`, `dtmf`, `mute`, `hold`, `resume`, `say`, `play`, `listen`, `raw`) take `--call <id>`;
leansipd then selects that call and runs the command right after it, with no other client's
command in between. Call ids are in `dial` replies (`"call"`) and in every call event (`"id"`).
Without `--call` the command goes to baresip's current call, which is fine with a single call.

## Events

Events are baresip's ctrl_tcp events, unchanged, plus `seq` (a counter that increases by one
per event) and `ts` (milliseconds since the epoch). leansipd keeps the last 2000.

```
{"event":true,"class":"call","type":"CALL_INCOMING","accountaor":"sip:bob@127.0.0.1",
 "direction":"incoming","id":"35fa5b711b6ec554","peeruri":"sip:alice@127.0.0.1",
 "param":"sip:alice@127.0.0.1","remoteaudiodir":"sendrecv","seq":3,"ts":1793462127308}
```

| Type | When |
|---|---|
| `REGISTER_OK`, `REGISTER_FAIL`, `UNREGISTERING` | registration; `param` holds the SIP status |
| `CALL_INCOMING` | a call rings here |
| `CALL_RINGING`, `CALL_PROGRESS` | outgoing call: 180 / 183 received |
| `CALL_ANSWERED`, `CALL_ESTABLISHED` | the call is up |
| `CALL_AUDIO` | from leansipd, once per call: `codec`, `srate`, `channels` of the stream |
| `CALL_LOCAL_SDP`, `CALL_REMOTE_SDP` | an offer or answer was sent or received |
| `CALL_RTCP` | an RTCP report; `rtcp_stats` holds rtt and rx/tx jitter, loss and packets |
| `CALL_DTMF_START`, `CALL_DTMF_END` | a digit was received; `param` is the digit |
| `CALL_CLOSED` | the call ended; `param` is the reason, e.g. `486 Busy Here` |
| `CALL_LIMIT` | leansipd hung up a call at `--max-call-seconds` |
| `PLAY_STARTED`, `PLAY_ENDED` | a `say`/`play` clip; `PLAY_ENDED` `param` is `done` or `call closed` |
| `RECORDING_STARTED`, `RECORDING_DONE` | with `--record`: the call's `sent` and `received` WAV paths |
| `AUDIO_TAP` | without `--record`, in a daemon the dialer started: the same paths, for the dialer's activity display; deleted when the call ends |
| `BACKEND_UP`, `BACKEND_DOWN` | class `leansipd`: baresip (re)started or went away; `param` says why |

`events --since N` prints the journal after `N`; `--follow` keeps printing. `events` and `wait`
filter with `--call`, `--account` and `--param` (substring).

### Waiting without races

Every reply carries `seq`, the number of the last event when the command arrived. Pass it to
`wait --since` so an event that fires between the command and the `wait` still counts:

```
r=$(leansipctl dial 940); seq=$(echo "$r" | jq .seq); call=$(echo "$r" | jq -r .call)
leansipctl wait CALL_ESTABLISHED,CALL_CLOSED --call "$call" --since "$seq" --timeout 30
```

Without `--since`, `wait` only sees events that arrive after it connected. Waiting for
`CALL_ESTABLISHED` alone hangs until the timeout when the call is rejected; include
`CALL_CLOSED` and check `type`.

## Speech and audio

Speech uses ElevenLabs and nothing else. `say`, `listen` (unless `--no-stt`) and `transcribe`
need `ELEVENLABS_API_KEY` in leansipctl's environment; the daemon never sees the key.

| Variable | Default |
|---|---|
| `ELEVENLABS_API_KEY` | required |
| `ELEVENLABS_VOICE_ID` | `JBFqnCBsd6RMkjVDRZzb` |
| `ELEVENLABS_TTS_MODEL` | `eleven_multilingual_v2` |
| `ELEVENLABS_STT_MODEL` | `scribe_v2` |
| `ELEVENLABS_BASE_URL` | `https://api.elevenlabs.io` (http is accepted only for loopback, for tests) |

With the key in a Bitwarden note: `with-secrets <note> -- leansipctl say "…"`.

### say and play

```
leansipctl say "I would like to speak to support" --wait
leansipctl play prompt.wav --call "$call" --wait
```

The clip replaces the call's source (microphone, `ausine`, `aufile`) while it plays, and the
source is switched back when it ends. `play` takes 16-bit PCM WAV, mono or stereo, at any rate;
leansipd converts it to the call's stream format first, because baresip's `aufile` does not
resample. `--wait` returns at `PLAY_ENDED`. One clip at a time per daemon.

### listen

Needs a daemon started with `--record`.

```
leansipctl start --record
leansipctl listen --timeout 20          # {"ok":true,"text":"Welcome to …","seconds":3.1,"audio":"…wav"}
```

It reads what the call receives from the moment it starts: waits up to `--timeout` seconds
(default 30) for sound above `--threshold` dBFS (default -45), then takes everything until
`--silence` ms of quiet (default 1200) or `--max` seconds (default 30), and transcribes that.
`--no-stt` skips the transcription and only returns the WAV. A call that stays at a constant
tone (`ausine`) or music never goes quiet; raise `--threshold` or lower `--max`.

### Recordings

`start --record` (or `leansip --record` for the dialer's daemon) loads baresip's `sndfile` module:
every call is recorded in both directions to
`<runtime dir>/recordings/<call id>-sent.wav` and `-received.wav`, at the stream's own rate.
`leansipctl recordings` lists them. They live in `$XDG_RUNTIME_DIR`, which is cleared at logout;
copy what you need. Recording is off by default, because it records everyone on the call.
When several calls start their audio in the same second, sndfile's file names can collide;
record one call at a time if the recordings matter.

### transcribe and analyze

`leansipctl transcribe file.wav` sends a WAV (converted to 16 kHz mono) to speech-to-text and
needs no daemon. `leansipctl analyze [--call ID]` summarises a call from the event journal:

```
{"ok":true,"id":"…","direction":"outgoing","state":"closed","closeReason":"Connection reset by user",
 "timing":{"ringingAfterMs":0,"answeredAfterMs":161,"talkMs":15502,"totalMs":15663},
 "audio":{"codec":"opus","sampleRate":48000,"channels":2},"dtmfReceived":"1#",
 "rtcp":{"rtt":0,"rx":{"jit":770,"lost":0,"sent":500},"tx":{…},"maxRxJitter":812,"reports":5},
 "levels":{"sent":{"rmsDbfs":-18.1,"silentPercent":87.1,…},"received":{…}},
 "warnings":[]}
```

`levels` needs `--record`. RTCP values are as baresip reports them. `warnings` names likely
problems: one-way audio, nothing sent, packet loss, no RTCP, never answered.

## Commands

| Command | baresip | Notes |
|---|---|---|
| `profiles` | | profiles in leansip.toml; needs no daemon |
| `status` | | daemon state; exit 3 when not running |
| `start [--sip-trace] [--record] [--max-call-seconds S]` | | returns once baresip is reachable |
| `stop` | | ends all calls, unregisters; waits for the daemon to exit |
| `dial <target> [--yes]` | `dial` | digits, `* # +`, `tel:`, `sip:`/`sips:`, `user@host[:port]` |
| `answer`, `hangup` | `accept`, `hangup` | `hangup` also rejects a ringing call |
| `dtmf <digits>` | `sndcode` | `0-9 * # A-D`, one digit every 200 ms (RFC 4733) |
| `mute`, `hold`, `resume` | same | `mute` toggles |
| `say`, `play`, `listen` | | see above |
| `transcribe <wav>`, `recordings`, `analyze` | | see above |
| `calls`, `reginfo` | `listcalls`, `reginfo` | |
| `raw <cmd> [params]` | any | `quit` is refused; use `stop`. `raw leansipd_restart` reloads the profile |
| `events`, `wait` | | see Events |

`dial` refuses anything with spaces, `;` or quotes, because the target goes verbatim to
baresip's command parser.

`leansipctl raw help` lists what the running baresip accepts; call commands (`hold`, `mute`,
`transfer`, `sndcode`, …) appear only while a call is active.

## baresip 1.1.0 behaviour to know

- `resume` replies "Invalid argument" even when it resumes; leansipctl checks `calls` and reports ok.
- When an `aufile` source reaches the end of its file, baresip's menu module hangs up the call.
  leansipd pads clips with 3 s of silence and switches back before the end.
- `ausine` only works at 48 kHz: with PCMU/PCMA/G722 calls, use an `aufile` source.
- `mixausrc` (mixing a clip into the microphone) only takes mono streams, and its stop command
  crashes baresip when nothing is mixing. leansip does not use it.

## SIP traces and logs

`leansipctl start --sip-trace` runs baresip with `-s`: every SIP message goes to the log
(`status.data.log`), next to leansipd's own lines and baresip's output. The log rotates at 5 MB
(one `.1` file is kept). It sits in the 0700 runtime directory and, with a trace on, contains
full SIP headers, including digest authentication responses.

## Local test calls without a SIP server

Two profiles can call each other on the loopback interface without registering anywhere and
without a sound card:

```toml
[profiles.alice]
aor = "sip:alice@127.0.0.1"
account_params = { regint = 0 }

[profiles.alice.baresip]
sip_listen = "127.0.0.1:15060"            # 15062 for bob: baresip may take the next port too
module = ["g711.so", "aufile.so"]
audio_source = "aufile,/path/to/silence.wav"
audio_player = "aufile,/path/to/heard.wav"
audio_alert = []
ausrc_srate = []                          # aufile does not resample: run at the codec's rate
auplay_srate = []
```

and the same for bob. The silence WAV must match the codec's rate and channels (8 kHz mono for
PCMU) and be longer than the call. `tests/integration_loopback.sh` builds exactly this, with a
temporary `XDG_RUNTIME_DIR` and `LEANSIP_CONFIG`, and runs a call through every command;
`ctest -R integration` runs it. Use two profiles, not one baresip with two accounts: with both
legs in one baresip, its menu module puts the first call on hold when the second is answered.
