# leansip

A minimal Linux SIP dialer. It does no SIP or audio work itself: it runs
[baresip](https://github.com/baresip/baresip) with a config generated from one
`leansip.toml` profile and drives it over its `ctrl_tcp` JSON interface.

## Usage

```
leansip                    # open the dialer
leansip 0612345678         # open and dial immediately
leansip tel:+31201234567   # tel: and sip: URIs are accepted
leansip --connect 127.0.0.1:4444   # attach to a baresip you started yourself
leansip -p work                    # another profile from leansip.toml
leansip --record                   # record calls (when this window starts leansipd)
```

One instance runs per profile. A second invocation for the same profile hands its number
(or just a "raise" request) to the open window and exits, so `leansip %u` works as a `tel:`
URL handler. `leansip -p work` opens a separate dialer for that profile next to the default
one; each gets its own baresip and runtime directory under `$XDG_RUNTIME_DIR`, and baresip
picks random SIP ports unless the profile sets `sip_listen`.

On Wayland a window may only take focus with an activation token, which the
launcher passes to the *new* process as `XDG_ACTIVATION_TOKEN`. leansip forwards
that token to the running instance, so launches from the app grid, a `.desktop`
file or `gio open tel:…` bring the dialer to the front. A plain terminal launch
carries no token; the compositor then flags the window ("leansip is ready")
instead of focusing it.

## Configuration

All profiles live in `~/.config/leansip/leansip.toml` (or `$LEANSIP_CONFIG`, or `--config`).
A profile is one SIP account. The file can hold passwords, so leansip refuses to read it unless
it belongs to you and is not readable by anyone else (`chmod 600`).

```toml
default_profile = "desk"           # used without --profile (and without $LEANSIP_PROFILE)

[baresip]                          # baresip config settings for every profile
audio_player = "pulse,"
audio_source = "pulse,"

[profiles.desk]
display_name = "Desk"              # optional; shown in the dialer
aor = "sip:phone1@pbx.example.com" # sip:user@domain[:port], or sips:
transport = "udp"                  # optional: udp, tcp or tls
auth_user = "phone1"               # optional; defaults to the user part
password_cmd = "rbw get sip-desk"  # or: password = "..."
outbound = "sip:127.0.0.1:5060"    # optional outbound proxy
account_params = { audio_codecs = "opus/48000/2,PCMA/8000/1", regint = 600 }
uuid = "..."                       # optional; keeps baresip's instance id stable
record = false                     # record every call (like --record)
record_dir = "~/leansip-calls"     # optional; default <runtime dir>/recordings
confirm_dial = false               # true: `leansipctl dial` needs --yes (for real trunks)

[profiles.desk.baresip]            # this profile only; wins over [baresip]
jitter_buffer_delay = "5-10"
module_app = ["account.so", "contact.so", "debug_cmd.so", "menu.so", "netroam.so"]
```

- **baresip settings** start from a built-in template: audio over ALSA at 48 kHz, opus, G.722
  and G.711, STUN/TURN/ICE. A key replaces every template line with that name, a list gives one
  line per element, and an empty list (`opus_bitrate = []`) removes the setting. `true`/`false`
  become `yes`/`no`. So `module = [...]` replaces the whole module list, while
  `module_app = [...]` leaves `module` alone.
- **password_cmd** runs with `/bin/sh -c` each time leansipd starts baresip. The first line it
  prints is the password; a failing command stops the start. It may take up to a minute, for
  instance while a password manager asks to be unlocked.
- Unknown keys are errors, so a typo never silently drops a setting.
- Edits apply the next time leansipd starts baresip: close the dialer (or `leansipctl stop`), or
  run `leansipctl raw leansipd_restart`, which reloads the profile and keeps the old baresip running
  if the file has an error. `--record` and `record_dir` only change with a new daemon.

## Keys

The window is frameless and entirely keyboard driven (drag anywhere to move it). It looks like a
small handset: a beige case with a border and a green LCD for the number and status, the same in
light and dark desktop themes. The LCD's corner shows `MUTE` and `+N CALLS` (calls besides the
one shown, e.g. an agent's).

| Key          | Action                                                   |
|--------------|----------------------------------------------------------|
| `0-9 * # +`  | type a number; during a call the field collects the DTMF digits you send |
| `Enter`      | call · answer                                            |
| `Esc`        | hang up · decline · cancel queued dial · clear · quit when empty |
| `↑`          | recall the last number dialled                           |
| `m`          | mute · unmute the microphone during a call               |
| `?` / `F1`   | help screen (`Esc` closes it)                            |
| `Ctrl+Q`     | quit                                                     |

## How it talks to baresip

The dialer does not run baresip itself. `leansipd`, a small daemon, owns one baresip per
profile and shares it: the dialer is one client, `leansipctl` (below) and anything
else are others. All clients see the same calls and events, so an agent can watch or drive
a call you are on, and you can hang up a call an agent placed.

- The dialer starts `leansipd --ui-owned` when none is running, connects, and binds to it.
  It identifies itself as the dialer, so its calls have no time limit, and it
  selects its own call and account by id for every command, since baresip's "current call"
  is shared with the other clients.
  That daemon stops (and with it baresip, unregistering) when the dialer closes or crashes,
  even if leansipctl clients are still connected.
- A daemon started by `leansipctl start` keeps running until `leansipctl stop`; a dialer that
  attaches to it leaves it running when it closes, and hangs up only a call it dialled or
  answered itself.
- Clients use `$XDG_RUNTIME_DIR/leansip-<profile>/leansipd.sock`, a Unix socket in a 0700
  directory. They speak baresip's own `ctrl_tcp` protocol (netstring-framed JSON); the
  daemon routes each response back to the client that asked, broadcasts every event with a
  running `"seq"` and a `"ts"`, and keeps the last 2000 events for clients that connect later.
  It refuses `quit` and adds its own commands: `leansipd_seq` (several commands back to
  back, e.g. `callfind` + `hangup`), `leansipd_play`, `leansipd_recordings`,
  `leansipd_status`, `leansipd_events <since>`, `leansipd_restart` (reloads the profile),
  `leansipd_hello ui`, `leansipd_bind` and `leansipd_shutdown`.
- The daemon's output, and baresip's, goes to `leansipd.log` in the same directory
  (rotated at 5 MB). It restarts baresip if baresip crashes.

leansipd writes baresip's config directory into that runtime directory:

- `config`: the template with the profile's settings, minus console UI modules (`stdio`,
  `cons`, `gtk`) and plus the modules leansip needs (`account`, `menu`, `debug_cmd`, `aufile`),
- `ctrl_tcp` enabled on `127.0.0.1:<random port>` (never `0.0.0.0`),
- `audio_path` pointed at a private `sounds/` directory holding baresip's stock WAVs
  (symlinked) plus generated tones: 16 DTMF digits, played via `debug_cmd`'s `play`
  command whenever you send one, and an "invalid number" tone (the ITU special information
  tone) written as `error.wav`/`notfound.wav` so baresip's own failed-call handling and
  leansip's local rejections sound the same,
- `accounts`: the profile's account line, with the password (mode 0600, removed when
  leansipd exits), and `uuid` when the profile sets one.

It then runs `baresip -f <that dir>` and stops it on exit. The password is never logged or
shown by leansip, leansipctl or the dialer.

Stop any baresip you already have running first, otherwise two user agents
register the same account.

### Security note

Only your user can open the leansipd socket, but any process running as you can then place
calls on your accounts. baresip's own `ctrl_tcp` interface, which leansipd uses, has no
authentication; binding it to loopback keeps it off the network, but any local user can
connect to it. This is the same trust level as the baresip console modules.

## leansipctl: scripting and agents

`leansipctl` talks to leansipd and prints one JSON object per line. `-p NAME` picks the profile;
`leansipctl profiles` lists them.

```
leansipctl start --record                # also --sip-trace, --max-call-seconds
r=$(leansipctl dial 940)                 # {"ok":true,"seq":12,"call":"…","data":"call id: …"}
leansipctl wait CALL_ESTABLISHED,CALL_CLOSED --since "$(echo "$r" | jq .seq)" --timeout 20
leansipctl listen                        # transcript of what the far end says next
leansipctl say "I'd like to speak to support" --wait
leansipctl dtmf 1#
leansipctl analyze                       # timing, codec, RTCP, levels, warnings
leansipctl hangup
leansipctl stop
```

Speech (`say`, `listen`, `transcribe`) uses ElevenLabs and needs `ELEVENLABS_API_KEY`. leansipctl
hangs up its own calls after 10 minutes by default; the dialer's calls have no limit.

`leansipctl help` lists the commands, `leansipctl help <command>` explains one. The full reference is
[docs/leansipctl.md](docs/leansipctl.md).

## Install

Releases on GitHub carry a `.deb` and a `.tar.gz` for x86-64 Linux, built on Ubuntu 24.04. Both
use the system's Qt 6 (6.4 or newer) and need baresip:

```
sudo apt install ./leansip_<version>_amd64.deb     # pulls in baresip-core and Qt
```

The tarball holds `bin/` and `share/` to unpack anywhere; install `baresip-core` and Qt 6 yourself.
`SHA256SUMS` lists the checksums, and `gh attestation verify <file> --repo arjan/leansip` checks
that a file was built by this repository's release workflow.

Releasing: set `project(leansip VERSION x.y.z)` in `CMakeLists.txt`, commit, and push a tag
`vx.y.z`. `.github/workflows/release.yml` builds, runs all tests, packages with CPack and
publishes the release. `cpack` in a build directory makes the same packages locally.

## Build (Ubuntu)

Tested on Ubuntu 26.04 (Qt 6.10, baresip 1.1.0). Needs Qt 6.4+ and CMake 3.22+;
`baresip-core` provides the `baresip` binary with the `ctrl_tcp` and `debug_cmd`
modules used at runtime.

```
sudo apt install build-essential cmake ninja-build qt6-base-dev libtomlplusplus-dev baresip-core
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
./build/src/leansip            # finds ./build/src/leansipd next to it
```

Write `~/.config/leansip/leansip.toml` (see Configuration) before the first start. To
install system-wide:

```
sudo cmake --install build
```

## Layout

- `src/core/` — Qt Core/Network only, unit-tested: netstring framing, `leansip.toml`
  (`settings`), the account line, config derivation, ctrl_tcp client, call state machine, single-instance,
  the leansipd proxy (`ctrl_proxy`) and its client/launcher (`daemon_client`), WAV
  handling and speech detection, stream format and recordings (`media_control`), call
  analysis, the ElevenLabs client.
- `src/ui/` — the dialer widget.
- `src/daemon/` — `leansipd`; `src/leansipctl/` — `leansipctl`.
- `tests/` — one QtTest executable per unit, run with the offscreen platform, plus
  `integration_loopback.sh`: two daemons and a real baresip calling each other on 127.0.0.1,
  with a fake ElevenLabs server (`fake_elevenlabs.py`). `LEANSIP_SCREENSHOT_DIR=<dir>
  ./build/tests/tst_main_window screenshots` saves PNGs of the dialer.
