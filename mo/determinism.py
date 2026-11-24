"""Static determinism guard for the judge core (premortem #6).

Replay is only trustworthy if the verdict engine learns time exclusively from
`ZeusEvent.ts`. A wall-clock read anywhere in the judge core -- `time.time()`,
`datetime.now()`, a sleep -- would make the same trace produce different
verdicts on different runs. This module scans the core's source and reports
every clock access so a test (and CI) can fail on it.

It is deliberately a *lint*, not a runtime sandbox: the CLI and the adapters
are allowed to read the wall clock, because that is where live ticks are minted.
"""
from __future__ import annotations

import ast
from dataclasses import dataclass
from pathlib import Path

# The judge core: everything that decides a verdict. `cli.py`, `report.py` and
# `adapters/` stay outside on purpose: they may read the clock for ticks,
# display timestamps, or sleeping between polls without breaking replay.
CORE_MODULES = (
    "engine.py",
    "events.py",
    "gate.py",
    "ledger.py",
    "parser.py",
    "rules.py",
)

_CLOCK_MODULES = frozenset({"time", "datetime", "timeit"})


@dataclass(frozen=True)
class Finding:
    path: str
    line: int
    symbol: str
    reason: str


def _root_name(node: ast.AST) -> str | None:
    while isinstance(node, ast.Attribute):
        node = node.value
    if isinstance(node, ast.Name):
        return node.id
    return None


def _call_name(node: ast.AST) -> str | None:
    parts: list[str] = []
    while isinstance(node, ast.Attribute):
        parts.append(node.attr)
        node = node.value
    if isinstance(node, ast.Name):
        parts.append(node.id)
        return ".".join(reversed(parts))
    return None


def _clock_aliases(tree: ast.AST) -> dict[str, str]:
    """Map local names to the clock module they were imported from."""
    aliases: dict[str, str] = {}
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            for alias in node.names:
                root = alias.name.split(".")[0]
                if root in _CLOCK_MODULES:
                    aliases[alias.asname or root] = alias.name
        elif isinstance(node, ast.ImportFrom):
            module = node.module or ""
            if module.split(".")[0] in _CLOCK_MODULES:
                for alias in node.names:
                    aliases[alias.asname or alias.name] = f"{module}.{alias.name}"
    return aliases


def scan_source(source: str, path: str = "<source>") -> list[Finding]:
    """Return every clock access in `source`, including aliased imports/calls."""
    tree = ast.parse(source, filename=path)
    aliases = _clock_aliases(tree)
    findings: list[Finding] = []

    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            for alias in node.names:
                if alias.name.split(".")[0] in _CLOCK_MODULES:
                    findings.append(Finding(
                        path, node.lineno, alias.name,
                        "imports a wall-clock module",
                    ))
        elif isinstance(node, ast.ImportFrom):
            module = node.module or ""
            if module.split(".")[0] in _CLOCK_MODULES:
                for alias in node.names:
                    findings.append(Finding(
                        path, node.lineno, f"{module}.{alias.name}",
                        "imports a wall-clock symbol",
                    ))
        elif isinstance(node, ast.Call):
            root = _root_name(node.func)
            if root is not None and root in aliases:
                findings.append(Finding(
                    path, node.lineno, _call_name(node.func) or root,
                    f"calls into {aliases[root]}",
                ))

    return findings


def scan_core(root: str | Path | None = None) -> list[Finding]:
    """Scan the judge core that ships next to this module."""
    base = Path(root) if root is not None else Path(__file__).resolve().parent
    findings: list[Finding] = []
    for name in CORE_MODULES:
        path = base / name
        findings.extend(scan_source(path.read_text(encoding="utf-8"), f"mo/{name}"))
    return findings
