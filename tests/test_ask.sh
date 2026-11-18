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

PROG2=/tmp/zeus_ask_two.zasm
JOURNAL2=/tmp/zeus_ask_two.log
SNAP2=/tmp/zeus_ask_two.zsnap
DOSSIER2="$SNAP2.dossier"
A1="$DOSSIER2.seq1.approve"
A2="$DOSSIER2.seq2.approve"

cleanup() {
    rm -f "$PROG" "$JOURNAL" "$SNAP" "$DOSSIER" "$APPROVAL" \
          "$PROG2" "$JOURNAL2" "$SNAP2" "$DOSSIER2" "$A1" "$A2"
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

if [ "$FAIL" -eq 0 ]; then
    printf '  [ask] all checks passed\n'
    exit 0
fi
printf '  [ask] %d check(s) failed\n' "$FAIL"
exit 1
