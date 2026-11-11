#!/usr/bin/env python3
"""LMON (Light Machine / Lucian Marin Object Notation) — single-file Python implementation.

Library API:
    loads(text: str) -> Any
    load(fp) -> Any
    dumps(obj: Any) -> str
    dump(obj: Any, fp) -> None

CLI usage:
    python3 lmon.py <file.lmon>            # Parse LMON and print formatted JSON
    python3 lmon.py --compact <file.lmon>  # Parse LMON and print compact JSON
    python3 lmon.py --lmon <file>          # Output canonical LMON (from .lmon or .json)
    python3 lmon.py --from-json <file>     # Convert JSON file to LMON
    python3 lmon.py --check <file.lmon>    # Validate LMON syntax only
    python3 lmon.py --test                 # Run conformance test suite
    python3 lmon.py --bench [file.lmon]    # Run parse benchmark
"""

from __future__ import annotations

import json
import math
import re
import sys
import time
from typing import Any, IO, List, Optional, Tuple

__all__ = ["LMONDecodeError", "loads", "load", "dumps", "dump"]

_UNQUOTED_KEY_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_-]*")
_VALID_UNQUOTED_KEY_FULL_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_-]*\Z")
_NUMBER_RE = re.compile(r"-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?")
_STRING_CHUNK_RE = re.compile(r'[^"\\\x00-\x1f\x7f]*')
_HEX4_RE = re.compile(r"[0-9A-Fa-f]{4}")
_ESCAPE_MAP = {
    '"': '"',
    "\\": "\\",
    "/": "/",
    "b": "\b",
    "f": "\f",
    "n": "\n",
    "r": "\r",
    "t": "\t",
}
_RESERVED_KEYS = frozenset(("true", "false", "null"))
_INLINE_MAX_BYTES = 80


class LMONDecodeError(ValueError):
    """Raised when an LMON document violates the LMON 1.0 specification."""

    def __init__(self, msg: str, doc: str, pos: int) -> None:
        line = doc.count("\n", 0, pos) + 1
        last_nl = doc.rfind("\n", 0, pos)
        col = pos + 1 if last_nl < 0 else pos - last_nl
        super().__init__(f"{msg}: line {line} column {col} (char {pos})")
        self.msg = msg
        self.doc = doc
        self.pos = pos
        self.lineno = line
        self.colno = col


class _Parser:
    __slots__ = ("s", "n", "pos", "line_indent", "line_start")

    def __init__(self, s: str) -> None:
        self.s = s
        self.n = len(s)
        self.pos = 0
        self.line_indent = -1
        self.line_start = 0

    def error(self, msg: str, pos: Optional[int] = None) -> LMONDecodeError:
        return LMONDecodeError(msg, self.s, self.pos if pos is None else pos)

    def peek_line(self) -> int:
        """Advance past blank lines and compute the current line's indentation level.

        Returns indentation level >= 0, or -1 at EOF. Cached until consumed.
        """
        if self.line_indent != -2:
            return self.line_indent

        s = self.s
        n = self.n
        p = self.pos

        while p < n:
            line_begin = p
            while p < n and s[p] == " ":
                p += 1
            if p >= n:
                self.pos = p
                self.line_indent = -1
                return -1
            ch = s[p]
            if ch == "\n":
                p += 1
                continue
            if ch == "\r":
                if p + 1 >= n or s[p + 1] != "\n":
                    raise self.error("bare CR is not allowed", p)
                p += 2
                continue
            if ch == "\t":
                raise self.error("tabs are not allowed for indentation", p)

            spaces = p - line_begin
            if spaces & 3:
                raise self.error("indentation must be a multiple of 4 spaces", line_begin)
            self.pos = p
            self.line_start = line_begin
            self.line_indent = spaces >> 2
            return self.line_indent

        self.pos = p
        self.line_indent = -1
        return -1

    def consume_line_end(self, allow_eof: bool) -> None:
        s = self.s
        n = self.n
        p = self.pos
        if p >= n:
            if not allow_eof:
                raise self.error("unexpected end of input; expected nested block", p)
            self.line_indent = -1
            return
        ch = s[p]
        if ch == "\n":
            self.pos = p + 1
            self.line_indent = -2
            return
        if ch == "\r":
            if p + 1 < n and s[p + 1] == "\n":
                self.pos = p + 2
                self.line_indent = -2
                return
            raise self.error("bare CR is not allowed", p)
        if ch == "\t":
            raise self.error("tabs are not allowed outside strings", p)
        if ch == " ":
            raise self.error("trailing spaces are not allowed", p)
        raise self.error(f"unexpected character {ch!r} at end of line", p)

    def parse_string(self) -> str:
        s = self.s
        n = self.n
        p = self.pos
        if p >= n or s[p] != '"':
            raise self.error("expected '\"'", p)
        p += 1
        chunks: List[str] = []

        while True:
            if p >= n:
                raise self.error("unterminated string literal", p)
            m = _STRING_CHUNK_RE.match(s, p)
            if m is not None and m.end() > p:
                chunks.append(m.group(0))
                p = m.end()
                if p >= n:
                    raise self.error("unterminated string literal", p)
            ch = s[p]
            if ch == '"':
                self.pos = p + 1
                return "".join(chunks) if len(chunks) != 1 else chunks[0]
            if ch == "\\":
                p += 1
                if p >= n:
                    raise self.error("unterminated escape sequence", p)
                esc = s[p]
                mapped = _ESCAPE_MAP.get(esc)
                if mapped is not None:
                    chunks.append(mapped)
                    p += 1
                elif esc == "u":
                    p += 1
                    hm = _HEX4_RE.match(s, p)
                    if hm is None:
                        raise self.error("invalid \\uXXXX escape sequence", p)
                    code = int(hm.group(0), 16)
                    p += 4
                    if 0xD800 <= code <= 0xDBFF:
                        if p + 6 > n or s[p] != "\\" or s[p + 1] != "u":
                            raise self.error("unpaired high surrogate in string", p)
                        hm2 = _HEX4_RE.match(s, p + 2)
                        if hm2 is None:
                            raise self.error("invalid low surrogate \\uXXXX escape", p + 2)
                        low = int(hm2.group(0), 16)
                        if not (0xDC00 <= low <= 0xDFFF):
                            raise self.error("expected low surrogate (DC00..DFFF)", p + 2)
                        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00)
                        p += 6
                    elif 0xDC00 <= code <= 0xDFFF:
                        raise self.error("unpaired low surrogate in string", p - 4)
                    chunks.append(chr(code))
                else:
                    raise self.error(f"invalid escape sequence '\\{esc}'", p - 1)
            elif ord(ch) < 0x20:
                raise self.error("unescaped control character in string", p)
            else:
                # DEL (0x7F) is legal unescaped UTF-8 in RFC 8259
                chunks.append(ch)
                p += 1

    def parse_number(self) -> Any:
        m = _NUMBER_RE.match(self.s, self.pos)
        if m is None:
            raise self.error("invalid number literal")
        end = m.end()
        # Reject trailing number characters like `01` or `1.2.3` or `1e`
        if end < self.n and self.s[end] in "0123456789.eE+-":
            raise self.error("malformed number literal", end)
        raw = m.group(0)
        self.pos = end
        if "." in raw or "e" in raw or "E" in raw:
            val = float(raw)
            if math.isnan(val) or math.isinf(val):
                raise self.error("numeric literal out of finite IEEE-754 range", m.start())
            return val
        return int(raw)

    def parse_inline_array(self) -> List[Any]:
        s = self.s
        n = self.n
        self.pos += 1  # skip '['
        while self.pos < n and s[self.pos] == " ":
            self.pos += 1
        if self.pos < n and s[self.pos] == "]":
            self.pos += 1
            return []

        items: List[Any] = []
        while True:
            items.append(self.parse_inline_value())
            while self.pos < n and s[self.pos] == " ":
                self.pos += 1
            if self.pos >= n:
                raise self.error("unterminated inline array")
            ch = s[self.pos]
            if ch == ",":
                self.pos += 1
                while self.pos < n and s[self.pos] == " ":
                    self.pos += 1
                if self.pos < n and s[self.pos] == "]":
                    raise self.error("trailing comma in inline array")
            elif ch == "]":
                self.pos += 1
                return items
            elif ch == "\t":
                raise self.error("tabs are not allowed outside strings")
            else:
                raise self.error(f"expected ',' or ']' in inline array, got {ch!r}")

    def parse_inline_value(self) -> Any:
        s = self.s
        n = self.n
        p = self.pos
        if p >= n:
            raise self.error("expected value")
        ch = s[p]
        if ch == '"':
            return self.parse_string()
        if ch == "-" or ("0" <= ch <= "9"):
            return self.parse_number()
        if ch == "t":
            if s.startswith("true", p):
                self.pos = p + 4
                return True
            raise self.error("invalid literal; expected 'true'")
        if ch == "f":
            if s.startswith("false", p):
                self.pos = p + 5
                return False
            raise self.error("invalid literal; expected 'false'")
        if ch == "n":
            if s.startswith("null", p):
                self.pos = p + 4
                return None
            raise self.error("invalid literal; expected 'null'")
        if ch == "[":
            return self.parse_inline_array()
        if ch == "{":
            if p + 1 < n and s[p + 1] == "}":
                self.pos = p + 2
                return {}
            raise self.error("curly-brace objects are not allowed in LMON; use indentation or '{}'")
        raise self.error(f"unexpected character {ch!r} when parsing value")

    def parse_key(self) -> str:
        s = self.s
        p = self.pos
        if p < self.n and s[p] == '"':
            return self.parse_string()
        m = _UNQUOTED_KEY_RE.match(s, p)
        if m is None:
            raise self.error("expected object key")
        key = m.group(0)
        if key in _RESERVED_KEYS:
            raise self.error(f"reserved literal {key!r} must be quoted when used as a key", p)
        self.pos = m.end()
        return key

    def is_dash_item(self) -> bool:
        s = self.s
        p = self.pos
        if p >= self.n or s[p] != "-":
            return False
        if p + 1 >= self.n:
            return True
        nxt = s[p + 1]
        return nxt == " " or nxt == "\n" or nxt == "\r"

    def parse_nested_block(self, expected_indent: int) -> Any:
        indent = self.peek_line()
        if indent < 0:
            raise self.error("expected indented block, got end of input")
        if indent != expected_indent:
            raise self.error(
                f"expected indentation level {expected_indent} ({expected_indent * 4} spaces), "
                f"got {indent * 4} spaces",
                self.line_start,
            )
        if self.is_dash_item():
            return self.parse_block_array(expected_indent)

        ch = self.s[self.pos]
        if ch == '"':
            str_start = self.pos
            first_key = self.parse_string()
            if self.pos >= self.n or self.s[self.pos] != ":":
                raise self.error("nested block cannot be a standalone scalar", str_start)
            return self.parse_block_object(expected_indent, first_key=first_key)
        if ("A" <= ch <= "Z") or ("a" <= ch <= "z") or ch == "_":
            return self.parse_block_object(expected_indent)
        raise self.error("nested block must be a block object or block array")

    def parse_block_object(self, expected_indent: int, first_key: Optional[str] = None) -> dict:
        obj: dict = {}
        while True:
            if first_key is not None:
                key = first_key
                first_key = None
            else:
                indent = self.peek_line()
                if indent < expected_indent:
                    break
                if indent > expected_indent:
                    raise self.error("unexpected indentation inside object", self.line_start)
                if self.is_dash_item():
                    raise self.error("cannot mix array items and object keys at same indentation")
                key = self.parse_key()

            if self.pos >= self.n or self.s[self.pos] != ":":
                raise self.error("expected ':' after object key")
            self.pos += 1

            if self.pos < self.n and self.s[self.pos] == " ":
                self.pos += 1
                val = self.parse_inline_value()
                self.consume_line_end(allow_eof=True)
            else:
                self.consume_line_end(allow_eof=False)
                val = self.parse_nested_block(expected_indent + 1)
            obj[key] = val

        return obj

    def parse_block_array(self, expected_indent: int) -> list:
        arr: list = []
        while True:
            indent = self.peek_line()
            if indent < expected_indent:
                break
            if indent > expected_indent:
                raise self.error("unexpected indentation inside array", self.line_start)
            if not self.is_dash_item():
                raise self.error("cannot mix object keys and array items at same indentation")
            self.pos += 1  # consume '-'

            if self.pos < self.n and self.s[self.pos] == " ":
                self.pos += 1
                elem = self.parse_inline_value()
                self.consume_line_end(allow_eof=True)
            else:
                self.consume_line_end(allow_eof=False)
                elem = self.parse_nested_block(expected_indent + 1)
            arr.append(elem)

        return arr

    def parse_document(self) -> Any:
        if self.s.startswith("\ufeff"):
            raise self.error("UTF-8 BOM is not allowed", 0)
        self.line_indent = -2
        indent = self.peek_line()
        if indent < 0:
            raise self.error("empty LMON document", self.pos)
        if indent != 0:
            raise self.error("root value must not be indented", self.line_start)

        if self.is_dash_item():
            result = self.parse_block_array(0)
        else:
            ch = self.s[self.pos]
            if ch == '"':
                first_str = self.parse_string()
                if self.pos < self.n and self.s[self.pos] == ":":
                    result = self.parse_block_object(0, first_key=first_str)
                else:
                    self.consume_line_end(allow_eof=True)
                    result = first_str
            elif ("A" <= ch <= "Z") or ("a" <= ch <= "z") or ch == "_":
                m = _UNQUOTED_KEY_RE.match(self.s, self.pos)
                ident = m.group(0) if m else ""
                after = m.end() if m else self.pos
                if ident in _RESERVED_KEYS and (after >= self.n or self.s[after] != ":"):
                    result = self.parse_inline_value()
                    self.consume_line_end(allow_eof=True)
                else:
                    result = self.parse_block_object(0)
            else:
                result = self.parse_inline_value()
                self.consume_line_end(allow_eof=True)

        trailing = self.peek_line()
        if trailing != -1:
            raise self.error("unexpected trailing content after root value", self.line_start)
        return result


def loads(text: str) -> Any:
    """Parse an LMON string into native Python JSON-equivalent objects."""
    if not isinstance(text, str):
        raise TypeError(f"the LMON object must be str, not {type(text).__name__}")
    return _Parser(text).parse_document()


def load(fp: IO[str]) -> Any:
    """Parse an LMON text stream into native Python JSON-equivalent objects."""
    return loads(fp.read())


# ---------------------------------------------------------------------------
# Serializer (Python objects -> canonical LMON 1.0)
# ---------------------------------------------------------------------------


def _escape_string(s: str) -> str:
    return json.dumps(s, ensure_ascii=False)


def _format_key(key: str) -> str:
    if not isinstance(key, str):
        raise TypeError(f"keys must be str, not {type(key).__name__}")
    if key not in _RESERVED_KEYS and _VALID_UNQUOTED_KEY_FULL_RE.match(key):
        return key
    return _escape_string(key)


def _format_number(val: Any) -> str:
    if isinstance(val, int):
        return str(val)
    if isinstance(val, float):
        if math.isnan(val) or math.isinf(val):
            raise ValueError("Out of range float values (NaN/Infinity) are not LMON compliant")
        if val == 0.0:
            return "0.0"
        return repr(val)
    raise TypeError(f"Object of type {type(val).__name__} is not LMON serializable")


def _try_format_inline(val: Any) -> Optional[str]:
    """Return inline LMON representation if eligible (no non-empty dict, <= 80 UTF-8 bytes)."""
    if val is None:
        return "null"
    if val is True:
        return "true"
    if val is False:
        return "false"
    if isinstance(val, str):
        return _escape_string(val)
    if isinstance(val, (int, float)):
        return _format_number(val)
    if isinstance(val, dict):
        return "{}" if len(val) == 0 else None
    if isinstance(val, (list, tuple)):
        if len(val) == 0:
            return "[]"
        parts: List[str] = []
        byte_len = 2  # '[' and ']'
        for idx, item in enumerate(val):
            rendered = _try_format_inline(item)
            if rendered is None:
                return None
            if idx > 0:
                byte_len += 2  # ", "
            byte_len += len(rendered.encode("utf-8"))
            if byte_len > _INLINE_MAX_BYTES:
                return None
            parts.append(rendered)
        return "[" + ", ".join(parts) + "]"
    raise TypeError(f"Object of type {type(val).__name__} is not LMON serializable")


def _emit_block(val: Any, indent: int, lines: List[str]) -> None:
    prefix = "    " * indent
    if isinstance(val, dict):
        for k, v in val.items():
            fkey = _format_key(k)
            inline = _try_format_inline(v)
            if inline is not None:
                lines.append(f"{prefix}{fkey}: {inline}")
            else:
                lines.append(f"{prefix}{fkey}:")
                _emit_block(v, indent + 1, lines)
    elif isinstance(val, (list, tuple)):
        for item in val:
            inline = _try_format_inline(item)
            if inline is not None:
                lines.append(f"{prefix}- {inline}")
            else:
                lines.append(f"{prefix}-")
                _emit_block(item, indent + 1, lines)
    else:
        raise TypeError(f"Object of type {type(val).__name__} is not LMON serializable")


def dumps(obj: Any) -> str:
    """Serialize a Python JSON-compatible value into canonical LMON 1.0 text."""
    inline = _try_format_inline(obj)
    if inline is not None:
        return inline + "\n"
    lines: List[str] = []
    _emit_block(obj, 0, lines)
    return "\n".join(lines) + "\n"


def dump(obj: Any, fp: IO[str]) -> None:
    """Serialize a Python JSON-compatible value to a text stream as canonical LMON."""
    fp.write(dumps(obj))


# ---------------------------------------------------------------------------
# Conformance Self-Tests & CLI
# ---------------------------------------------------------------------------


def _run_tests() -> int:
    cases: List[Tuple[str, Any]] = [
        ("empty_obj", {}),
        ("empty_arr", []),
        ("scalars", {"s": "Lemon", "i": -42, "f": 3.25, "t": True, "fa": False, "n": None}),
        ("root_int", -99),
        ("root_str", "hello: world"),
        ("root_bool", True),
        ("root_null", None),
        (
            "nested_mix",
            {
                "service": "lmon",
                "tags": ["a", "b", "c"],
                "empty_map": {},
                "empty_list": [],
                "matrix": [[1, 2], [3, 4]],
                "nodes": [
                    {"id": 1, "meta": {"ok": True}},
                    {"id": 2, "meta": {}},
                ],
                "": "empty",
                "true": "keyword",
                "false": False,
                "null": None,
                "123": "numeric-key",
                "-dash": "dash-key",
                "with space": "quoted\nline\t🍋",
            },
        ),
    ]

    for name, expected in cases:
        encoded = dumps(expected)
        decoded = loads(encoded)
        if decoded != expected:
            print(f"FAIL [{name}]: round-trip mismatch\nGot: {decoded!r}\nExp: {expected!r}", file=sys.stderr)
            return 1

    # Surrogate pair decoding check
    if loads('emoji: "\\uD83C\\uDF4B"\n') != {"emoji": "🍋"}:
        print("FAIL [surrogate_pair]", file=sys.stderr)
        return 1

    invalid_inputs = [
        "",
        "   \n",
        "  bad_indent: 1\n",
        "\ttab: 1\n",
        "dangling:\n",
        "dangling:\nstill_root: 1\n",
        "curly: {\"a\": 1}\n",
        "true: 1\n",
        "trailing_space: 1 \n",
        "two_spaces:  1\n",
        "bad_num: 01\n",
        "trailing_comma: [1, 2,]\n",
        "mixed: 1\n- 2\n",
        "# comments are forbidden\na: 1\n",
    ]
    for bad in invalid_inputs:
        try:
            loads(bad)
            print(f"FAIL [invalid accepted]: {bad!r}", file=sys.stderr)
            return 1
        except LMONDecodeError:
            pass

    print(f"Python LMON: all {len(cases) + len(invalid_inputs) + 1} conformance tests passed.")
    return 0


def _run_bench(path: Optional[str]) -> int:
    if path:
        with open(path, "r", encoding="utf-8") as f:
            sample = f.read()
    else:
        records = [
            {
                "id": i,
                "name": f"service-node-{i}",
                "active": (i % 2 == 0),
                "latency_ms": round(1.25 * (i % 17), 3),
                "tags": ["edge", "lemon", "v1"],
                "meta": {"region": "eu-central", "capacity": 256 + i, "extra": {}},
            }
            for i in range(500)
        ]
        sample = dumps({"cluster": "lmon-bench", "count": 500, "nodes": records})

    size_kb = len(sample.encode("utf-8")) / 1024.0
    iterations = max(10, int(20000 / max(1.0, size_kb)))
    t0 = time.perf_counter()
    for _ in range(iterations):
        loads(sample)
    elapsed = time.perf_counter() - t0
    mb_s = (len(sample.encode("utf-8")) * iterations) / (1024.0 * 1024.0 * elapsed)
    print(
        f"Python LMON benchmark: {iterations} iterations x {size_kb:.1f} KiB "
        f"in {elapsed:.3f}s ({mb_s:.2f} MiB/s)"
    )
    return 0


def _usage() -> str:
    return (
        "Usage: python3 lmon.py [options] <file.lmon | ->\n"
        "Options:\n"
        "  -c, --compact    Output compact single-line JSON\n"
        "  -l, --lmon       Output canonical LMON\n"
        "  --from-json      Parse input as JSON and output canonical LMON\n"
        "  --check          Validate LMON syntax only\n"
        "  --test           Run conformance test suite\n"
        "  --bench [file]   Run parse benchmark\n"
    )


def main(argv: List[str]) -> int:
    if not argv or "-h" in argv or "--help" in argv:
        sys.stdout.write(_usage())
        return 0 if argv else 1

    if argv[0] == "--test":
        return _run_tests()
    if argv[0] == "--bench":
        return _run_bench(argv[1] if len(argv) > 1 else None)

    compact = False
    out_lmon = False
    from_json = False
    check_only = False
    target: Optional[str] = None

    for arg in argv:
        if arg in ("-c", "--compact"):
            compact = True
        elif arg in ("-l", "--lmon"):
            out_lmon = True
        elif arg == "--from-json":
            from_json = True
            out_lmon = True
        elif arg == "--check":
            check_only = True
        elif arg.startswith("-") and arg != "-":
            sys.stderr.write(f"Unknown option: {arg}\n{_usage()}")
            return 2
        else:
            target = arg

    if target is None:
        sys.stderr.write(_usage())
        return 2

    try:
        if target == "-":
            raw = sys.stdin.read()
        else:
            with open(target, "r", encoding="utf-8") as f:
                raw = f.read()
            if target.endswith(".json"):
                from_json = True

        data = json.loads(raw) if from_json else loads(raw)
        if check_only:
            return 0
        if out_lmon:
            sys.stdout.write(dumps(data))
        elif compact:
            sys.stdout.write(json.dumps(data, ensure_ascii=False, separators=(",", ":")) + "\n")
        else:
            sys.stdout.write(json.dumps(data, ensure_ascii=False, indent=2) + "\n")
        return 0
    except (LMONDecodeError, json.JSONDecodeError, OSError, ValueError) as exc:
        sys.stderr.write(f"lmon error: {exc}\n")
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
