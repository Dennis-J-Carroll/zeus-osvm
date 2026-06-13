#!/usr/bin/env bash
#
# smoke_test.sh - one-shot end-to-end test for Zeus_osVM.
#
# Builds the project, runs unit tests, and exercises each example
# (hello, boot splash, echo server, http server, raw ping). Prints a
# PASS/FAIL summary and exits non-zero if anything required failed.
#
# Usage:  ./smoke_test.sh
# Env:    ECHO_PORT (default 12345)  HTTP_PORT (default 8080)
#         SKIP_NET=1  -> skip the network server tests
#         SKIP_RAW=1  -> skip the raw-ping test

set -u

cd "$(dirname "$0")"

ECHO_PORT="${ECHO_PORT:-12345}"
HTTP_PORT="${HTTP_PORT:-8080}"

# --- pretty output -----------------------------------------------------------
if [ -t 1 ]; then
    G='\033[32m'; R='\033[31m'; Y='\033[33m'; B='\033[1m'; Z='\033[0m'
else
    G=''; R=''; Y=''; B=''; Z=''
fi

PASS=0; FAIL=0; SKIP=0
pass() { printf "  ${G}PASS${Z} %s\n" "$1"; PASS=$((PASS+1)); }
fail() { printf "  ${R}FAIL${Z} %s\n" "$1"; FAIL=$((FAIL+1)); }
skip() { printf "  ${Y}SKIP${Z} %s\n" "$1"; SKIP=$((SKIP+1)); }
hdr()  { printf "\n${B}== %s ==${Z}\n" "$1"; }

# --- cleanup of any spawned servers -----------------------------------------
PIDS=()
cleanup() { for p in "${PIDS[@]:-}"; do kill "$p" 2>/dev/null; done; }
trap cleanup EXIT INT TERM

# ----------------------------------------------------------------------------
hdr "Build"
if make >/tmp/zeus_build.log 2>&1; then
    pass "make"
else
    fail "make (see /tmp/zeus_build.log)"
    tail -20 /tmp/zeus_build.log
    echo; echo "Build failed - aborting."; exit 1
fi

hdr "Unit tests"
if make test >/tmp/zeus_test.log 2>&1; then
    pass "make test ($(grep -ho '[0-9]*/[0-9]* tests passed' /tmp/zeus_test.log | paste -sd' '))"
else
    fail "make test (see /tmp/zeus_test.log)"
    tail -20 /tmp/zeus_test.log
fi

hdr "Hello"
OUT="$(./zeus run-asm examples/hello.zasm 2>&1)"
if printf '%s' "$OUT" | grep -q "Hello, Zeus_osVM!"; then
    pass "hello prints greeting"
else
    fail "hello (got: $OUT)"
fi

hdr "Assemble / disassemble round-trip"
if ./zasm examples/hello.zasm /tmp/hello.zeus >/dev/null 2>&1 \
   && ./zeus run /tmp/hello.zeus 2>&1 | grep -q "Hello" \
   && ./zdis /tmp/hello.zeus 2>&1 | grep -q "HALT"; then
    pass "zasm -> run -> zdis"
else
    fail "round-trip"
fi

hdr "Boot splash"
if ./zeus run-asm examples/boot_splash.zasm >/tmp/zeus_splash.log 2>&1 \
   && [ -s /tmp/zeus_splash.log ]; then
    pass "boot splash renders ($(wc -l </tmp/zeus_splash.log) lines)"
else
    fail "boot splash"
fi

# --- network tests ----------------------------------------------------------
if [ "${SKIP_NET:-0}" = "1" ]; then
    hdr "Network servers"; skip "SKIP_NET=1"
elif ! command -v nc >/dev/null 2>&1; then
    hdr "Network servers"; skip "nc not installed"
else
    hdr "TCP echo server (port $ECHO_PORT)"
    ./zeus run-asm examples/echo_server.zasm >/tmp/zeus_echo.log 2>&1 &
    PIDS+=($!)
    sleep 1
    REPLY="$(printf 'ping zeus\n' | nc -w2 127.0.0.1 "$ECHO_PORT" 2>/dev/null)"
    if printf '%s' "$REPLY" | grep -q "ping zeus"; then
        pass "echo server echoes input"
    else
        fail "echo server (got: '$REPLY' - port $ECHO_PORT in use or wrong?)"
    fi

    hdr "HTTP server (port $HTTP_PORT)"
    ./zeus run-asm examples/http_server.zasm >/tmp/zeus_http.log 2>&1 &
    PIDS+=($!)
    sleep 1
    if command -v curl >/dev/null 2>&1; then
        BODY="$(curl -s -m2 "http://127.0.0.1:$HTTP_PORT/" 2>/dev/null)"
    else
        BODY="$(printf 'GET / HTTP/1.0\r\n\r\n' | nc -w2 127.0.0.1 "$HTTP_PORT" 2>/dev/null)"
    fi
    if [ -n "$BODY" ]; then
        pass "http server responds ($(printf '%s' "$BODY" | wc -c) bytes)"
    else
        fail "http server (no response - port $HTTP_PORT in use?)"
    fi
fi

# --- raw ping (needs CAP_NET_RAW / root) ------------------------------------
hdr "Raw ping"
if [ "${SKIP_RAW:-0}" = "1" ]; then
    skip "SKIP_RAW=1"
elif [ "$(id -u)" -ne 0 ]; then
    # Unprivileged: opcodes should degrade to -1 without crashing.
    if ./zeus run-asm examples/raw_ping.zasm >/tmp/zeus_raw.log 2>&1; then
        pass "raw ping degrades gracefully unprivileged (run as root to send real packets)"
    else
        fail "raw ping crashed unprivileged (should return -1, not crash)"
    fi
else
    if ./zeus run-asm examples/raw_ping.zasm >/tmp/zeus_raw.log 2>&1; then
        pass "raw ping sent (root)"
    else
        fail "raw ping (root, see /tmp/zeus_raw.log)"
    fi
fi

# --- summary ----------------------------------------------------------------
printf "\n${B}Summary:${Z} ${G}%d passed${Z}, ${R}%d failed${Z}, ${Y}%d skipped${Z}\n" \
    "$PASS" "$FAIL" "$SKIP"

[ "$FAIL" -eq 0 ]
