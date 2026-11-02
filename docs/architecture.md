# Architecture

leansip does no SIP or audio work itself. It runs [baresip](https://github.com/baresip/baresip)
and drives it over baresip's `ctrl_tcp` JSON interface.

## leansipd

`leansipd`, a small daemon, owns one baresip per profile and shares it: the dialer is one
client, `leansipctl` and anything else are others. All clients see the same calls and events,
so an agent can watch or drive a call you are on, and you can hang up a call an agent placed.

- The dialer starts `leansipd --ui-owned` when none is running, connects, and binds to it.
  It identifies itself as the dialer, so its calls have no time limit, and it selects its own
  call and account by id for every command, since baresip's "current call" is shared with the
  other clients. That daemon stops (and with it baresip, unregistering) when the dialer closes
  or crashes, even if leansipctl clients are still connected.
- A daemon started by `leansipctl start` keeps running until `leansipctl stop`; a dialer that
  attaches to it leaves it running when it closes, and hangs up only a call it dialled or
  answered itself.
- Before it listens, leansipd reads the profile and runs `password_cmd`. If that fails it exits,
  and whoever started it reports the last line of its log.
- Clients use `$XDG_RUNTIME_DIR/leansip-<profile>/leansipd.sock`, a Unix socket in a 0700
  directory. They speak baresip's own `ctrl_tcp` protocol (netstring-framed JSON); the daemon
  routes each response back to the client that asked, broadcasts every event with a running
  `"seq"` and a `"ts"`, and keeps the last 2000 events for clients that connect later. It
  refuses `quit` and adds its own commands: `leansipd_seq` (several commands back to back,
  e.g. `callfind` + `hangup`), `leansipd_play`, `leansipd_recordings`, `leansipd_status`,
  `leansipd_events <since>`, `leansipd_restart` (reloads the profile), `leansipd_hello ui`,
  `leansipd_bind` and `leansipd_shutdown`.
- The daemon's output, and baresip's, goes to `leansipd.log` in the same directory (rotated at
  5 MB). It restarts baresip if baresip crashes.

## baresip's files

leansipd writes baresip's configuration directory into the runtime directory:

- `config`: the built-in template with the profile's settings (see
  [configuration.md](configuration.md)), minus console UI modules (`stdio`, `cons`, `gtk`), plus
  the modules leansip needs (`account`, `menu`, `debug_cmd`, `aufile`, and `sndfile` when
  recording), and `ctrl_tcp` on `127.0.0.1:<random port>` (never `0.0.0.0`).
- `audio_path` points at a private `sounds/` directory holding baresip's stock WAVs (symlinked)
  plus generated tones: 16 DTMF digits, played via `debug_cmd`'s `play` command whenever you
  send one, and an "invalid number" tone (the ITU special information tone) written as
  `error.wav`/`notfound.wav`, so baresip's own failed-call handling and leansip's local
  rejections sound the same.
- With `--record`, baresip's `sndfile` module records both directions of every call. A daemon
  the dialer started records them anyway, into `live/` in the runtime directory (tmpfs, 0700),
  and announces them with an `AUDIO_TAP` event: the dialer reads the files as they grow for its
  activity display and frees what it has read, and leansipd deletes them when the call ends and
  removes `live/` when it exits.
- `accounts`: the profile's account line with the password, mode 0600, never written through a
  link, and removed when leansipd exits. `uuid`: the profile's instance id, its own `uuid` or one
  derived from `/etc/machine-id` and the profile (see "Instance id" in configuration.md).

It then runs `baresip -f <that dir>` and stops it on exit. The password is never logged or
shown by leansip, leansipctl or the dialer.

Stop any baresip you already run for the same account first, otherwise two user agents register
it and split its incoming calls.

## Security

Only your user can open the leansipd socket, but any process running as you can then place calls
on your accounts; `confirm_dial` guards against mistakes, not against such a process. baresip's
`ctrl_tcp` interface, which leansipd uses, has no authentication; binding it to loopback keeps it
off the network, but any local user can connect to it. This is the same trust level as baresip's
own console modules.

## Desktop integration

On Wayland a window may only take focus with an activation token, which the launcher passes to
the *new* process as `XDG_ACTIVATION_TOKEN`. A second `leansip` forwards that token to the
running one, so launches from the app grid, the `.desktop` file or `gio open tel:…` bring the
dialer to the front. A plain terminal launch carries no token; the compositor then flags the
window ("leansip is ready") instead of focusing it.

`leansip --connect host:port` attaches to a baresip you started yourself instead of a leansipd.

## Source layout

- `src/core/` — Qt Core/Network only, unit-tested: netstring framing, `leansip.toml`
  (`settings`), the account line, config derivation, ctrl_tcp client, call state machine,
  single-instance, the leansipd proxy (`ctrl_proxy`) and its client/launcher (`daemon_client`),
  WAV handling and speech detection, stream format and recordings (`media_control`), the
  audio activity of a growing recording for the dialer (`call_audio`), call
  analysis, the ElevenLabs client.
- `src/ui/` — the dialer widget.
- `src/daemon/` — `leansipd`; `src/leansipctl/` — `leansipctl`.
- `tests/` — one QtTest executable per unit, run with the offscreen platform, plus
  `integration_loopback.sh`: two daemons and a real baresip calling each other on 127.0.0.1,
  with a fake ElevenLabs server (`fake_elevenlabs.py`). `LEANSIP_SCREENSHOT_DIR=<dir>
  ./build/tests/tst_main_window screenshots` saves PNGs of the dialer.

## Releasing

Set `project(leansip VERSION x.y.z)` in `CMakeLists.txt`, commit, and push a tag `vx.y.z`.
`.github/workflows/release.yml` builds in an Ubuntu 26.04 container (the oldest release with
baresip 1.1.0, which leansip needs for call ids), runs all tests
including the loopback call test, packages with CPack, installs and runs the `.deb`, and
publishes the release with `SHA256SUMS` and build provenance attestations.
