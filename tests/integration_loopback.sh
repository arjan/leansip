#!/usr/bin/env bash
# End to end with a real baresip: two leansipd daemons (alice, bob) call each other on
# 127.0.0.1 with accounts that never register, and no sound card (aufile sources and players).
# Covers dial/answer, stream format, say -> listen, DTMF, analyze, hangup by
# call id, and stop/start reusing the same SIP port.
#
#   integration_loopback.sh <dir with sipctl and leansipd>
#
# Speech goes to a local fake ElevenLabs server. With LEANSIP_E2E_REAL_ELEVENLABS=1 and
# ELEVENLABS_API_KEY in the environment it uses the real service instead, e.g.
#   with-secrets botsi-dev-env -- env LEANSIP_E2E_REAL_ELEVENLABS=1 ctest -R integration -V
# Exit 77 (skipped) without baresip. LEANSIP_E2E_KEEP=1 keeps the temp directory.
set -euo pipefail

BIN=${1:?usage: integration_loopback.sh <bin dir>}
HERE=$(cd -- "$(dirname -- "$0")" >/dev/null && pwd) # cd echoes the path when CDPATH is set
command -v baresip >/dev/null || { echo "baresip not installed: skipping"; exit 77; }
[ -d /usr/lib/baresip/modules ] || { echo "no /usr/lib/baresip/modules: skipping"; exit 77; }

T=$(mktemp -d)
SERVER_PID=
cleanup() {
    "$BIN/sipctl" -d "$T/alice" stop >/dev/null 2>&1 || true
    "$BIN/sipctl" -d "$T/bob" stop >/dev/null 2>&1 || true
    [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null || true
    if [ -n "${LEANSIP_E2E_KEEP:-}" ]; then echo "kept $T"; else rm -rf "$T"; fi
}
trap cleanup EXIT

# Keep every daemon's socket, log and recordings inside the test directory.
export XDG_RUNTIME_DIR="$T/run"
mkdir -m 700 "$XDG_RUNTIME_DIR"

PORT=$((20000 + RANDOM % 9000))
python3 - "$T/silence.wav" <<'EOF'
import sys, wave
w = wave.open(sys.argv[1], "wb"); w.setnchannels(1); w.setsampwidth(2); w.setframerate(8000)
w.writeframes(b"\0\0" * 8000 * 300); w.close()
EOF

profile() { # name port source
    mkdir -p "$T/$1"
    cat >"$T/$1/config" <<EOF
module_path		/usr/lib/baresip/modules
sip_listen		127.0.0.1:$2
module			g711.so
module			aufile.so
audio_player		aufile,$T/$1/heard.wav
audio_source		$3
EOF
    printf '<sip:%s@127.0.0.1>;regint=0\n' "$1" >"$T/$1/accounts"
    chmod 600 "$T/$1/accounts"
}
profile alice "$PORT" "aufile,$T/silence.wav"
profile bob $((PORT + 1)) "aufile,$T/silence.wav" # ausine only does 48 kHz; PCMU is 8 kHz

if [ "${LEANSIP_E2E_REAL_ELEVENLABS:-}" = 1 ]; then
    [ -n "${ELEVENLABS_API_KEY:-}" ] || { echo "LEANSIP_E2E_REAL_ELEVENLABS=1 needs ELEVENLABS_API_KEY"; exit 1; }
    unset ELEVENLABS_BASE_URL
    echo "speech: real ElevenLabs"
else
    coproc FAKE { exec python3 -u "$HERE/fake_elevenlabs.py" 2>/dev/null; }
    SERVER_PID=$FAKE_PID # bash unsets FAKE_PID once the coprocess exits
    read -r FAKE_PORT <&"${FAKE[0]}"
    export ELEVENLABS_API_KEY=test-key ELEVENLABS_BASE_URL="http://127.0.0.1:$FAKE_PORT"
    echo "speech: fake ElevenLabs on port $FAKE_PORT"
fi

a() { "$BIN/sipctl" -d "$T/alice" "$@"; }
b() { "$BIN/sipctl" -d "$T/bob" "$@"; }
step() { echo "--- $*"; }
fail() {
    echo "FAIL: $*"
    for log in "$XDG_RUNTIME_DIR"/leansip*/leansipd.log; do
        echo "== $log"
        grep -av "audio=" "$log" | tail -25
    done
    exit 1
}
expect() { # description, jq filter, json
    echo "$3" | jq -e "$2" >/dev/null || fail "$1: $3"
}

step start both daemons
expect "alice start" '.ok and .status.baresipUp and .status.recording' "$(a start --record)"
expect "bob start" '.ok and .status.baresipUp and .status.recording' "$(b start --record --max-call-seconds 60)"

step call bob
out=$(a dial "sip:bob@127.0.0.1:$((PORT + 1))")
expect "dial" '.ok and (.call | length > 0)' "$out"
CALL=$(echo "$out" | jq -r .call)
SEQ=$(echo "$out" | jq -r .seq)
out=$(b wait CALL_INCOMING --since 0 --timeout 10)
expect "incoming" '.type == "CALL_INCOMING"' "$out"
BCALL=$(echo "$out" | jq -r .id)
expect "answer" '.ok' "$(b answer --call "$BCALL")"
expect "established" '.type == "CALL_ESTABLISHED"' "$(a wait CALL_ESTABLISHED --call "$CALL" --since "$SEQ" --timeout 10)"
expect "stream format" '.codec == "PCMU" and .srate == 8000 and .channels == 1' \
    "$(a wait CALL_AUDIO --call "$CALL" --since "$SEQ" --timeout 10)"

step "say -> listen"
b listen --timeout 20 >"$T/listen.json" &
LISTEN=$!
sleep 1
expect "say" '.ok and .ended == "done"' "$(a say "Testing one two three, this is alice." --wait)"
wait "$LISTEN" || fail "listen: $(cat "$T/listen.json")"
heard=$(cat "$T/listen.json")
echo "heard: $(echo "$heard" | jq -c '{text, seconds}')"
expect "listen" '.ok and .seconds > 0.5 and (.text | length > 0)' "$heard"
if [ "${LEANSIP_E2E_REAL_ELEVENLABS:-}" = 1 ]; then
    expect "transcript" '.text | ascii_downcase | test("three|alice|testing")' "$heard"
else
    expect "transcript" '.text == "hello from the fake speech service"' "$heard"
fi
expect "call survives playback" '.data | test("ESTABLISHED")' "$(a calls)"

step dtmf
expect "dtmf" '.ok' "$(a dtmf '1#' --call "$CALL")"
expect "digit #" '.param == "#"' "$(b wait CALL_DTMF_START --param '#' --since 0 --timeout 5)"

step hangup and analyze
expect "hangup" '.ok' "$(a hangup --call "$CALL")"
expect "closed" '.type == "CALL_CLOSED"' "$(b wait CALL_CLOSED --call "$BCALL" --since 0 --timeout 10)"
out=$(b analyze --call "$BCALL")
echo "bob analyze: $(echo "$out" | jq -c '{dtmfReceived, closeReason, timing, levels}')"
expect "analyze bob" '.ok and .dtmfReceived == "1#" and .state == "closed" and .levels.received.seconds > 1' "$out"
out=$(a analyze)
expect "analyze alice" '.ok and .id == "'"$CALL"'" and .audio.codec == "PCMU" and (.timing.answeredAfterMs >= 0)' "$out"

step restart baresip through the daemon, as the account editor does
seq=$(a status | jq -r .data.seq)
expect "restart" '.ok' "$(a raw leansipd_restart)"
expect "back up" '.type == "BACKEND_UP"' "$(a wait BACKEND_UP --since "$seq" --timeout 10)"
expect "accounts after restart" '.data | test("alice@127.0.0.1")' "$(a reginfo)"

step stop and start again on the same port
expect "stop" '.ok' "$(a stop)"
expect "restart" '.ok and .status.baresipUp' "$(a start)"
expect "stop again" '.ok' "$(a stop)"
expect "bob stop" '.ok' "$(b stop)"

echo "PASS"
