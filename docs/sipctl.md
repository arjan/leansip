# sipctl

`sipctl` controls `leansipd`, the daemon that runs baresip for one baresip directory and
shares it between the leansip dialer, scripts and coding agents. Every client sees the same
calls and events: an agent can follow, speak into or listen to a call you have open in the
dialer, and the dialer shows (and can hang up) calls an agent placed.

`sipctl help` lists the commands; `sipctl help <command>` or `sipctl <command> --help` shows
one in detail. This page covers the same ground plus what needs more room.

## Accounts and daemons

An account is a baresip directory: `~/.baresip` by default, another one with `-d`:

```
sipctl -d ~/.baresip2 status
```

Each directory gets its own daemon, socket, log and recordings under
`$XDG_RUNTIME_DIR/leansip[-<hash>]/`. `dial` uses the first account in the directory's
`accounts` file; `dial --from <aor>` uses another one.

### Who stops the daemon

| Started by | Stops when |
|---|---|
| the leansip dialer | the dialer closes or crashes, even if sipctl clients are still connected |
| `sipctl start` | `sipctl stop` |

`sipctl start` on an account whose dialer is open reuses the dialer's daemon; the reply then
has `"uiOwned": true` and a `note`. Close the dialer first and run `sipctl start` if the agent
needs a daemon of its own. Options such as `--record` only apply when `start` launches the
daemon; on a running one the reply carries a `warning`.

`sipctl stop` returns once the daemon has exited, so a following `start` can bind the same SIP
port. If baresip crashes, leansipd restarts it (after 1, 2, 5, 10, then 30 s) and clients see
`BACKEND_DOWN` and `BACKEND_UP`.

## Safety rules

These apply to sipctl and every other client except the dialer:

- **Destinations.** `dial`, `dialdir` and `transfer` only reach loopback destinations
  (`…@127.0.0.1`, `localhost`, `::1`) and those matching a pattern in
  `<baresip dir>/sipctl-allow`: one per line, `#` comments, `*` matches anything, compared with
  the number/user part and with the whole target. `dialcontact` is refused. The file is re-read
  on every dial.

  ```
  # ~/.baresip/sipctl-allow
  940            # local DialoX bot extension
  phone*         # local Grid phones
  ```

- **Call length.** Calls that sipctl placed or answered are hung up after
  `--max-call-seconds` (default 600; `start --max-call-seconds 0` turns it off). The event is
  `CALL_LIMIT`.
- Every allowed `dial` on a real account is still a real call that may cost money or reach a
  person.

## Output and exit codes

stdout gets one JSON object per line. Command replies:

```
{"ok":true,"seq":12,"call":"35fa5b711b6ec554","data":"call id: 35fa5b711b6ec554"}
{"ok":false,"seq":12,"data":"destination '940' is not allowed for sipctl: add a pattern to …"}
```

`data` is baresip's reply text with ANSI colours removed, or a JSON value for the daemon's own
commands. Errors that never reached baresip look like `{"ok":false,"error":"..."}`.

| Exit | Meaning |
|---|---|
| 0 | ok |
| 1 | refused or failed, or `wait`/`listen` timed out |
| 2 | usage error (unknown command, bad number, bad option) |
| 3 | no leansipd for this account; run `sipctl start` |

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
| `BACKEND_UP`, `BACKEND_DOWN` | class `leansipd`: baresip (re)started or went away; `param` says why |

`events --since N` prints the journal after `N`; `--follow` keeps printing. `events` and `wait`
filter with `--call`, `--account` and `--param` (substring).

### Waiting without races

Every reply carries `seq`, the number of the last event when the command arrived. Pass it to
`wait --since` so an event that fires between the command and the `wait` still counts:

```
r=$(sipctl dial 940); seq=$(echo "$r" | jq .seq); call=$(echo "$r" | jq -r .call)
sipctl wait CALL_ESTABLISHED,CALL_CLOSED --call "$call" --since "$seq" --timeout 30
```

Without `--since`, `wait` only sees events that arrive after it connected. Waiting for
`CALL_ESTABLISHED` alone hangs until the timeout when the call is rejected; include
`CALL_CLOSED` and check `type`.

## Speech and audio

Speech uses ElevenLabs and nothing else. `say`, `listen` (unless `--no-stt`) and `transcribe`
need `ELEVENLABS_API_KEY` in sipctl's environment; the daemon never sees the key.

| Variable | Default |
|---|---|
| `ELEVENLABS_API_KEY` | required |
| `ELEVENLABS_VOICE_ID` | `JBFqnCBsd6RMkjVDRZzb` |
| `ELEVENLABS_TTS_MODEL` | `eleven_multilingual_v2` |
| `ELEVENLABS_STT_MODEL` | `scribe_v2` |
| `ELEVENLABS_BASE_URL` | `https://api.elevenlabs.io` (http is accepted only for loopback, for tests) |

With the key in a Bitwarden note: `with-secrets <note> -- sipctl say "…"`.

### say and play

```
sipctl say "I would like to speak to support" --wait
sipctl play prompt.wav --call "$call" --wait
```

The clip replaces the call's source (microphone, `ausine`, `aufile`) while it plays, and the
source is switched back when it ends. `play` takes 16-bit PCM WAV, mono or stereo, at any rate;
leansipd converts it to the call's stream format first, because baresip's `aufile` does not
resample. `--wait` returns at `PLAY_ENDED`. One clip at a time per daemon.

### listen

Needs a daemon started with `--record`.

```
sipctl start --record
sipctl listen --timeout 20          # {"ok":true,"text":"Welcome to …","seconds":3.1,"audio":"…wav"}
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
`sipctl recordings` lists them. They live in `$XDG_RUNTIME_DIR`, which is cleared at logout;
copy what you need. Recording is off by default, because it records everyone on the call.
When several calls start their audio in the same second, sndfile's file names can collide;
record one call at a time if the recordings matter.

### transcribe and analyze

`sipctl transcribe file.wav` sends a WAV (converted to 16 kHz mono) to speech-to-text and
needs no daemon. `sipctl analyze [--call ID]` summarises a call from the event journal:

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
| `status` | | daemon state; exit 3 when not running |
| `start [--sip-trace] [--record] [--max-call-seconds S]` | | returns once baresip is reachable |
| `stop` | | ends all calls, unregisters; waits for the daemon to exit |
| `dial <target> [--from AOR]` | `dial` | digits, `* # +`, `tel:`, `sip:`/`sips:`, `user@host[:port]` |
| `answer`, `hangup` | `accept`, `hangup` | `hangup` also rejects a ringing call |
| `dtmf <digits>` | `sndcode` | `0-9 * # A-D`, one digit every 200 ms (RFC 4733) |
| `mute`, `hold`, `resume` | same | `mute` toggles |
| `say`, `play`, `listen` | | see above |
| `transcribe <wav>`, `recordings`, `analyze` | | see above |
| `calls`, `reginfo` | `listcalls`, `reginfo` | |
| `raw <cmd> [params]` | any | `quit` is refused; use `stop` |
| `events`, `wait` | | see Events |

`dial` refuses anything with spaces, `;` or quotes, because the target goes verbatim to
baresip's command parser.

`sipctl raw help` lists what the running baresip accepts; call commands (`hold`, `mute`,
`transfer`, `sndcode`, …) appear only while a call is active.

## baresip 1.1.0 behaviour to know

- `resume` replies "Invalid argument" even when it resumes; sipctl checks `calls` and reports ok.
- When an `aufile` source reaches the end of its file, baresip's menu module hangs up the call.
  leansipd pads clips with 3 s of silence and switches back before the end.
- `ausine` only works at 48 kHz: with PCMU/PCMA/G722 calls, use an `aufile` source.
- `mixausrc` (mixing a clip into the microphone) only takes mono streams, and its stop command
  crashes baresip when nothing is mixing. leansip does not use it.

## SIP traces and logs

`sipctl start --sip-trace` runs baresip with `-s`: every SIP message goes to the log
(`status.data.log`), next to leansipd's own lines and baresip's output. The log rotates at 5 MB
(one `.1` file is kept). It sits in the 0700 runtime directory and, with a trace on, contains
full SIP headers, including digest authentication responses.

## Local test calls without a SIP server

Two directories can call each other on the loopback interface without registering anywhere and
without a sound card. In each directory's `config`:

```
sip_listen      127.0.0.1:15060          # 15061 for the other one
module          aufile.so
audio_source    aufile,/path/to/silence.wav
audio_player    aufile,/path/to/heard.wav
```

with `accounts` holding `<sip:alice@127.0.0.1>;regint=0` (and bob in the other). The silence
WAV must match the codec's rate and channels (48 kHz stereo for opus, 8 kHz mono for PCMU) and
be longer than the call. `tests/integration_loopback.sh` builds exactly this, with a temporary
`XDG_RUNTIME_DIR`, and runs a call through every command; `ctest -R integration` runs it.
Use two directories, not two accounts in one: with both legs in one baresip, its menu module
puts the first call on hold when the second is answered.
