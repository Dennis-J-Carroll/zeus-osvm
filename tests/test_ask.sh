#!/usr/bin/env bash
#
# End-to-end test for the approval gate:
#   run --ask   -> parks, writes snapshot + dossier, exit 3
#   approve     -> writes an approval bound to (lineage, seq, digest)
#   restore     -> delivers the approved effect, exit 0
#   restore     -> a second sensitive effect parks again, exit 3
#   stale approval -> refused, exit 4
#   deny        -> kills the machine at the pending effect, exit 4
#
# No network, no stdin: the parked effect is READ, which returns EOF.

set -u
cd "$(dirname "$0")/.."

PROG=/tmp/zeus_ask_prog.zasm
JOURNAL=/tmp/zeus_ask_journal.log
SNAP=/tmp/zeus_ask.zsnap
DOSSIER="$SNAP.dossier"
APPROVAL=/tmp/zeus_ask.approve
BAD_DOSSIER=/tmp/zeus_ask_bad.dossier
BAD_APPROVAL=/tmp/zeus_ask_bad.approve

PROG2=/tmp/zeus_ask_two.zasm
JOURNAL2=/tmp/zeus_ask_two.log
SNAP2=/tmp/zeus_ask_two.zsnap
DOSSIER2="$SNAP2.dossier"
A1="$DOSSIER2.seq1.approve"
A2="$DOSSIER2.seq2.approve"

PROG3=/tmp/zeus_ask_hostile.zasm
JOURNAL3=/tmp/zeus_ask_hostile.log
SNAP3=/tmp/zeus_ask_hostile.zsnap
DOSSIER3="$SNAP3.dossier"
A3="$DOSSIER3.seq1.approve"
BAD3=/tmp/zeus_ask_hostile.approve
JOURNAL4=/tmp/zeus_ask_hostile_run.log
SNAP4=/tmp/zeus_ask_hostile_run.zsnap
JOURNAL5=/tmp/zeus_ask_ambiguous.log

cleanup() {
    rm -f           "$PROG" "$JOURNAL" "$SNAP" "$DOSSIER" "$APPROVAL" \
          "$BAD_DOSSIER" "$BAD_APPROVAL" \
          "$PROG2" "$JOURNAL2" "$SNAP2" "$DOSSIER2" "$A1" "$A2" \
          "$PROG3" "$JOURNAL3" "$SNAP3" "$DOSSIER3" "$A3" "$BAD3" \
          "$JOURNAL4" "$SNAP4" "$JOURNAL5"
}
cleanup
trap cleanup EXIT

cat > "$PROG" <<'EOF'
PUSH 65
PRINTC
READ
HALT
EOF

FAIL=0
check() {
    local what="$1" want="$2" got="$3"
    if [ "$want" = "$got" ]; then
        printf '  [ask] %s ... OK\n' "$what"
    else
        printf '  [ask] %s ... FAIL (want %s, got %s)\n' "$what" "$want" "$got"
        FAIL=$((FAIL + 1))
    fi
}

OUT="$(./zeus run-asm "$PROG" --ask --journal "$JOURNAL" --save "$SNAP" 2>/dev/null)"
RC=$?
check "park exits 3" 3 "$RC"
check "program printed before parking" "A" "$(printf '%s' "$OUT" | head -c 1)"
[ -f "$SNAP" ] && [ -f "$DOSSIER" ] && PASS=1 || PASS=0
check "snapshot + dossier written" 1 "$PASS"

./zeus approve "$DOSSIER" --out "$APPROVAL" >/dev/null 2>&1
check "approve exits 0" 0 "$?"

# The dossier must describe the snapshot it names: a digest that does not
# match the pending effect is refused, and no approval file is written.
sed 's/^digest=.*/digest=0x0000000000000000/' "$DOSSIER" > "$BAD_DOSSIER"
./zeus approve "$BAD_DOSSIER" --out "$BAD_APPROVAL" >/dev/null 2>&1
check "approve refuses a dossier that does not match its snapshot (4)" 4 "$?"
[ -e "$BAD_APPROVAL" ] && WROTE=1 || WROTE=0
check "approve writes nothing for a mismatched dossier" 0 "$WROTE"

# Restoring without the approval parks again rather than guessing.
./zeus restore "$SNAP" --journal "$JOURNAL" --ask --save "$SNAP" >/dev/null 2>&1
check "restore without approval parks again (3)" 3 "$?"

# --approve alone turns the gate on and authorises exactly this effect.
./zeus restore "$SNAP" --journal "$JOURNAL" --approve "$APPROVAL" >/dev/null 2>&1
check "restore with approval exits 0" 0 "$?"

./zeus deny "$DOSSIER" --journal "$JOURNAL" >/dev/null 2>&1
check "deny exits 4" 4 "$?"

# ------------------------------------------------------------------
# Two sensitive effects: the second one must park after the first
# approval is spent, even when --approve is given without --ask or
# --save (the parking path is then inferred from the restored file).
# ------------------------------------------------------------------

cat > "$PROG2" <<'EOF'
READ
READ
HALT
EOF

./zeus run-asm "$PROG2" --ask --journal "$JOURNAL2" --save "$SNAP2" >/dev/null 2>&1
check "two effects: first park exits 3" 3 "$?"

./zeus approve "$DOSSIER2" --out "$A1" >/dev/null 2>&1
check "two effects: first approve exits 0" 0 "$?"

./zeus restore "$SNAP2" --journal "$JOURNAL2" --approve "$A1" >/dev/null 2>&1
check "two effects: second effect parks (3)" 3 "$?"
check "two effects: dossier advanced to seq 2" "seq=2" \
    "$(grep -m1 '^seq=' "$DOSSIER2")"

./zeus approve "$DOSSIER2" --out "$A2" >/dev/null 2>&1
check "two effects: second approve exits 0" 0 "$?"

./zeus restore "$SNAP2" --journal "$JOURNAL2" --approve "$A2" >/dev/null 2>&1
check "two effects: second resume exits 0" 0 "$?"

# The spent approval names seq 1, which the journal has moved past: the
# approval must be checked, not treated as a standing grant.
./zeus restore "$SNAP2" --journal "$JOURNAL2" --approve "$A1" >/dev/null 2>&1
check "two effects: spent approval refused (4)" 4 "$?"

# ------------------------------------------------------------------
# A malformed approval is an error (exit 1), not a refusal (exit 4).
# It must not spend the parked sequence, kill the machine or reach the
# journal, and the good approval must still resume afterwards.
# ------------------------------------------------------------------

cat > "$PROG3" <<'EOF'
READ
HALT
EOF

./zeus run-asm "$PROG3" --ask --journal "$JOURNAL3" --save "$SNAP3" >/dev/null 2>&1
check "hostile approval: park exits 3" 3 "$?"

./zeus approve "$DOSSIER3" --out "$A3" >/dev/null 2>&1
check "hostile approval: good approval written" 0 "$?"

# Duplicate key: the file names two sequence numbers at once.
cat "$A3" > "$BAD3"
printf 'seq=1\n' >> "$BAD3"
./zeus restore "$SNAP3" --journal "$JOURNAL3" --approve "$BAD3" >/dev/null 2>&1
check "hostile approval: duplicate key is an error (1)" 1 "$?"
check "hostile approval: duplicate key journals no denial" 0 \
    "$(cat "$JOURNAL3" 2>/dev/null | grep -c '"event":"deny"')"

# Truncation: every field is present, the digest is cut and the final
# newline is gone. This used to read as a plausible approval.
printf 'format=1\nkind=approval\nlineage=%s\nseq=1\ndigest=0x0123' \
    "$(sed -n 's/^lineage=//p' "$A3")" > "$BAD3"
./zeus restore "$SNAP3" --journal "$JOURNAL3" --approve "$BAD3" >/dev/null 2>&1
check "hostile approval: truncated file is an error (1)" 1 "$?"

# Oversized value: no field is allowed to be a buffer's worth of hex.
{ cat "$A3"; printf 'digest=0x'; printf '%*s' 2000 '' | tr ' ' 'a'; printf '\n'; } > "$BAD3"
./zeus restore "$SNAP3" --journal "$JOURNAL3" --approve "$BAD3" >/dev/null 2>&1
check "hostile approval: oversized value is an error (1)" 1 "$?"

# run --approve must refuse before opening a journal or delivering anything.
./zeus run-asm "$PROG3" --journal "$JOURNAL4" --save "$SNAP4" --approve "$BAD3" >/dev/null 2>&1
check "hostile approval: run refuses (1)" 1 "$?"
check "hostile approval: run delivered nothing" 0 \
    "$(cat "$JOURNAL4" 2>/dev/null | grep -c '"event":"\(intent\|deny\)"')"

# The machine survived all of that: the original approval still resumes it.
./zeus restore "$SNAP3" --journal "$JOURNAL3" --approve "$A3" >/dev/null 2>&1
check "hostile approval: machine still resumes (0)" 0 "$?"

# ------------------------------------------------------------------
# An unmatched intent is evidence, not a state a retry may clear. A
# refused open used to append a `start` record after the intent, so the
# second retry looked clean and replayed an effect that may already have
# happened. Both retries must refuse and write nothing.
# ------------------------------------------------------------------
LINEAGE3="$(sed -n 's/^lineage=//p' "$DOSSIER3")"
{
    printf '{"seq":0,"event":"start","machine":"deadbeef","lineage":"%s"}\n' "$LINEAGE3"
    printf '{"seq":1,"event":"intent","machine":"deadbeef","lineage":"%s","effect":"read"}\n' "$LINEAGE3"
} > "$JOURNAL5"

./zeus restore "$SNAP3" --journal "$JOURNAL5" --lineage "$LINEAGE3" >/dev/null 2>&1
check "unmatched intent: first retry refuses (1)" 1 "$?"
./zeus restore "$SNAP3" --journal "$JOURNAL5" --lineage "$LINEAGE3" >/dev/null 2>&1
check "unmatched intent: second retry refuses (1)" 1 "$?"
check "unmatched intent: retries wrote no start record" 1 \
    "$(grep -c '"event":"start"' "$JOURNAL5")"

if [ "$FAIL" -eq 0 ]; then
    printf '  [ask] all checks passed\n'
    exit 0
fi
printf '  [ask] %d check(s) failed\n' "$FAIL"
exit 1
