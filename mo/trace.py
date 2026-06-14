"""Read a recorded ZeusEvent stream from JSONL (M0: replay-only).

One JSON object per line. Blank lines ignored. This is the file form of the
adapter↔MO seam: the adapter writes these, MO never knows what produced them.
"""
from __future__ import annotations

import json
from typing import Iterator

from .events import ZeusEvent


def read_jsonl(path: str) -> Iterator[ZeusEvent]:
    with open(path, "r") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            obj = json.loads(line)
            yield ZeusEvent(
                primitive=obj["primitive"],
                identifier=obj.get("identifier"),
                payload=obj.get("payload", {}),
                ts=float(obj.get("ts", 0.0)),
            )
