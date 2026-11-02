# Configuration

All profiles live in `~/.config/leansip/leansip.toml`; `$LEANSIP_CONFIG` or `--config` points at
another file. A profile is one SIP account and runs its own leansipd and baresip.

The file can hold passwords, so leansip refuses to read it unless it belongs to you and is not
readable by anyone else (`chmod 600`). Unknown keys are errors, so a typo never silently drops a
setting.

## Which profile

`--profile`/`-p` if given, else `$LEANSIP_PROFILE`, else `default_profile`, else the only profile
in the file.

## A minimal profile

Most profiles need only the address, the password and, behind a proxy, the outbound proxy:

```toml
[profiles.desk]
aor = "sip:phone1@pbx.example.com"
password_cmd = "rbw get sip-desk"
outbound = "sip:127.0.0.1:5060"    # only when the provider needs one
```

Everything else has a default: UDP for `sip:` addresses, the user part of `aor` as `auth_user`,
an instance id derived per machine (see "Instance id"), and the built-in baresip settings (see
"baresip settings"). Add a `[profiles.<name>.baresip]` table only for what should differ.

## Example with every option

```toml
default_profile = "desk"

[baresip]                          # baresip settings for every profile
audio_player = "pulse,"
audio_source = "pulse,"

[profiles.desk]
display_name = "Desk"              # optional; the SIP display name the far end sees
label = "Desk phone"               # optional; the dialer's name for it (default: display_name,
                                   #   else the address, which stays in the label's tooltip)
shortcut = 1                       # optional; Ctrl+1 switches the dialer to this profile
aor = "sip:phone1@pbx.example.com" # sip:user@domain[:port], or sips:
transport = "udp"                  # optional: udp, tcp or tls
auth_user = "phone1"               # optional; defaults to the user part
password_cmd = "rbw get sip-desk"  # or: password = "..."
outbound = "sip:127.0.0.1:5060"    # optional outbound proxy
account_params = { audio_codecs = "opus/48000/2,PCMA/8000/1", regint = 600 }
uuid = "..."                       # optional; see "Instance id"
record = false                     # record every call (like --record)
record_dir = "~/leansip-calls"     # optional; default <runtime dir>/recordings
confirm_dial = false               # true: `leansipctl dial` needs --yes (for real trunks)

[profiles.desk.baresip]            # this profile only; wins over [baresip]
jitter_buffer_delay = "5-10"
module_app = ["account.so", "contact.so", "debug_cmd.so", "menu.so", "netroam.so"]
```

## Switching profiles in the dialer

Ctrl+<digit> in the dialer switches its window to the profile with that `shortcut` (each digit at
most once in the file). The file is read again at that moment, so a new shortcut works without
restarting. The old profile's leansipd stops, and unregisters, if the dialer started it; one that
`leansipctl start` started keeps running. Switching waits until there is no call, and when another
dialer window already serves the profile, that window comes to the front instead.

## Passwords

`password` holds the password itself. `password_cmd` runs with `/bin/sh -c` each time leansipd
starts baresip; the first line it prints is the password, and a failing command stops the start.
It may take up to a minute, for instance while a password manager asks to be unlocked; after
that it is killed together with anything it started. A password must not contain spaces, quotes
or `; < >`.

## Instance id

baresip sends an instance id (`+sip.instance`) with each registration, so the registrar can tell
a restarted phone from a second one. Without `uuid`, leansip derives it from `/etc/machine-id`,
the profile name and `aor`: it stays the same across restarts and reboots and differs on another
machine that uses the same file. Renaming the profile or changing `aor` gives a new one; the
registrar then keeps the old binding until it expires. Only a hash of the machine id is sent.
Set `uuid` to pin a specific id, for instance one an older baresip setup used.

## Account parameters

`account_params` adds parameters to baresip's account line: codecs (`audio_codecs`), the
registration interval (`regint`, 0 = never register), `medianat`, `answermode` and so on.
`auth_user`, `auth_pass`, `outbound` and `transport` are set on the profile itself.

## baresip settings

leansip starts from a built-in baresip configuration: audio over ALSA at 48 kHz, opus, G.722 and
G.711, STUN/TURN/ICE. `[baresip]` and `[profiles.<name>.baresip]` change it:

- A key replaces every line with that name; a profile's key wins over the shared one.
- A list gives one line per element: `module = ["g711.so", "aufile.so"]` replaces the whole
  module list, while `module_app = [...]` leaves `module` alone.
- An empty list removes the setting: `opus_bitrate = []`.
- `true`/`false` become `yes`/`no`; numbers are written as they are.

leansip always adds what it needs itself (its control connection, call and account modules,
`aufile` for playing audio into calls, `sndfile` when recording) and removes console UI modules.
A `sip_cafile` that does not exist is left out, since baresip would not start with it. Without
CA certificates baresip accepts any TLS server certificate, so a profile that uses TLS (`sips:`,
`transport = "tls"` or a `sips:` outbound proxy) refuses to start unless `sip_cafile` names an
existing file or `sip_capath` an existing directory. The default is
`/etc/ssl/certs/ca-certificates.crt`, from the `ca-certificates` package.

The settings themselves are baresip's: see the comments in the `config` file baresip writes on
its first run, or [baresip's documentation](https://github.com/baresip/baresip/wiki).

## Applying changes

Edits apply the next time leansipd starts baresip: close the dialer (or `leansipctl stop`), or
run `leansipctl raw leansipd_restart`, which reloads the profile and keeps the running baresip if
the file has an error. `--record` and `record_dir` only change with a new daemon.

## Test profiles without a SIP server

Two profiles can call each other on the loopback interface without registering and without a
sound card; see "Local test calls without a SIP server" in [leansipctl.md](leansipctl.md).
