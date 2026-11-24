#!/usr/bin/env bash
#
# End-to-end proof of journal integrity through the real ./zeus binary.
# Prints a Markdown transcript on stdout; exits 1 if any check fails.
#
# Generated with:  bash tests/journal_transcript.sh > docs/journal-transcript.md

set -u
cd "$(dirname "$0")/.."

WORK=$(mktemp -d)
FAIL=0
trap 'rm -rf "$WORK"' EXIT

# Provenance: the transcript must come from the binary built after the last
# PR C source edit. `find -newer` compares with sub-second mtimes, so this does
# not depend on the one-second resolution of `stat`/`date`.
PRC_SRCS="src/journal.c src/journal.h src/supervisor.c src/snapshot.c src/main.c include/supervisor.h include/snapshot.h Makefile"
BIN=./zeus
if [ ! -x "$BIN" ]; then
    printf 'error: %s is missing; run make first\n' "$BIN" >&2
    exit 1
fi
BIN_HASH=$(sha256sum "$BIN" | cut -d' ' -f1)
CAPTURE_TS=$(date -u '+%Y-%m-%dT%H:%M:%SZ')
BIN_MTIME=$(date -u -r "$BIN" '+%Y-%m-%dT%H:%M:%SZ')
LAST_SRC=
LAST_MTIME=0
for f in $PRC_SRCS; do
    m=$(stat -c %Y "$f" 2>/dev/null || echo 0)
    if [ "$m" -ge "$LAST_MTIME" ]; then
        LAST_MTIME=$m
        LAST_SRC=$f
    fi
done
LAST_SRC_TIME=$(date -u -r "$LAST_SRC" '+%Y-%m-%dT%H:%M:%SZ')
NEWER_SRC=$(find $PRC_SRCS -newer "$BIN" -print -quit 2>/dev/null)

pass() { printf -- '- PASS %s\n' "$1"; }
fail() { printf -- '- FAIL %s (want %s, got %s)\n' "$1" "$2" "$3" >&2; FAIL=$((FAIL + 1)); }
expect() {
    if [ "$2" = "$3" ]; then pass "$1"; else fail "$1" "$2" "$3"; fi
}
h() { printf '\n## %s\n' "$1"; }
cmd() { printf '\n```bash\n%s\n```\n' "$1"; }
out() { printf '\n```\n%s\n```\n' "$1"; }

printf '# Journal integrity transcript\n'
printf '\nCaptured by `tests/journal_transcript.sh` against the real `./zeus` binary'
printf ' on `%s` at `%s`.\n' "$(uname -srm)" "$CAPTURE_TS"
printf '\nChain verification, tamper detection, resolving an ambiguous intent, and'
printf ' snapshot head binding.\n'

h 'Provenance'
out "binary:       $BIN
sha256:       $BIN_HASH
binary mtime: $BIN_MTIME
last PR C source edit: $LAST_SRC_TIME ($LAST_SRC)
sources checked: $PRC_SRCS
captured:     $CAPTURE_TS
binary postdates every PR C source: $([ -z "$NEWER_SRC" ] && echo yes || echo "no ($NEWER_SRC)")"
expect 'binary postdates every PR C source file' 1 \
    "$([ -z "$NEWER_SRC" ] && echo 1 || echo 0)"

# ------------------------------------------------------------------
h '1. A clean chain verifies'

PROG="$WORK/p.zasm"
J="$WORK/clean.jsonl"
printf 'READ\nHALT\n' > "$PROG"

cmd "./zeus run-asm p.zasm --journal clean.jsonl < /dev/null"
RUN_OUT="$(./zeus run-asm "$PROG" --journal "$J" < /dev/null 2>&1)"
RUN_RC=$?
out "$RUN_OUT
[exit $RUN_RC]"
expect 'clean run exits 0' 0 "$RUN_RC"

cmd "./zeus journal verify clean.jsonl"
VER_OUT="$(./zeus journal verify "$J" 2>&1)"
VER_RC=$?
out "$VER_OUT
[exit $VER_RC]"
expect 'verify exits 0' 0 "$VER_RC"
expect 'verify reports the chain verified' 1 \
    "$(printf '%s' "$VER_OUT" | grep -c 'chain verified')"

cmd "head -2 clean.jsonl"
out "$(head -2 "$J")"

# ------------------------------------------------------------------
h '2. One flipped byte is located'

TAMPERED="$WORK/tampered.jsonl"
awk 'NR==2 {
    p = index($0, "\"machine\":\"")
    if (p > 0) {
        c = substr($0, p + 11, 1)
        r = (c == "0") ? "1" : "0"
        $0 = substr($0, 1, p + 10) r substr($0, p + 12)
    }
} { print }' "$J" > "$TAMPERED"

cmd "# the same journal with one hex digit of line 2's machine id changed"
TAM_OUT="$(./zeus journal verify "$TAMPERED" 2>&1)"
TAM_RC=$?
out "$TAM_OUT
[exit $TAM_RC]"
expect 'tampered journal exits 1' 1 "$TAM_RC"
expect 'verify names line 3' 1 \
    "$(printf '%s' "$TAM_OUT" | grep -c 'line 3')"
expect 'verify calls it a chain break' 1 \
    "$(printf '%s' "$TAM_OUT" | grep -c 'chain break')"

# ------------------------------------------------------------------
h '3. An ambiguous legacy journal needs resolve'

AMB="$WORK/ambiguous.jsonl"
AMBSEED="$WORK/amb-seed.jsonl"
AMBSNAP="$WORK/amb-seed.zsnap"
MACHINE="deadbeefdeadbeefdeadbeefdeadbeef"

# tests/test_ask.sh takes the lineage from a real parked dossier; do the same
# so the ambiguous journal names a lineage this build actually ran under.
cmd "./zeus run-asm p.zasm --ask --journal amb-seed.jsonl --save amb-seed.zsnap"
SEED_OUT="$(./zeus run-asm "$PROG" --ask --journal "$AMBSEED" \
    --save "$AMBSNAP" 2>&1)"
SEED_RC=$?
out "$SEED_OUT
[exit $SEED_RC]"
expect 'seed park exits 3' 3 "$SEED_RC"

cmd "LINEAGE=\$(sed -n 's/^lineage=//p' amb-seed.zsnap.dossier)"
LINEAGE="$(sed -n 's/^lineage=//p' "$AMBSNAP.dossier")"
out "$LINEAGE"

cmd "# a legacy start + unmatched intent (no prev), as an older build wrote it"
{
    printf '{"seq":0,"event":"start","machine":"%s","lineage":"%s"}\n' \
        "$MACHINE" "$LINEAGE"
    printf '{"seq":1,"event":"intent","machine":"%s","lineage":"%s",' \
        "$MACHINE" "$LINEAGE"
    printf '"effect":"read"}\n'
} > "$AMB"
out "$(cat "$AMB")"

cmd "# before resolve: the open intent is still a wedge
./zeus run-asm p.zasm --journal ambiguous.jsonl --lineage $LINEAGE < /dev/null"
BEFORE="$(./zeus run-asm "$PROG" --journal "$AMB" --lineage "$LINEAGE" \
    < /dev/null 2>&1)"
BEFORE_RC=$?
out "$BEFORE
[exit $BEFORE_RC]"
expect 'the unmatched intent refuses the next run (1)' 1 "$BEFORE_RC"
expect 'the refusal names the uncommitted intent' 1 \
    "$(printf '%s' "$BEFORE" | grep -c 'uncommitted intent')"

cmd "./zeus journal verify ambiguous.jsonl"
V1="$(./zeus journal verify "$AMB" 2>&1)"
V1_RC=$?
out "$V1
[exit $V1_RC]"
expect 'legacy verify refuses without --allow-legacy' 1 "$V1_RC"
expect 'the refusal asks for --allow-legacy' 1 \
    "$(printf '%s' "$V1" | grep -c 'allow-legacy')"

cmd "./zeus journal verify ambiguous.jsonl --allow-legacy"
V2="$(./zeus journal verify "$AMB" --allow-legacy 2>&1)"
V2_RC=$?
out "$V2
[exit $V2_RC]"
expect 'legacy verify accepts with --allow-legacy' 0 "$V2_RC"

cmd "./zeus journal resolve --journal ambiguous.jsonl --seq 1 --outcome aborted --reason 'operator resolved the crash'"
R1="$(./zeus journal resolve --journal "$AMB" --seq 1 --outcome aborted \
    --reason 'operator resolved the crash' 2>&1)"
R1_RC=$?
out "$R1
[exit $R1_RC]"
expect 'resolve exits 0' 0 "$R1_RC"

cmd "./zeus journal resolve --journal ambiguous.jsonl --seq 1 --outcome aborted --reason again"
R2="$(./zeus journal resolve --journal "$AMB" --seq 1 --outcome aborted \
    --reason again 2>&1)"
R2_RC=$?
out "$R2
[exit $R2_RC]"
expect 'double resolve is refused' 1 "$R2_RC"

cmd "# after resolve: the same journal continues
./zeus run-asm p.zasm --journal ambiguous.jsonl --lineage $LINEAGE < /dev/null"
NEXT="$(./zeus run-asm "$PROG" --journal "$AMB" --lineage "$LINEAGE" \
    < /dev/null 2>&1)"
NEXT_RC=$?
out "$NEXT
[exit $NEXT_RC]"
expect 'the next run proceeds' 0 "$NEXT_RC"
expect 'the legacy chain warns once' 1 \
    "$(printf '%s' "$NEXT" | grep -c 'chain unverified')"

cmd "tail -2 ambiguous.jsonl"
out "$(tail -2 "$AMB")"

# ------------------------------------------------------------------
h '4. A snapshot is bound to its journal head'

SNAPJ="$WORK/snap-a.jsonl"
SNAP="$WORK/snap-a.zsnap"
OTHERJ="$WORK/other.jsonl"

cmd "./zeus run-asm p.zasm --ask --journal snap-a.jsonl --save snap-a.zsnap"
PARK="$(./zeus run-asm "$PROG" --ask --journal "$SNAPJ" --save "$SNAP" 2>&1)"
PARK_RC=$?
out "$PARK
[exit $PARK_RC]"
expect 'park exits 3' 3 "$PARK_RC"

cmd "./zeus run-asm p.zasm --journal other.jsonl"
./zeus run-asm "$PROG" --journal "$OTHERJ" >/dev/null 2>&1

cmd "./zeus restore snap-a.zsnap --journal other.jsonl"
DIFF="$(./zeus restore "$SNAP" --journal "$OTHERJ" 2>&1)"
DIFF_RC=$?
out "$DIFF
[exit $DIFF_RC]"
expect 'restore against a different journal exits 1' 1 "$DIFF_RC"
expect 'the refusal names the different journal' 1 \
    "$(printf '%s' "$DIFF" | grep -c 'different journal')"

# ------------------------------------------------------------------
h 'Result'
if [ "$FAIL" -eq 0 ]; then
    printf 'All checks passed.\n'
    exit 0
fi
printf '%d check(s) failed.\n' "$FAIL"
exit 1
