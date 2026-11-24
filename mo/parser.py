"""The .zspec text parser (M1). Line-oriented, like the zasm assembler.

Produces the same rules.Spec / TriggerRule / AssertRule objects the engine
already consumes, so M0's loop is untouched. Grammar v0:

    ; comment to end of line
    INCLUDE <path>
    on <primitive> as $<bind>:
        EXPECT <primitive> for $<bind>
        WINDOW <ms>

    ASSERT no <primitiveA> where identifier not in <primitiveB> [GRACE <ms>]

`INCLUDE` composes fragments: the file is parsed at the point of inclusion,
relative to the including file, into the same spec. It is include-once (a
diamond adds one copy of the shared rules), and a cycle or missing file is a
parse error at the `INCLUDE` line. Rules remember the file they came from so a
verdict's `spec_line` is never ambiguous across fragments.

Errors carry a 1-based line number (assembler-style) and the source file, so a
bad spec points at the offending line rather than failing opaquely.
"""
from __future__ import annotations

import math
import os

from .events import ZeusEvent
from .rules import AssertRule, Spec, TriggerRule


class ParseError(Exception):
    pass


def _err(source: str | None, line: int, message: str) -> ParseError:
    prefix = f"{source}: " if source else ""
    return ParseError(f"{prefix}line {line}: {message}")


def _membership_predicate(primitive_a: str, primitive_b: str):
    # violation: an event of primitive_a whose identifier was never seen on
    # any primitive_b event. (factory avoids loop-closure capture bugs)
    def pred(ev: ZeusEvent, seen: dict) -> bool:
        return (ev.primitive == primitive_a
                and ev.identifier not in seen.get(primitive_b, set()))
    return pred


def _strip_comment(raw: str) -> str:
    return raw.split(";", 1)[0]


class _Block:
    def __init__(self, on_primitive: str, bind: str, line: int):
        self.on_primitive = on_primitive
        self.bind = bind
        self.line = line
        self.expects: str | None = None
        self.window: float | None = None


def _parse_on(tokens: list[str], line: int, source: str | None) -> _Block:
    # on <primitive> as $<bind>:
    if len(tokens) != 4 or tokens[2] != "as" or not tokens[3].startswith("$"):
        raise _err(source, line,
                   "expected 'on <primitive> as $<bind>:'")
    return _Block(on_primitive=tokens[1], bind=tokens[3][1:], line=line)


def _parse_expect(tokens: list[str], block: _Block, line: int,
                  source: str | None) -> None:
    # EXPECT <primitive> for $<bind>
    if len(tokens) != 4 or tokens[2] != "for" or not tokens[3].startswith("$"):
        raise _err(source, line, "expected 'EXPECT <primitive> for $<bind>'")
    bind = tokens[3][1:]
    if bind != block.bind:
        raise _err(
            source, line,
            f"binding ${bind} does not match block binding ${block.bind}",
        )
    block.expects = tokens[1]


def _parse_window(tokens: list[str], block: _Block, line: int,
                  source: str | None) -> None:
    # WINDOW <ms>
    if len(tokens) != 2:
        raise _err(source, line, "expected 'WINDOW <ms>'")
    try:
        block.window = float(tokens[1])
    except ValueError:
        raise _err(source, line,
                   f"WINDOW value '{tokens[1]}' is not a number")


def _parse_assert(tokens: list[str], line: int,
                  source: str | None) -> AssertRule:
    # ASSERT no <A> where identifier not in <B> [GRACE <ms>]
    shape = ["ASSERT", "no", None, "where", "identifier", "not", "in", None]
    if len(tokens) not in (8, 10) or any(
        want is not None and got != want
        for got, want in zip(tokens, shape)
    ):
        raise _err(
            source, line,
            "expected 'ASSERT no <primitive> where identifier not in "
            "<primitive> [GRACE <ms>]'",
        )
    primitive_a, primitive_b = tokens[2], tokens[7]

    grace: float | None = None
    if len(tokens) == 10:
        if tokens[8] != "GRACE":
            raise _err(source, line,
                       "expected 'GRACE <ms>' after the ASSERT predicate")
        try:
            grace = float(tokens[9])
        except ValueError:
            raise _err(source, line,
                       f"GRACE value '{tokens[9]}' is not a number")
        if not math.isfinite(grace) or grace <= 0:
            raise _err(source, line,
                       "GRACE must be a positive finite number of ms")

    detail = {"rule": f"no {primitive_a} where identifier not in {primitive_b}"}
    if grace is not None:
        detail["grace_ms"] = grace
    return AssertRule(
        spec_line=line,
        predicate=_membership_predicate(primitive_a, primitive_b),
        detail=detail,
        grace_ms=grace,
        primitive_a=primitive_a,
        primitive_b=primitive_b,
        spec_source=source,
    )


def _finalize(block: _Block, source: str | None) -> TriggerRule:
    if block.expects is None:
        raise _err(source, block.line, "'on' block missing EXPECT")
    if block.window is None:
        raise _err(source, block.line, "'on' block missing WINDOW")
    return TriggerRule(
        on_primitive=block.on_primitive,
        expects=block.expects,
        window_ms=block.window,
        spec_line=block.line,
        spec_source=source,
    )


class _Parser:
    def __init__(self) -> None:
        self.spec = Spec()
        # canonical paths already composed, so a diamond includes one copy
        self.included: set[str] = set()
        # the active include chain, for a precise cycle error
        self.stack: list[str] = []

    def parse_text(self, text: str, source: str | None,
                   base_dir: str) -> Spec:
        block: _Block | None = None

        for n, raw in enumerate(text.splitlines(), start=1):
            content = _strip_comment(raw)
            if not content.strip():
                continue
            indented = content[:1].isspace()
            tokens = content.replace(":", " ").split()

            if indented:
                if block is None:
                    raise _err(source, n,
                               "indented line outside any 'on' block")
                if tokens[0] == "EXPECT":
                    _parse_expect(tokens, block, n, source)
                elif tokens[0] == "WINDOW":
                    _parse_window(tokens, block, n, source)
                else:
                    raise _err(source, n,
                               f"unknown directive '{tokens[0]}'")
                continue

            # top-level line: finalize any open block first
            if block is not None:
                self.spec.triggers.append(_finalize(block, source))
                block = None

            if tokens[0] == "on":
                block = _parse_on(tokens, n, source)
            elif tokens[0] == "ASSERT":
                self.spec.assertions.append(_parse_assert(tokens, n, source))
            elif tokens[0] == "INCLUDE":
                self._include(tokens, n, source, base_dir)
            else:
                raise _err(source, n, f"unknown directive '{tokens[0]}'")

        if block is not None:
            self.spec.triggers.append(_finalize(block, source))

        return self.spec

    def _include(self, tokens: list[str], line: int, source: str | None,
                 base_dir: str) -> None:
        if len(tokens) != 2:
            raise _err(source, line, "expected 'INCLUDE <path>'")
        target = tokens[1]
        if not os.path.isabs(target):
            target = os.path.join(base_dir or ".", target)
        target = os.path.normpath(target)
        real = os.path.realpath(target)

        if real in self.stack:
            chain = " -> ".join(
                os.path.basename(path) for path in (*self.stack, real)
            )
            raise _err(source, line, f"include cycle: {chain}")
        if real in self.included:
            return   # a diamond must add one copy, not two obligations

        try:
            with open(target, "r", encoding="utf-8") as fh:
                text = fh.read()
        except OSError as exc:
            raise _err(source, line,
                       f"cannot include '{tokens[1]}': {exc.strerror}")

        self.included.add(real)
        self.stack.append(real)
        try:
            self.parse_text(text, target, os.path.dirname(target) or ".")
        finally:
            self.stack.pop()


def parse(text: str, *, source: str | None = None) -> Spec:
    base_dir = os.path.dirname(source) if source else "."
    return _Parser().parse_text(text, source, base_dir)


def parse_file(path: str) -> Spec:
    parser = _Parser()
    real = os.path.realpath(path)
    parser.included.add(real)
    parser.stack.append(real)
    with open(path, "r", encoding="utf-8") as fh:
        return parser.parse_text(fh.read(), path,
                                 os.path.dirname(path) or ".")
