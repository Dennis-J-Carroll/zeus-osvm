#!/usr/bin/env bash
#
# End-to-end proof of the two denial outcomes through the real ./zeus binary.
# Prints a Markdown transcript on stdout; exits 1 if any check fails.
#
# The unrecorded case is forced with RLIMIT_FSIZE, not a stub: the journal's
# start record fits under the limit, the kernel clamps the deny record, and
# journal_write rolls the partial write back. No supervisor code is bypassed.
#
# Generated with:  bash tests/deny_transcript.sh > docs/deny-transcript.md

set -u
cd "$(dirname "$0")/.."

WORK=$(mktemp -d)
FAIL=0
trap 'rm -rf "$WORK"' EXIT

pass() { printf -- '- PASS %s\n' "$1"; }
fail() { printf -- '- FAIL %s (want %s, got %s)\n' "$1" "$2" "$3" >&2; FAIL=$((FAIL + 1)); }
expect() {
    if [ "$2" = "$3" ]; then pass "$1"; else fail "$1" "$2" "$3"; fi
}
h() { printf '\n## %s\n' "$1"; }
sub() { printf '\n### %s\n' "$1"; }
cmd() { printf '\n```bash\n%s\n```\n' "$1"; }
out() { printf '\n```\n%s\n```\n' "$1"; }

printf '# Deny transcript\n'
printf '\nCaptured by `tests/deny_transcript.sh` against the real `./zeus` binary'
printf ' on `%s`.\n' "$(uname -srm)"
printf '\nRecorded denial: the journal accepts the refusal (exit 4).\n'
printf 'Unrecorded denial: the journal cannot append it (exit 1).\n'

# ------------------------------------------------------------------
h '1. Recorded denial (`zeus deny`)'

PROG="$WORK/recorded.zasm"
J="$WORK/recorded.jsonl"
SNAP="$WORK/recorded.zsnap"
DOSSIER="$SNAP.dossier"
GOOD="$WORK/recorded.approve"

printf 'READ\nPRINTC\nHALT\n' > "$PROG"

cmd "printf 'READ\\nPRINTC\\nHALT\\n' > recorded.zasm
printf 'Q\\n' | ./zeus run-asm recorded.zasm --ask --journal recorded.jsonl --save recorded.zsnap"
PARK_OUT="$(printf 'Q\n' | ./zeus run-asm "$PROG" --ask --journal "$J" --save "$SNAP" 2>&1)"
PARK_RC=$?
out "$PARK_OUT
[exit $PARK_RC]"
expect 'park exits 3' 3 "$PARK_RC"

cmd "./zeus approve recorded.zsnap.dossier --out recorded.approve"
APP_OUT="$(./zeus approve "$DOSSIER" --out "$GOOD" 2>&1)"
APP_RC=$?
out "$APP_OUT
[exit $APP_RC]"
expect 'approve exits 0' 0 "$APP_RC"

cmd "printf 'Q\\n' | ./zeus deny recorded.zsnap.dossier --journal recorded.jsonl"
REC_OUT="$(printf 'Q\n' | ./zeus deny "$DOSSIER" --journal "$J" 2>&1)"
REC_RC=$?
out "$REC_OUT
[exit $REC_RC]"
expect 'deny exits 4' 4 "$REC_RC"
expect 'message reports the kill' 1 \
    "$(printf '%s' "$REC_OUT" | grep -c 'machine killed')"
expect 'exactly one deny record' 1 \
    "$(grep -c '"event":"deny"' "$J")"
[ -f "$SNAP" ] && KEPT=1 || KEPT=0
expect 'parked snapshot retained' 1 "$KEPT"
expect 'effect was not delivered (stdin value never echoed)' 0 \
    "$(printf '%s' "$REC_OUT" | grep -c 'Q')"

sub 'Journal afterwards'
out "$(cat "$J")"

sub 'Snapshot afterwards'
out "bytes:  $(wc -c < "$SNAP")
sha256: $(sha256sum "$SNAP" | cut -d' ' -f1)"

# ------------------------------------------------------------------
h '2. Unrecorded denial (`zeus deny`, journal cannot append)'

PROG2="$WORK/unrecorded.zasm"
J2="$WORK/unrecorded.jsonl"
SNAP2="$WORK/unrecorded.zsnap"
DOSSIER2="$SNAP2.dossier"
GOOD2="$WORK/unrecorded.approve"

printf 'READ\nPRINTC\nHALT\n' > "$PROG2"

cmd "printf 'READ\\nPRINTC\\nHALT\\n' > unrecorded.zasm
printf 'Q\\n' | ./zeus run-asm unrecorded.zasm --ask --journal unrecorded.jsonl --save unrecorded.zsnap"
PARK2_OUT="$(printf 'Q\n' | ./zeus run-asm "$PROG2" --ask --journal "$J2" --save "$SNAP2" 2>&1)"
PARK2_RC=$?
out "$PARK2_OUT
[exit $PARK2_RC]"
expect 'park exits 3' 3 "$PARK2_RC"

cmd "./zeus approve unrecorded.zsnap.dossier --out unrecorded.approve"
APP2_OUT="$(./zeus approve "$DOSSIER2" --out "$GOOD2" 2>&1)"
APP2_RC=$?
out "$APP2_OUT
[exit $APP2_RC]"
expect 'approve exits 0' 0 "$APP2_RC"

# Discover the byte limit `ulimit -f 2` imposes: bash's block size is not
# portable, and the padding below must be exact.
( trap '' XFSZ; ulimit -f 2; head -c 8192 /dev/zero > "$WORK/probe" ) >/dev/null 2>&1
LIMIT_BYTES=$(wc -c < "$WORK/probe" 2>/dev/null || echo 0)
expect 'RLIMIT_FSIZE probe yields a byte limit' 1 \
    "$([ "$LIMIT_BYTES" -ge 512 ] && echo 1 || echo 0)"

PAD_TARGET=$((LIMIT_BYTES - 200))
SIZE_BEFORE_PAD=$(wc -c < "$J2")
PAD=$((PAD_TARGET - SIZE_BEFORE_PAD))
# The filler must be a valid record for the strict parser. The previous raw
# xxxx line was not JSON and would be rejected before the denial path.
FILLER_HEAD='{"seq":0,"event":"start","machine":"00000000000000000000000000000000","lineage":"ffffffffffffffffffffffffffffffff","raw":0,"pad":"'
FILLER_TAIL='"}'
FILLER_X=$((PAD - 1 - ${#FILLER_HEAD} - ${#FILLER_TAIL}))
cmd "# 200 bytes of headroom at the limit: the start record (~150 bytes with
# its prev field) fits, the deny record (~211 bytes) cannot. The filler is a
# valid record for another lineage, so the strict parser accepts it."
if [ "$PAD" -gt 0 ] && [ "$FILLER_X" -ge 0 ]; then
    { printf '%s' "$FILLER_HEAD"
      head -c "$FILLER_X" /dev/zero | tr '\0' 'x'
      printf '%s\n' "$FILLER_TAIL"
    } >> "$J2"
fi
expect 'journal padded to the intended size' "$PAD_TARGET" \
    "$(wc -c < "$J2")"

SNAP_HASH_BEFORE=$(sha256sum "$SNAP2" | cut -d' ' -f1)

cmd "trap '' XFSZ; ulimit -f 2
printf 'Q\\n' | ./zeus deny unrecorded.zsnap.dossier --journal unrecorded.jsonl"
# The legacy warning is on stderr and embeds the journal path. mktemp paths
# can contain an uppercase Q, so capturing stderr with stdout would let a Q
# in the warning path satisfy the delivery check; stdout alone is the effect's
# output.
UNREC_OUT="$(
    trap '' XFSZ
    ulimit -f 2
    printf 'Q\n' | ./zeus deny "$DOSSIER2" --journal "$J2" 2>"$WORK/unrec.err"
)"
UNREC_RC=$?
UNREC_ERR="$(cat "$WORK/unrec.err")"
out "$UNREC_ERR
$UNREC_OUT
[exit $UNREC_RC]"
expect 'deny exits 1' 1 "$UNREC_RC"
expect 'message says the denial was NOT recorded' 1 \
    "$(printf '%s' "$UNREC_ERR" | grep -c 'NOT recorded')"
expect 'message does not claim a kill' 0 \
    "$(printf '%s' "$UNREC_ERR" | grep -c 'machine killed')"
expect 'no deny record in the real journal' 0 \
    "$(grep -c '"event":"deny"' "$J2")"
expect 'effect was not delivered (stdin value never echoed)' 0 \
    "$(printf '%s' "$UNREC_OUT" | grep -c 'Q')"
[ -f "$SNAP2" ] && KEPT2=1 || KEPT2=0
expect 'parked snapshot retained' 1 "$KEPT2"
expect 'snapshot unchanged by the failed denial' "$SNAP_HASH_BEFORE" \
    "$(sha256sum "$SNAP2" | cut -d' ' -f1)"
expect 'journal ends on a complete record' 10 \
    "$(tail -c 1 "$J2" | od -An -t u1 | tr -d ' ')"

# The refusal was never an outcome, so its sequence is still free: the same
# parked effect can still be approved and delivered.
cmd "./zeus approve unrecorded.zsnap.dossier --out again.approve"
AGAIN_OUT="$(./zeus approve "$DOSSIER2" --out "$WORK/again.approve" 2>&1)"
AGAIN_RC=$?
out "$AGAIN_OUT
[exit $AGAIN_RC]"
expect 'approve after the failed denial exits 0' 0 "$AGAIN_RC"
expect 'approval still names seq 1 (seq did not advance)' 1 \
    "$(printf '%s' "$AGAIN_OUT" | grep -c 'seq 1')"

cmd "printf 'Q\\n' | ./zeus restore unrecorded.zsnap --journal unrecorded.jsonl --approve again.approve"
RES_OUT="$(printf 'Q\n' | ./zeus restore "$SNAP2" --journal "$J2" --approve "$WORK/again.approve" 2>"$WORK/res.err")"
RES_RC=$?
RES_ERR="$(cat "$WORK/res.err")"
out "$RES_ERR
$RES_OUT
[exit $RES_RC]"
expect 'restore with the good approval exits 0' 0 "$RES_RC"
expect 'the cancelled READ was still deliverable' 1 \
    "$(printf '%s' "$RES_OUT" | grep -c 'Q')"

sub 'Journal afterwards (filler line elided)'
out "$(sed 's/^x\{40,\}$/<filler: x characters, sized so the start record fits and the deny record cannot>/' "$J2")"

sub 'Snapshot afterwards'
out "bytes:  $(wc -c < "$SNAP2")
sha256: $(sha256sum "$SNAP2" | cut -d' ' -f1)"

# ------------------------------------------------------------------
h 'Result'
if [ "$FAIL" -eq 0 ]; then
    printf 'All checks passed.\n'
    exit 0
fi
printf '%d check(s) failed.\n' "$FAIL"
exit 1
