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

# Nothing here prompts the operator: the pending READ is approved and then
# delivered, so it must see EOF rather than inherit a terminal. Taking stdin
# out of the equation keeps `make test` non-blocking however it is invoked.
exec < /dev/null

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

PROG4=/tmp/zeus_ask_deny.zasm
JOURNAL6=/tmp/zeus_ask_deny.log
SNAP6=/tmp/zeus_ask_deny.zsnap
DOSSIER6="$SNAP6.dossier"
A6=/tmp/zeus_ask_deny.approve
JOURNAL7=/tmp/zeus_ask_clean.log
TAMPERED=/tmp/zeus_ask_tampered.log

cleanup() {
    rm -f           "$PROG" "$JOURNAL" "$SNAP" "$DOSSIER" "$APPROVAL" \
          "$BAD_DOSSIER" "$BAD_APPROVAL" \
          "$PROG2" "$JOURNAL2" "$SNAP2" "$DOSSIER2" "$A1" "$A2" \
          "$PROG3" "$JOURNAL3" "$SNAP3" "$DOSSIER3" "$A3" "$BAD3" \
          "$JOURNAL4" "$SNAP4" "$JOURNAL5" \
          "$PROG4" "$JOURNAL6" "$SNAP6" "$DOSSIER6" "$A6" \
          "$JOURNAL7" "$TAMPERED"
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

# The snapshot records the lineage and sequence it was parked under. A
# dossier naming another point in the chain is refused before its digest is
# even considered.
sed 's/^seq=.*/seq=3/' "$DOSSIER" > "$BAD_DOSSIER"
./zeus approve "$BAD_DOSSIER" --out "$BAD_APPROVAL" >/dev/null 2>&1
check "approve refuses a dossier from another sequence (4)" 4 "$?"
sed 's/^lineage=.*/lineage=ffffffffffffffffffffffffffffffff/' "$DOSSIER" > "$BAD_DOSSIER"
./zeus approve "$BAD_DOSSIER" --out "$BAD_APPROVAL" >/dev/null 2>&1
check "approve refuses a dossier from another lineage (4)" 4 "$?"

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
# approval must be checked against the pending effect, not treated as a
# standing grant. A mismatch is a denial (exit 4) before any gap override.
SPENT_OUT="$(./zeus restore "$SNAP2" --journal "$JOURNAL2" --approve "$A1" \
    2>&1 >/dev/null)"
check "two effects: spent approval refused (4)" 4 "$?"
check "two effects: spent approval refusal names the approval mismatch" 1 \
    "$(printf '%s' "$SPENT_OUT" | grep -c 'approval does not match')"
./zeus restore "$SNAP2" --journal "$JOURNAL2" --approve "$A1" --allow-gap >/dev/null 2>&1
check "two effects: spent approval still refused with --allow-gap (4)" 4 "$?"

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
    printf '{"seq":0,"event":"start","machine":"deadbeefdeadbeefdeadbeefdeadbeef","lineage":"%s"}\n' "$LINEAGE3"
    printf '{"seq":1,"event":"intent","machine":"deadbeefdeadbeefdeadbeefdeadbeef","lineage":"%s","effect":"read"}\n' "$LINEAGE3"
} > "$JOURNAL5"

./zeus restore "$SNAP3" --journal "$JOURNAL5" --lineage "$LINEAGE3" >/dev/null 2>&1
check "unmatched intent: first retry refuses (1)" 1 "$?"
./zeus restore "$SNAP3" --journal "$JOURNAL5" --lineage "$LINEAGE3" >/dev/null 2>&1
check "unmatched intent: second retry refuses (1)" 1 "$?"
check "unmatched intent: retries wrote no start record" 1 \
    "$(grep -c '"event":"start"' "$JOURNAL5")"

# ------------------------------------------------------------------
# A recorded denial is an outcome with evidence: exit 4, the message
# reports the kill, the refusal is in the journal and the parked
# snapshot is kept (denying must not destroy what the dossier names).
# ------------------------------------------------------------------
cat > "$PROG4" <<'EOF'
READ
HALT
EOF

./zeus run-asm "$PROG4" --ask --journal "$JOURNAL6" --save "$SNAP6" >/dev/null 2>&1
check "recorded deny: park exits 3" 3 "$?"

# The approval is written while the dossier and its snapshot still agree, so
# the stale-approval check below is about the journal moving on, not a
# malformed file.
./zeus approve "$DOSSIER6" --out "$A6" >/dev/null 2>&1
check "recorded deny: approval written before the denial" 0 "$?"

DENY_OUT="$(./zeus deny "$DOSSIER6" --journal "$JOURNAL6" 2>/dev/null)"
check "recorded deny: exit 4" 4 "$?"
check "recorded deny: message reports the kill" 1 \
    "$(printf '%s' "$DENY_OUT" | grep -c 'machine killed')"
check "recorded deny: does not claim an unrecorded denial" 0 \
    "$(printf '%s' "$DENY_OUT" | grep -c 'NOT recorded')"
check "recorded deny: journal has one deny record" 1 \
    "$(grep -c '"event":"deny"' "$JOURNAL6")"
[ -f "$SNAP6" ] && KEPT=1 || KEPT=0
check "recorded deny: parked snapshot kept as evidence" 1 "$KEPT"

# ------------------------------------------------------------------
# A dossier/approval from an earlier point in the chain must not act on a
# journal that has moved past it. The default is an operator error (exit 1)
# and no new record; --allow-gap is the explicit opt-in.
# ------------------------------------------------------------------
ERR="$(./zeus deny "$DOSSIER6" --journal "$JOURNAL6" 2>&1 >/dev/null)"
check "stale dossier: gap refuses by default (1)" 1 "$?"
check "stale dossier: refusal names the gap" 1 \
    "$(printf '%s' "$ERR" | grep -c 'refusing to act across a gap')"
check "stale dossier: gap refusal writes no second denial" 1 \
    "$(grep -c '"event":"deny"' "$JOURNAL6")"

./zeus deny "$DOSSIER6" --journal "$JOURNAL6" --allow-gap >/dev/null 2>&1
check "stale dossier: --allow-gap proceeds (4)" 4 "$?"
check "stale dossier: --allow-gap records the new denial" 2 \
    "$(grep -c '"event":"deny"' "$JOURNAL6")"

ERR="$(./zeus restore "$SNAP6" --journal "$JOURNAL6" --approve "$A6" \
    2>&1 >/dev/null)"
check "stale approval: refused at the effect check (4)" 4 "$?"
check "stale approval: refusal names the approval mismatch" 1 \
    "$(printf '%s' "$ERR" | grep -c 'approval does not match')"
check "stale approval: denial is recorded" 3 \
    "$(grep -c '"event":"deny"' "$JOURNAL6")"

./zeus restore "$SNAP6" --journal "$JOURNAL6" --approve "$A6" --allow-gap \
    >/dev/null 2>&1
check "stale approval: --allow-gap still refuses (4)" 4 "$?"
check "stale approval: --allow-gap records the mismatch" 4 \
    "$(grep -c '"event":"deny"' "$JOURNAL6")"

# ------------------------------------------------------------------
# Journal integrity: a legacy chain is accepted only explicitly, resolve
# closes the ambiguous intent so the next run proceeds, and a journal written
# by this build is a verified chain whose tampering is located by line.
# ------------------------------------------------------------------
./zeus journal verify "$JOURNAL5" >/dev/null 2>&1
check "ambiguous journal: verify refuses an unverified legacy chain (1)" 1 "$?"
./zeus journal verify "$JOURNAL5" --allow-legacy >/dev/null 2>&1
check "ambiguous journal: --allow-legacy accepts it (0)" 0 "$?"

./zeus journal resolve --journal "$JOURNAL5" --seq 1 --outcome aborted \
    --reason "operator resolved the crash" >/dev/null 2>&1
check "ambiguous journal: resolve closes the intent (0)" 0 "$?"
./zeus journal resolve --journal "$JOURNAL5" --seq 1 --outcome aborted \
    --reason "again" >/dev/null 2>&1
check "ambiguous journal: double resolve refused (1)" 1 "$?"
./zeus journal resolve --journal "$JOURNAL5" --seq 2 --outcome committed \
    --reason "nothing here" >/dev/null 2>&1
check "ambiguous journal: resolve without an intent refused (1)" 1 "$?"

LEGACY_WARN="$(./zeus run-asm "$PROG3" --journal "$JOURNAL5" \
    --lineage "$LINEAGE3" 2>&1 >/dev/null)"
check "ambiguous journal: the next run proceeds after resolve (0)" 0 "$?"
check "legacy journal: open warns once about the unverified chain" 1 \
    "$(printf '%s' "$LEGACY_WARN" | grep -c 'chain unverified')"

# A fresh journal from this build has a chain from the first record.
./zeus run-asm "$PROG4" --journal "$JOURNAL7" >/dev/null 2>&1
check "clean journal: seeded run exits 0" 0 "$?"
./zeus journal verify "$JOURNAL7" >/dev/null 2>&1
check "clean journal: verify exits 0" 0 "$?"
check "clean journal: verify reports a verified chain" 1 \
    "$(./zeus journal verify "$JOURNAL7" 2>/dev/null | grep -c 'chain verified')"

# Flip one hex digit of the machine id on line 2. The record stays valid, so
# only the next record's prev can expose the change.
awk 'NR==2 {
    p = index($0, "\"machine\":\"")
    if (p > 0) {
        c = substr($0, p + 11, 1)
        r = (c == "0") ? "1" : "0"
        $0 = substr($0, 1, p + 10) r substr($0, p + 12)
    }
} { print }' "$JOURNAL7" > "$TAMPERED"
TAMPER_ERR="$(./zeus journal verify "$TAMPERED" 2>&1 >/dev/null)"
check "tampered journal: verify exits 1" 1 "$?"
check "tampered journal: names the first divergent line" 1 \
    "$(printf '%s' "$TAMPER_ERR" | grep -c 'line 3')"
check "tampered journal: reports a chain break" 1 \
    "$(printf '%s' "$TAMPER_ERR" | grep -c 'chain break')"

# The parked machine names the journal head it was written at; handing it a
# different journal must refuse instead of resuming the effect there.
HEAD_ERR="$(./zeus restore "$SNAP6" --journal "$JOURNAL7" \
    2>&1 >/dev/null)"
check "snapshot head: a different journal is refused (1)" 1 "$?"
check "snapshot head: refusal names the different journal" 1 \
    "$(printf '%s' "$HEAD_ERR" | grep -c 'different journal')"

check "help lists journal verify" 1 \
    "$(./zeus --help 2>&1 | grep -c 'journal verify')"
check "help lists journal resolve" 1 \
    "$(./zeus --help 2>&1 | grep -c 'journal resolve')"

if [ "$FAIL" -eq 0 ]; then
    printf '  [ask] all checks passed\n'
    exit 0
fi
printf '  [ask] %d check(s) failed\n' "$FAIL"
exit 1
