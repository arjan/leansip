---
name: sipctl
description: >-
  Place, answer, listen to, speak into, record and analyze real SIP calls from the command
  line with sipctl/leansipd (baresip underneath): DTMF, text-to-speech and speech-to-text
  via ElevenLabs, event waits, RTCP and audio-level analysis, SIP traces. Use when testing or
  debugging a phone bot, IVR, SIP trunk, PBX or softphone end to end, when a call fails,
  rings out, has one-way audio or ignores DTMF, or when the user asks to call a number and
  check what it says. For the local DialoX/Grid setup see also the dialox-phone skill.
---

# SIP testing and debugging with sipctl

`sipctl` drives `leansipd`, a daemon that owns one baresip per baresip directory. The
leansip dialer (a GUI) is a client of the same daemon, so you can watch or join a call the
user has open. Binaries: `~/devel/leansip/build/src/{sipctl,leansipd,leansip}` (not on PATH).
`sipctl help` and `sipctl help <command>` are authoritative; `docs/sipctl.md` in the leansip
repo is the full reference.

```bash
X() { ~/devel/leansip/build/src/sipctl -d ~/.baresip-e2e "$@"; }   # -d picks the account
```

Every command prints one JSON object per line. Exit codes: 0 ok, 1 failed/refused/timeout,
2 usage, 3 daemon not running.

## Rules

- A dial is a real call on a real account. sipctl only dials loopback destinations and
  patterns in `<baresip dir>/sipctl-allow`. Never add patterns to that file without the
  user's go-ahead for those destinations; show them the line you would add.
- Do not use an account the user's dialer window is using for unrelated test calls: the
  daemon is shared and the user hears/sees the calls. `status` shows `uiOwned`.
- Recording (`start --record`) records the other party. Use it for test calls to bots and
  test lines; for calls with people, ask first.
- Always `hangup` and `stop` what you started, also after a failure.
- Never print `auth_pass` from an accounts file or the value of `ELEVENLABS_API_KEY`.

## A test call

```bash
X start --record                              # --sip-trace to log SIP; --max-call-seconds N
X reginfo                                     # registered? the account's line shows OK
r=$(X dial 940); echo "$r"                    # {"ok":true,"seq":12,"call":"<id>",...}
seq=$(echo "$r" | jq .seq); call=$(echo "$r" | jq -r .call)
X wait CALL_ESTABLISHED,CALL_CLOSED --call "$call" --since "$seq" --timeout 30
X listen --call "$call" --timeout 15          # {"text":"Welcome to ...","seconds":3.2,...}
X say "I want to speak to support" --call "$call" --wait
X listen --call "$call"
X dtmf 1 --call "$call"
X hangup --call "$call"
X analyze --call "$call"                      # timing, codec, RTCP, levels, warnings
X stop
```

- Always pass a command's `seq` to `wait --since`, or an event that fires before `wait`
  connects is missed. Wait for `CALL_CLOSED` alongside the event you expect.
- Pass `--call` whenever more than one call may exist: baresip's "current call" is shared
  with every client, including the user's dialer.
- `listen` returns after the far end's next utterance (it waits for `--silence` ms of quiet,
  default 1200). Call it right after the bot is expected to speak; tune `--timeout`,
  `--threshold` (dBFS, default -45) and `--max`. `--no-stt` gives just the WAV.
- `say` replaces the call's audio source while it plays; `--wait` returns when done.

## Speech

`say`, `listen` and `transcribe` need `ELEVENLABS_API_KEY`; load it with the
secrets-loading skill rather than exporting it, e.g.

```bash
with-secrets botsi-dev-env -- ~/devel/leansip/build/src/sipctl -d ~/.baresip-e2e listen
```

Optional: `ELEVENLABS_VOICE_ID`, `ELEVENLABS_TTS_MODEL` (eleven_multilingual_v2),
`ELEVENLABS_STT_MODEL` (scribe_v2).

## Diagnosing

| Symptom | Look at |
|---|---|
| Not registered | `X reginfo`; `X events --since 0` for `REGISTER_FAIL` and its `param` (401/403/408); `start --sip-trace` and read `status.data.log` |
| Call fails or rings out | `CALL_CLOSED` `param` (e.g. `404 Not Found`, `486 Busy Here`, `488`); `analyze` `timing` |
| One-way or no audio | `analyze` `levels` (needs `--record`) and `warnings`; `rtcp` loss and jitter; `X raw callstat` for SDP and RTP |
| DTMF ignored | the far end's codec offers `telephone-event` (`raw callstat`); send slower or one digit at a time |
| Bot says nothing | `listen --no-stt --threshold -60` to see whether any audio arrives at all |
| Daemon problems | `X status`; the log at `status.data.log`; `BACKEND_DOWN` events |

Two daemons (two baresip directories) can call each other on 127.0.0.1 without a SIP server,
see "Local test calls" in docs/sipctl.md. Keep both legs of a test in separate directories.

## Known baresip 1.1.0 quirks

- `resume` reports "Invalid argument" even when it works (sipctl corrects for it).
- `ausine` sources only work for 48 kHz codecs; use an `aufile` source with PCMU/G722.
- An `aufile` source that reaches its end makes baresip hang up; give silence files longer
  than the call.
