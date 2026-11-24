# Journal integrity transcript

Captured by `tests/journal_transcript.sh` against the real `./zeus` binary on `Linux 6.1.102 x86_64` at `2026-11-24T15:34:08Z`.

Chain verification, tamper detection, resolving an ambiguous intent, and snapshot head binding.

## Provenance

```
binary:       ./zeus
sha256:       016f49aea2a0825e87a2ae0ce4a618666e9439f8582e64536174b486c549fdcf
binary mtime: 2026-11-24T15:33:58Z
last PR C source edit: 2026-11-24T15:29:54Z (src/main.c)
sources checked: src/journal.c src/journal.h src/supervisor.c src/snapshot.c src/main.c include/supervisor.h include/snapshot.h Makefile
captured:     2026-11-24T15:34:08Z
binary postdates every PR C source: yes
```
- PASS binary postdates every PR C source file

## 1. A clean chain verifies

```bash
./zeus run-asm p.zasm --journal clean.jsonl < /dev/null
```

```

[exit 0]
```
- PASS clean run exits 0

```bash
./zeus journal verify clean.jsonl
```

```
journal verify: ok (4 records, chain verified)
[exit 0]
```
- PASS verify exits 0
- PASS verify reports the chain verified

```bash
head -2 clean.jsonl
```

```
{"prev":"0000000000000000","seq":0,"event":"start","machine":"e174019cff0ea62143e10206e5911e23","lineage":"a7c34f7e606cca775088033f61f49429","raw":0}
{"prev":"ae110e409517e0f9","seq":1,"event":"intent","machine":"e174019cff0ea62143e10206e5911e23","lineage":"a7c34f7e606cca775088033f61f49429","effect":"read","fd":0,"id":0,"value":0,"buf":0,"len":0,"digest":"0xb540caca4b83a380"}
```

## 2. One flipped byte is located

```bash
# the same journal with one hex digit of line 2's machine id changed
```

```
journal verify: line 3: prev 0x3e924e8ae78eb7dc does not match previous record 0x148ee9e0bd7c0257 (chain break)
[exit 1]
```
- PASS tampered journal exits 1
- PASS verify names line 3
- PASS verify calls it a chain break

## 3. An ambiguous legacy journal needs resolve

```bash
./zeus run-asm p.zasm --ask --journal amb-seed.jsonl --save amb-seed.zsnap
```

```
suspended: read seq 1 parked for approval (digest 0xb540caca4b83a380)
snapshot:  /tmp/tmp.4PcDO2QTzN/amb-seed.zsnap
dossier:   /tmp/tmp.4PcDO2QTzN/amb-seed.zsnap.dossier
approve:   zeus approve /tmp/tmp.4PcDO2QTzN/amb-seed.zsnap.dossier --out /tmp/tmp.4PcDO2QTzN/amb-seed.zsnap.dossier.seq1.approve
resume:    zeus restore /tmp/tmp.4PcDO2QTzN/amb-seed.zsnap --journal /tmp/tmp.4PcDO2QTzN/amb-seed.jsonl --ask --approve /tmp/tmp.4PcDO2QTzN/amb-seed.zsnap.dossier.seq1.approve
deny:      zeus deny /tmp/tmp.4PcDO2QTzN/amb-seed.zsnap.dossier --journal /tmp/tmp.4PcDO2QTzN/amb-seed.jsonl
[exit 3]
```
- PASS seed park exits 3

```bash
LINEAGE=$(sed -n 's/^lineage=//p' amb-seed.zsnap.dossier)
```

```
99fc39785e8b63755ffe20e0bd1c5e9a
```

```bash
# a legacy start + unmatched intent (no prev), as an older build wrote it
```

```
{"seq":0,"event":"start","machine":"deadbeefdeadbeefdeadbeefdeadbeef","lineage":"99fc39785e8b63755ffe20e0bd1c5e9a"}
{"seq":1,"event":"intent","machine":"deadbeefdeadbeefdeadbeefdeadbeef","lineage":"99fc39785e8b63755ffe20e0bd1c5e9a","effect":"read"}
```

```bash
# before resolve: the open intent is still a wedge
./zeus run-asm p.zasm --journal ambiguous.jsonl --lineage 99fc39785e8b63755ffe20e0bd1c5e9a < /dev/null
```

```
warning: journal '/tmp/tmp.4PcDO2QTzN/ambiguous.jsonl' has legacy records without prev; chain unverified
error: journal ends in an uncommitted intent at seq 1; refusing to replay an effect that may already have happened
[exit 1]
```
- PASS the unmatched intent refuses the next run (1)
- PASS the refusal names the uncommitted intent

```bash
./zeus journal verify ambiguous.jsonl
```

```
journal verify: line 1: record has no prev (legacy chain); pass --allow-legacy to accept
[exit 1]
```
- PASS legacy verify refuses without --allow-legacy
- PASS the refusal asks for --allow-legacy

```bash
./zeus journal verify ambiguous.jsonl --allow-legacy
```

```
journal verify: ok (2 records; legacy records present, chain unverified)
[exit 0]
```
- PASS legacy verify accepts with --allow-legacy

```bash
./zeus journal resolve --journal ambiguous.jsonl --seq 1 --outcome aborted --reason 'operator resolved the crash'
```

```
resolved seq 1 as aborted
[exit 0]
```
- PASS resolve exits 0

```bash
./zeus journal resolve --journal ambiguous.jsonl --seq 1 --outcome aborted --reason again
```

```
error: journal resolve: no open intent at seq 1
[exit 1]
```
- PASS double resolve is refused

```bash
# after resolve: the same journal continues
./zeus run-asm p.zasm --journal ambiguous.jsonl --lineage 99fc39785e8b63755ffe20e0bd1c5e9a < /dev/null
```

```
warning: journal '/tmp/tmp.4PcDO2QTzN/ambiguous.jsonl' has legacy records without prev; chain unverified
[exit 0]
```
- PASS the next run proceeds
- PASS the legacy chain warns once

```bash
tail -2 ambiguous.jsonl
```

```
{"prev":"d0539bddb0c60bd5","seq":2,"event":"commit","machine":"cda505950e4a9dbda67a93bcb0668987","lineage":"99fc39785e8b63755ffe20e0bd1c5e9a","result":-1,"error":0}
{"prev":"df002af173b57b1b","seq":2,"event":"halt","machine":"cda505950e4a9dbda67a93bcb0668987","lineage":"99fc39785e8b63755ffe20e0bd1c5e9a","error":0,"pc":2,"reason":"halted"}
```

## 4. A snapshot is bound to its journal head

```bash
./zeus run-asm p.zasm --ask --journal snap-a.jsonl --save snap-a.zsnap
```

```
suspended: read seq 1 parked for approval (digest 0xb540caca4b83a380)
snapshot:  /tmp/tmp.4PcDO2QTzN/snap-a.zsnap
dossier:   /tmp/tmp.4PcDO2QTzN/snap-a.zsnap.dossier
approve:   zeus approve /tmp/tmp.4PcDO2QTzN/snap-a.zsnap.dossier --out /tmp/tmp.4PcDO2QTzN/snap-a.zsnap.dossier.seq1.approve
resume:    zeus restore /tmp/tmp.4PcDO2QTzN/snap-a.zsnap --journal /tmp/tmp.4PcDO2QTzN/snap-a.jsonl --ask --approve /tmp/tmp.4PcDO2QTzN/snap-a.zsnap.dossier.seq1.approve
deny:      zeus deny /tmp/tmp.4PcDO2QTzN/snap-a.zsnap.dossier --journal /tmp/tmp.4PcDO2QTzN/snap-a.jsonl
[exit 3]
```
- PASS park exits 3

```bash
./zeus run-asm p.zasm --journal other.jsonl
```

```bash
./zeus restore snap-a.zsnap --journal other.jsonl
```

```
error: snapshot was parked at journal head 0x0bb4866e956dd00f, which is not in '/tmp/tmp.4PcDO2QTzN/other.jsonl'; refusing to resume a machine from a different journal
[exit 1]
```
- PASS restore against a different journal exits 1
- PASS the refusal names the different journal

## Result
All checks passed.
