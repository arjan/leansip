# leansip

A minimal Linux SIP dialer. It does no SIP or audio work itself: it spawns
[baresip](https://github.com/baresip/baresip) with your existing `~/.baresip`
configuration and drives it over its `ctrl_tcp` JSON interface.

## Usage

```
leansip                    # open the dialer
leansip 0612345678         # open and dial immediately
leansip tel:+31201234567   # tel: and sip: URIs are accepted
leansip --connect 127.0.0.1:4444   # attach to a baresip you started yourself
leansip --baresip-dir ~/other-baresip
```

Only one instance runs per user. A second invocation hands its number (or just a
"raise" request) to the open window and exits, so `leansip %u` works as a `tel:`
URL handler.

On Wayland a window may only take focus with an activation token, which the
launcher passes to the *new* process as `XDG_ACTIVATION_TOKEN`. leansip forwards
that token to the running instance, so launches from the app grid, a `.desktop`
file or `gio open tel:…` bring the dialer to the front. A plain terminal launch
carries no token; the compositor then flags the window ("leansip is ready")
instead of focusing it.

The UI shows the **first** account from `~/.baresip/accounts`.

## Keys

The window is frameless and entirely keyboard driven (drag anywhere to move it).

| Key          | Action                                                   |
|--------------|----------------------------------------------------------|
| `0-9 * # +`  | type a number; during a call the field collects the DTMF digits you send |
| `Enter`      | call · answer                                            |
| `Esc`        | hang up · decline · cancel queued dial · clear · quit when empty |
| `↑`          | recall the last number dialled                           |
| `?` / `F1`   | help screen (`Esc` closes it)                            |
| `F2`         | accounts screen (see below)                              |
| `Ctrl+Q`     | quit                                                     |

### Accounts screen (F2)

Edits `~/.baresip/accounts`. The list is the file order and the **first entry is the
account the dialer uses**, so switching accounts is just moving one to the top.

| Key              | Action                                                        |
|------------------|---------------------------------------------------------------|
| `↑` `↓`          | select an account; the form below edits it                    |
| `Enter`          | make the selected account active (moves it to the top)        |
| `Ctrl+↑` `Ctrl+↓`| move the selected account                                     |
| `Ctrl+N` / `Del` | add / remove an account                                       |
| `Ctrl+H`         | show or hide the password                                     |
| `Ctrl+S`         | save: the edited file is handed to a restarted baresip and the active account must actually register; only then is `~/.baresip/accounts` rewritten |
| `Esc`            | abort a running validation, or close discarding changes       |

Fields: display name, user, domain, transport, auth user, password, outbound proxy. Anything
else already on an account line (codecs, `regint`, …) is kept verbatim. The screen opens by
itself when there is no account yet. Saving is refused during a call, because validation
restarts baresip.

## How it talks to baresip

leansip never edits `~/.baresip`. On start it writes a derived config to
`$XDG_RUNTIME_DIR/leansip/`:

- your `config` with the console UI modules (`stdio`, `cons`, `gtk`) removed,
- `ctrl_tcp` enabled on `127.0.0.1:<random port>` (never `0.0.0.0`),
- `audio_path` pointed at a private `sounds/` directory holding baresip's stock WAVs
  (symlinked) plus generated tones: 16 DTMF digits, played via `debug_cmd`'s `play`
  command whenever you send one, and an "invalid number" tone (the ITU special information
  tone) written as `error.wav`/`notfound.wav` so baresip's own failed-call handling and
  leansip's local rejections sound the same,
- symlinks to your `accounts`, `contacts` and `uuid` so credentials stay in one place.

It then runs `baresip -f <that dir>` and stops it on exit. Your password is never
read by leansip; the accounts parser only extracts the display name and SIP address.

Stop any baresip you already have running first, otherwise two user agents
register the same account.

### Security note

The `ctrl_tcp` interface has no authentication. Binding it to loopback keeps it
off the network, but any process running as any user on the same machine can
connect to it. This is the same trust level as the baresip console modules.

## Build (Ubuntu)

Tested on Ubuntu 26.04 (Qt 6.10, baresip 1.1.0). Needs Qt 6.4+ and CMake 3.22+;
`baresip-core` provides the `baresip` binary with the `ctrl_tcp` and `debug_cmd`
modules used at runtime.

```
sudo apt install build-essential cmake ninja-build qt6-base-dev baresip-core
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
./build/src/leansip
```

On a machine without `~/.baresip`, leansip lets baresip write its template config on
first start and then opens the accounts screen so you can enter your SIP account. To
install system-wide:

```
sudo cmake --install build
```

## Layout

- `src/core/` — Qt Core/Network only, unit-tested: netstring framing, accounts
  parser, config derivation, ctrl_tcp client, call state machine, single-instance.
- `src/ui/` — the dialer widget.
- `tests/` — one QtTest executable per unit, run with the offscreen platform.
