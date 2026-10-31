# sipctl

`sipctl` controls `leansipd`, the daemon that runs baresip for one baresip directory and
shares it between the leansip dialer, scripts and coding agents. Every client sees the same
calls and events: an agent can follow or drive a call you have open in the dialer, and the
dialer shows and can hang up a call an agent placed.

`sipctl help` lists the commands; `sipctl help <command>` or `sipctl <command> --help` shows
one in detail. This page covers the same ground plus the parts that need more room.

## Accounts and daemons

An account is a baresip directory: `~/.baresip` by default, another one with `-d`:

```
sipctl -d ~/.baresip2 status
```

Each directory gets its own daemon, socket and log under `$XDG_RUNTIME_DIR/leansip[-<hash>]/`.
leansipd uses the **first** account in that directory's `accounts` file for `dial`.

### Who stops the daemon

| Started by | Stops when |
|---|---|
| the leansip dialer | the dialer closes or crashes, even if sipctl clients are still connected |
| `sipctl start` | `sipctl stop` |

`sipctl start` on an account whose dialer is open reuses the dialer's daemon. The reply then
has `"uiOwned": true` in `status` and a `note`: that daemon goes away when the dialer closes,
and every call on it ends. Close the dialer first and run `sipctl start` if the agent needs a
daemon of its own.

`sipctl stop` on a dialer's daemon works too; the dialer immediately starts a new one for
itself.

## Output and exit codes

stdout gets one JSON object per line. Command replies:

```
{"ok":true,"seq":12,"data":"call id: 35fa5b711b6ec554"}
{"ok":false,"seq":12,"data":"could not find UA for 123"}
```

`data` is baresip's reply text with ANSI colours removed, or a JSON value for the daemon's own
commands (`status`, `events`). Errors that never reached baresip look like
`{"ok":false,"error":"..."}`.

| Exit | Meaning |
|---|---|
| 0 | ok |
| 1 | baresip refused the command, or `wait` timed out |
| 2 | usage error (unknown command, bad number, bad option) |
| 3 | no leansipd for this account; run `sipctl start` |

## Events

Events are baresip's ctrl_tcp events, unchanged, plus two fields from leansipd: `seq`, a
counter that increases by one per event, and `ts`, milliseconds since the epoch.

```
{"event":true,"class":"call","type":"CALL_INCOMING","accountaor":"sip:bob@127.0.0.1",
 "direction":"incoming","id":"35fa5b711b6ec554","peeruri":"sip:alice@127.0.0.1",
 "param":"sip:alice@127.0.0.1","remoteaudiodir":"sendrecv","seq":3,"ts":1793462127308}
```

Types seen in practice:

| Type | When |
|---|---|
| `REGISTER_OK`, `REGISTER_FAIL`, `UNREGISTERING` | registration; `param` holds the SIP status |
| `CALL_INCOMING` | a call rings here |
| `CALL_RINGING`, `CALL_PROGRESS` | outgoing call: 180 / 183 received |
| `CALL_ANSWERED`, `CALL_ESTABLISHED` | the call is up |
| `CALL_LOCAL_SDP`, `CALL_REMOTE_SDP` | an offer or answer was sent or received; `param` is `offer` or `answer` |
| `CALL_RTCP` | an RTCP report arrived |
| `CALL_DTMF_START`, `CALL_DTMF_END` | a digit was received; `param` is the digit |
| `CALL_CLOSED` | the call ended; `param` is the reason, e.g. `486 Busy Here` or `Connection reset by peer` |
| `BACKEND_UP`, `BACKEND_DOWN` | class `leansipd`: baresip (re)started or went away; `param` says why |

leansipd keeps the last 2000 events. `sipctl events --since N` prints those after `N`;
`--follow` keeps printing new ones.

### Waiting without races

Every reply carries `seq`: the number of the last event when the command arrived. Pass it to
`wait --since` so an event that fires between the command and the `wait` still counts:

```
seq=$(sipctl dial 940 | jq .seq)
sipctl wait CALL_ESTABLISHED,CALL_CLOSED --since "$seq" --timeout 30
```

Without `--since`, `wait` only sees events that arrive after it connected. Waiting for
`CALL_ESTABLISHED` alone hangs until the timeout when the call is rejected, so wait for
`CALL_CLOSED` as well and check `type` in the result.

## Commands

| Command | baresip command | Notes |
|---|---|---|
| `status` | | daemon state; exit 3 when not running |
| `start [--sip-trace]` | | returns once baresip is reachable (up to 5 s; else a `warning`) |
| `stop` | | ends all calls, unregisters |
| `dial <target>` | `dial` | digits, `* # +`, `tel:`, `sip:`/`sips:` URIs, `user@host[:port]` |
| `answer` | `accept` | |
| `hangup` | `hangup` | also rejects a ringing call |
| `dtmf <digits>` | `sndcode` | `0-9 * # A-D`, one digit every 200 ms, as RFC 4733 events |
| `calls` | `listcalls` | |
| `reginfo` | `reginfo` | a registered account's line contains `OK` |
| `raw <cmd> [params]` | any | `quit` is refused; use `stop` |
| `events [--since N] [--follow]` | | |
| `wait <TYPES> [--since N] [--timeout S]` | | default timeout 30 s |

`dial` refuses anything with spaces, `;` or quotes, because the target is passed verbatim to
baresip's command parser.

### raw

`sipctl raw help` lists what the running baresip accepts. Call commands appear in that list
only while a call is active:

```
/hold        x     Call hold
/mute        m     Call mute/un-mute
/reinvite    I     Send re-INVITE
/resume      X     Call resume
/sndcode ..        Send Code
/transfer .. t ..  Transfer call
```

Other useful ones: `callstat` (SDP, codecs and RTP/RTCP statistics of the current call),
`uafind <aor>` (pick the account the next `dial` uses), `ausrc` / `auplay` (switch audio
source or player), `play <file>`.

baresip 1.1.0 answers `resume` with `{"ok":false,"data":"Invalid argument"}` even though the
call does come off hold. Check with `sipctl calls`: a held call shows `(on hold)`.

## SIP traces and logs

`sipctl start --sip-trace` runs baresip with `-s`, which writes every SIP message to the log at
`status.data.log`. The option only applies when that `start` launches the daemon; on a running
one the reply carries a `warning`. The log also holds leansipd's own lines and baresip's normal
output. It sits in the 0700 runtime directory, and with a trace on it contains full SIP
headers, including digest authentication responses.

## Local test calls without a SIP server

Two directories can call each other on the loopback interface without registering anywhere and
without a sound card. In one directory's `config`:

```
sip_listen      127.0.0.1:15060
module          ausine.so
module          aufile.so
audio_source    ausine,440
audio_player    aufile,/path/to/alice-heard.wav
```

with `accounts` containing `<sip:alice@127.0.0.1>;regint=0`. Do the same for bob on port 15061
with a different tone and WAV path. Then:

```
sipctl -d ./alice start; sipctl -d ./bob start
seq=$(sipctl -d ./alice dial sip:bob@127.0.0.1:15061 | jq .seq)
sipctl -d ./bob wait CALL_INCOMING --since 0 && sipctl -d ./bob answer
sipctl -d ./alice wait CALL_ESTABLISHED --since "$seq"
sipctl -d ./alice dtmf 12#
sipctl -d ./bob events --since 0 | jq -c 'select(.type=="CALL_DTMF_START") | .param'
sipctl -d ./alice hangup
```

Each side's received audio is in its WAV file.

Use two directories, not two accounts in one. With both legs in one baresip, its menu module
puts the first call on hold when the second is answered, and DTMF stops arriving.

## Safety

- Every `dial` on a real account is a real call and may cost money or reach a person.
- SIP traces contain the numbers you call and are called from. The log stays in your runtime
  directory until you delete it or log out. WAV files written by `aufile` contain the audio of
  the call and go wherever the config says.
- Any process running as your user can open the socket and place calls. Other users cannot.
