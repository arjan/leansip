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

Only one instance runs per user. A second invocation with a number hands it to
the open window and exits, so `leansip %u` works as a `tel:` URL handler.

The UI shows the **first** account from `~/.baresip/accounts`.

## Keys

The window is frameless and entirely keyboard driven (drag anywhere to move it).

| Key          | Action                                                   |
|--------------|----------------------------------------------------------|
| `0-9 * # +`  | type a number; sends DTMF during a call                  |
| `Enter`      | call · answer                                            |
| `Esc`        | hang up · decline · cancel queued dial · clear · quit when empty |
| `↑`          | recall the last number dialled                           |
| `?` / `F1`   | help screen (`Esc` closes it)                            |
| `Ctrl+Q`     | quit                                                     |

## How it talks to baresip

leansip never edits `~/.baresip`. On start it writes a derived config to
`$XDG_RUNTIME_DIR/leansip/`:

- your `config` with the console UI modules (`stdio`, `cons`, `gtk`) removed,
- `ctrl_tcp` enabled on `127.0.0.1:<random port>` (never `0.0.0.0`),
- symlinks to your `accounts`, `contacts` and `uuid` so credentials stay in one place.

It then runs `baresip -f <that dir>` and stops it on exit. Your password is never
read by leansip; the accounts parser only extracts the display name and SIP address.

Stop any baresip you already have running first, otherwise two user agents
register the same account.

### Security note

The `ctrl_tcp` interface has no authentication. Binding it to loopback keeps it
off the network, but any process running as any user on the same machine can
connect to it. This is the same trust level as the baresip console modules.

## Build

Requires Qt 6.4+ (Core, Network, Widgets, Test), CMake 3.22+, a C++20 compiler and
a `baresip` binary with the `ctrl_tcp` module (`baresip-core` on Debian/Ubuntu).

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
./build/src/leansip
```

## Layout

- `src/core/` — Qt Core/Network only, unit-tested: netstring framing, accounts
  parser, config derivation, ctrl_tcp client, call state machine, single-instance.
- `src/ui/` — the dialer widget.
- `tests/` — one QtTest executable per unit, run with the offscreen platform.
