# Deny transcript

Captured by `tests/deny_transcript.sh` against the real `./zeus` binary on `Linux 6.1.102 x86_64`.

Recorded denial: the journal accepts the refusal (exit 4).
Unrecorded denial: the journal cannot append it (exit 1).

## 1. Recorded denial (`zeus deny`)

```bash
printf 'READ\nPRINTC\nHALT\n' > recorded.zasm
printf 'Q\n' | ./zeus run-asm recorded.zasm --ask --journal recorded.jsonl --save recorded.zsnap
```

```
suspended: read seq 1 parked for approval (digest 0xb540caca4b83a380)
snapshot:  /tmp/tmp.wyYhgkwiGg/recorded.zsnap
dossier:   /tmp/tmp.wyYhgkwiGg/recorded.zsnap.dossier
approve:   zeus approve /tmp/tmp.wyYhgkwiGg/recorded.zsnap.dossier --out /tmp/tmp.wyYhgkwiGg/recorded.zsnap.dossier.seq1.approve
resume:    zeus restore /tmp/tmp.wyYhgkwiGg/recorded.zsnap --journal /tmp/tmp.wyYhgkwiGg/recorded.jsonl --ask --approve /tmp/tmp.wyYhgkwiGg/recorded.zsnap.dossier.seq1.approve
deny:      zeus deny /tmp/tmp.wyYhgkwiGg/recorded.zsnap.dossier --journal /tmp/tmp.wyYhgkwiGg/recorded.jsonl
[exit 3]
```
- PASS park exits 3

```bash
./zeus approve recorded.zsnap.dossier --out recorded.approve
```

```
approved read seq 1
lineage:  8f25d125d907f6a4d51376ff62d25e79
digest:   0xb540caca4b83a380
approval: /tmp/tmp.wyYhgkwiGg/recorded.approve
[exit 0]
```
- PASS approve exits 0

```bash
printf 'Q\n' | ./zeus deny recorded.zsnap.dossier --journal recorded.jsonl
```

```
denied read seq 1 (digest 0xb540caca4b83a380); machine killed
[exit 4]
```
- PASS deny exits 4
- PASS message reports the kill
- PASS exactly one deny record
- PASS parked snapshot retained
- PASS effect was not delivered (stdin value never echoed)

### Journal afterwards

```
{"prev":"0000000000000000","seq":0,"event":"start","machine":"20af0610c6e70a3d4596982aa16c8d37","lineage":"8f25d125d907f6a4d51376ff62d25e79","raw":0}
{"prev":"5f0941cc1dff9863","seq":0,"event":"park","machine":"20af0610c6e70a3d4596982aa16c8d37","lineage":"8f25d125d907f6a4d51376ff62d25e79","pending":1,"effect":"read","digest":"0xb540caca4b83a380"}
{"prev":"67032519d84505e8","seq":0,"event":"start","machine":"ea82ade782e1a0a173fdf9603d5b25e5","lineage":"8f25d125d907f6a4d51376ff62d25e79","raw":0}
{"prev":"22f470088b8d5a15","seq":1,"event":"deny","machine":"ea82ade782e1a0a173fdf9603d5b25e5","lineage":"8f25d125d907f6a4d51376ff62d25e79","effect":"read","digest":"0xb540caca4b83a380","reason":"operator_denied"}
```

### Snapshot afterwards

```
bytes:  135332
sha256: 2bee78c7446d4e1d30759d36d28002e43d321bf4321ac0ce782e1d5eb0b1c3c1
```

## 2. Unrecorded denial (`zeus deny`, journal cannot append)

```bash
printf 'READ\nPRINTC\nHALT\n' > unrecorded.zasm
printf 'Q\n' | ./zeus run-asm unrecorded.zasm --ask --journal unrecorded.jsonl --save unrecorded.zsnap
```

```
suspended: read seq 1 parked for approval (digest 0xb540caca4b83a380)
snapshot:  /tmp/tmp.wyYhgkwiGg/unrecorded.zsnap
dossier:   /tmp/tmp.wyYhgkwiGg/unrecorded.zsnap.dossier
approve:   zeus approve /tmp/tmp.wyYhgkwiGg/unrecorded.zsnap.dossier --out /tmp/tmp.wyYhgkwiGg/unrecorded.zsnap.dossier.seq1.approve
resume:    zeus restore /tmp/tmp.wyYhgkwiGg/unrecorded.zsnap --journal /tmp/tmp.wyYhgkwiGg/unrecorded.jsonl --ask --approve /tmp/tmp.wyYhgkwiGg/unrecorded.zsnap.dossier.seq1.approve
deny:      zeus deny /tmp/tmp.wyYhgkwiGg/unrecorded.zsnap.dossier --journal /tmp/tmp.wyYhgkwiGg/unrecorded.jsonl
[exit 3]
```
- PASS park exits 3

```bash
./zeus approve unrecorded.zsnap.dossier --out unrecorded.approve
```

```
approved read seq 1
lineage:  6ac665769a116a7d1b3f952b5ae36242
digest:   0xb540caca4b83a380
approval: /tmp/tmp.wyYhgkwiGg/unrecorded.approve
[exit 0]
```
- PASS approve exits 0
- PASS RLIMIT_FSIZE probe yields a byte limit

```bash
# 200 bytes of headroom at the limit: the start record (~150 bytes with
# its prev field) fits, the deny record (~211 bytes) cannot. The filler is a
# valid record for another lineage, so the strict parser accepts it.
```
- PASS journal padded to the intended size

```bash
trap '' XFSZ; ulimit -f 2
printf 'Q\n' | ./zeus deny unrecorded.zsnap.dossier --journal unrecorded.jsonl
```

```
warning: journal '/tmp/tmp.wyYhgkwiGg/unrecorded.jsonl' has legacy records without prev; chain unverified
error: denied read seq 1 (digest 0xb540caca4b83a380) but the denial was NOT recorded in the journal; the parked machine is untouched

[exit 1]
```
- PASS deny exits 1
- PASS message says the denial was NOT recorded
- PASS message does not claim a kill
- PASS no deny record in the real journal
- PASS effect was not delivered (stdin value never echoed)
- PASS parked snapshot retained
- PASS snapshot unchanged by the failed denial
- PASS journal ends on a complete record

```bash
./zeus approve unrecorded.zsnap.dossier --out again.approve
```

```
approved read seq 1
lineage:  6ac665769a116a7d1b3f952b5ae36242
digest:   0xb540caca4b83a380
approval: /tmp/tmp.wyYhgkwiGg/again.approve
[exit 0]
```
- PASS approve after the failed denial exits 0
- PASS approval still names seq 1 (seq did not advance)

```bash
printf 'Q\n' | ./zeus restore unrecorded.zsnap --journal unrecorded.jsonl --approve again.approve
```

```
warning: journal '/tmp/tmp.wyYhgkwiGg/unrecorded.jsonl' has legacy records without prev; chain unverified
Q
[exit 0]
```
- PASS restore with the good approval exits 0
- PASS the cancelled READ was still deliverable

### Journal afterwards (filler line elided)

```
{"prev":"0000000000000000","seq":0,"event":"start","machine":"f9f1bf0127a5b819a23b8f58ba09f61f","lineage":"6ac665769a116a7d1b3f952b5ae36242","raw":0}
{"prev":"b526ca66f1adf30b","seq":0,"event":"park","machine":"f9f1bf0127a5b819a23b8f58ba09f61f","lineage":"6ac665769a116a7d1b3f952b5ae36242","pending":1,"effect":"read","digest":"0xb540caca4b83a380"}
{"seq":0,"event":"start","machine":"00000000000000000000000000000000","lineage":"ffffffffffffffffffffffffffffffff","raw":0,"pad":"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"}
{"prev":"70f0d490d2c02ee9","seq":0,"event":"start","machine":"595e14a56d25328033acdfa2c1181985","lineage":"6ac665769a116a7d1b3f952b5ae36242","raw":0}
{"prev":"e6a6e75a429be2b9","seq":0,"event":"start","machine":"34c234191746341ccecb4182935a7b2e","lineage":"6ac665769a116a7d1b3f952b5ae36242","raw":0}
{"prev":"4c5c1ac6d29b4ea8","seq":1,"event":"intent","machine":"34c234191746341ccecb4182935a7b2e","lineage":"6ac665769a116a7d1b3f952b5ae36242","effect":"read","fd":0,"id":0,"value":0,"buf":0,"len":0,"digest":"0xb540caca4b83a380"}
{"prev":"b17afb7fd4478ea8","seq":1,"event":"commit","machine":"34c234191746341ccecb4182935a7b2e","lineage":"6ac665769a116a7d1b3f952b5ae36242","result":81,"error":0}
{"prev":"c2250999ed9d466e","seq":2,"event":"intent","machine":"34c234191746341ccecb4182935a7b2e","lineage":"6ac665769a116a7d1b3f952b5ae36242","effect":"printc","fd":0,"id":0,"value":81,"buf":0,"len":0,"digest":"0x982310f65e653fb0"}
{"prev":"e6a99e1c2e1492e2","seq":2,"event":"commit","machine":"34c234191746341ccecb4182935a7b2e","lineage":"6ac665769a116a7d1b3f952b5ae36242","result":0,"error":0}
{"prev":"ac162a407295dcd8","seq":2,"event":"halt","machine":"34c234191746341ccecb4182935a7b2e","lineage":"6ac665769a116a7d1b3f952b5ae36242","error":0,"pc":3,"reason":"halted"}
```

### Snapshot afterwards

```
bytes:  135332
sha256: e5c3cb2bf0d4fcf30039fc3e1108a33de96c5790ac381110724625d982f41e89
```

## Result
All checks passed.
