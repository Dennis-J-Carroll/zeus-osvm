"""MO command-line entry point.

    mo eval <spec.py> <trace.jsonl>

M0 loads the spec as a Python module that defines a module-level `spec`
(a rules.Spec). The .zspec text parser arrives in M1 and will slot in here
behind the same `load_spec` call.

Exit code: 0 if the session is clean, 1 if any BOLTED or CONDEMNED verdict
fired — so `mo eval` is usable as a CI gate.
"""
from __future__ import annotations

import importlib.util
import sys

from .engine import run
from .report import human_summary, jsonl_lines
from .rules import Spec
from .trace import read_jsonl


def load_spec(path: str) -> Spec:
    spec_module = importlib.util.spec_from_file_location("_mo_spec", path)
    if spec_module is None or spec_module.loader is None:
        raise ValueError(f"cannot load spec from {path}")
    module = importlib.util.module_from_spec(spec_module)
    spec_module.loader.exec_module(module)
    return module.spec


def main(argv: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    if len(argv) != 3 or argv[0] != "eval":
        print("usage: mo eval <spec.py> <trace.jsonl>", file=sys.stderr)
        return 2

    _, spec_path, trace_path = argv
    spec = load_spec(spec_path)
    result = run(read_jsonl(trace_path), spec)

    for line in jsonl_lines(result):
        print(line)
    print(human_summary(result), file=sys.stderr)

    deviations = sum(1 for v in result.verdicts if v.kind in ("BOLTED", "CONDEMNED"))
    return 1 if deviations else 0


if __name__ == "__main__":
    raise SystemExit(main())
