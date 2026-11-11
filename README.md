# LMON — Light Machine Object Notation

**LMON** — **Light Machine Object Notation** (or **Lucian Marin Object Notation**), pronounced **"Lemon"** — is a human-friendly, machine-fast data serialization format.

LMON is a **strict, lossless 1-to-1 conversion of JSON** with **no curly braces** for structured objects and **Python-style 4-space indentation**. It eliminates JSON's noisy braces, commas, and key quotes while avoiding YAML's complex state machines, implicit type coercion traps, and slow parsing.

- **Extension:** `.lmon`
- **Media Type:** `application/lmon`
- **Encoding:** UTF-8 (without BOM)
- **Version:** 1.0

---

## Design Goals

1. **Bijective JSON Equivalence:** Every valid JSON document maps losslessly to LMON and back to the exact same JSON data model (`object`, `array`, `string`, `number`, `true`, `false`, `null`).
2. **Pythonic Readability:** No `{` `}` curly braces around objects, no trailing comma errors, and clean 4-space block indentation.
3. **Zero Ambiguity (No YAML Footguns):** Strings are always double-quoted (`"NO"` is never coerced to `false`, `"1.20"` never loses trailing zeros), `null` is explicit (`key:` without a nested block is a syntax error, not silent `null`), and there are no comments, anchors, tags, or multiline folding modes.
4. **Faster Than YAML — and Competitive With or Faster Than JSON:** Fixed 4-space indentation (`0x20202020`), single-byte `LL(1)` token dispatch, unquoted ASCII keys requiring zero escape scanning, and fewer structural delimiters enable single-pass arena parsers that run at hundreds of MB/s.

---

## Side-by-Side Comparison

### LMON (`.lmon`)

```lmon
service: "lmon-gateway"
version: 1
production: true
deprecation_date: null
metadata: {}
tags: ["fast", "strict", "pythonic"]
empty_list: []
limits:
    max_connections: 1024
    timeout_sec: 15.5
upstreams:
    -
        host: "10.0.0.1"
        port: 8080
        healthy: true
    -
        host: "10.0.0.2"
        port: 8081
        healthy: false
"http/headers":
    "x-powered-by": "Lemon"
    "200": "OK"
```

### Equivalent JSON (`.json`)

```json
{
  "service": "lmon-gateway",
  "version": 1,
  "production": true,
  "deprecation_date": null,
  "metadata": {},
  "tags": ["fast", "strict", "pythonic"],
  "empty_list": [],
  "limits": {
    "max_connections": 1024,
    "timeout_sec": 15.5
  },
  "upstreams": [
    {
      "host": "10.0.0.1",
      "port": 8080,
      "healthy": true
    },
    {
      "host": "10.0.0.2",
      "port": 8081,
      "healthy": false
    }
  ],
  "http/headers": {
    "x-powered-by": "Lemon",
    "200": "OK"
  }
}
```

---

## LMON 1.0 Specification

### 1. Data Model

LMON shares the exact [RFC 8259](https://datatracker.ietf.org/doc/html/rfc8259) JSON data model:

- **Object:** Ordered or unordered collection of string-keyed name/value pairs.
- **Array:** Ordered sequence of values.
- **String:** Sequence of Unicode scalar values.
- **Number:** Signed decimal integer or floating-point number.
- **Boolean:** `true` or `false`.
- **Null:** `null`.

LMON defines **no comments**; every non-blank line represents pure structured data.

### 2. Encoding, Lines, and Indentation

1. **UTF-8:** Documents MUST be valid UTF-8 without a byte-order mark (`U+FEFF` at byte 0 is a syntax error).
2. **Line Endings:** Lines are separated by `LF` (`\n`, `0x0A`) or `CRLF` (`\r\n`, `0x0D 0x0A`). A bare `CR` (`\r`) outside a string literal is invalid. The final line MAY omit a trailing newline.
3. **Blank Lines:** Lines containing zero characters or only ASCII spaces (`0x20`) before the line ending are blank lines and are ignored anywhere in the document.
4. **4-Space Indentation:**
   - Every non-blank line begins with `4 * N` ASCII space bytes (`0x20`), where `N >= 0` is the line's **indentation level**.
   - Any leading space count not divisible by 4 (`spaces % 4 != 0`) is a **syntax error**.
   - Tab characters (`\t`, `0x09`) anywhere outside a double-quoted string are a **syntax error**.
5. **No Trailing Spaces:** Non-blank lines MUST NOT have trailing whitespace after tokens (`key:`, `-`, or inline values).

### 3. Scalar & Empty Container Literals

All scalar tokens are identical to RFC 8259 JSON:

| Type | Syntax | Examples |
| :--- | :--- | :--- |
| **String** | Double-quoted UTF-8 with JSON escapes (`\"`, `\\`, `\/`, `\b`, `\f`, `\n`, `\r`, `\t`, `\uXXXX` including UTF-16 surrogate pairs). Control bytes `0x00..0x1F` MUST be escaped. Single-line only. | `"Lemon"`, `"line 1\nline 2"`, `"\uD83C\uDF4B"` |
| **Number** | `-?(0\|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?` (no leading zeros, no `+` prefix, no `NaN`/`Infinity`). | `0`, `-42`, `3.14159`, `6.022e23` |
| **Boolean** | Exact lowercase `true` or `false`. | `true`, `false` |
| **Null** | Exact lowercase `null`. | `null` |
| **Empty Object** | Exact two-byte token `{}`. Non-empty curly-brace objects (`{"a": 1}`) are **forbidden**. | `{}` |
| **Empty Array** | `[]` (optional ASCII spaces inside brackets: `[ ]` is permitted on input; `[]` is canonical). | `[]` |

### 4. Object Keys

Object keys appear immediately before `:` (with no space before `:`) in one of two forms:

1. **Unquoted Identifier Key:**
   - Matches `^[A-Za-z_][A-Za-z0-9_-]*$`.
   - MUST NOT be a reserved literal (`true`, `false`, `null`).
   - Examples: `id`, `first_name`, `content-type`, `_meta`, `v2`.
2. **Quoted String Key:**
   - Any valid RFC 8259 JSON string `"..."`.
   - Required when a key is empty (`""`), starts with a digit or hyphen (`"200"`, `"-flag"`), equals a reserved literal (`"true"`, `"false"`, `"null"`), or contains spaces, punctuation, escapes, or non-ASCII characters (`"first name"`, `"a:b"`).
   - Quoted keys are always valid on input, even when the key could have been unquoted.

If an object contains duplicate keys, parsers SHOULD preserve order or apply last-value-wins map semantics consistent with JSON parsers in the host language.

### 5. Block Objects

A non-empty object at indentation level `N` is written as one or more key/value entries at indentation level `N` without surrounding braces:

- **Inline property:** `<indent(N)><key>: <inline-value><EOL>`
  - Exactly **one** ASCII space (`0x20`) MUST follow `:`.
- **Nested block property:** `<indent(N)><key>:<EOL>`
  - Immediately followed on the next non-blank line by a nested **Block Object** or **Block Array** at indentation level `N + 1`.
  - If EOF occurs or the next non-blank line has indentation `<= N` or `> N + 1`, the parser MUST raise a syntax error.
  - A nested block at `N + 1` MUST be a Block Object or Block Array (never a standalone scalar; scalars are always written inline).

### 6. Arrays (Inline and Block)

LMON supports both compact inline arrays and indented block arrays:

1. **Inline Array:** `[<inline-value>, <inline-value>, ...]`
   - Single-line comma-separated sequence of `<inline-value>` elements.
   - Optional ASCII spaces (`0x20`) are allowed after `[`, before `]`, and around `,`.
   - Trailing commas (`[1, 2,]`) are invalid.
   - May contain scalars, `{}`, `[]`, and nested inline arrays (`[[1, 2], [3, 4]]`), but NEVER non-empty objects (since `{...}` is forbidden).

2. **Block Array:** One or more `-` entries at indentation level `N`:
   - **Inline element:** `<indent(N)>- <inline-value><EOL>`
     - `-` followed by a single ASCII space (`0x20`) and an `<inline-value>`.
   - **Nested block element:** `<indent(N)>-<EOL>`
     - `-` followed immediately by `<EOL>`, then a nested **Block Object** or **Block Array** at indentation level `N + 1`.

Block array items (`-`) and block object entries (`key:`) MUST NOT be mixed at the same indentation level within the same block.

### 7. Root Document Value

A valid LMON document contains at least one non-blank line at indentation level `0` and represents exactly one root JSON value:

1. **Root Block Object:** One or more `key: ...` entries at indentation level `0`.
2. **Root Block Array:** One or more `- ...` entries at indentation level `0`.
3. **Root Inline Value:** A single `<inline-value>` on a single line at indentation level `0` (followed only by optional blank lines). For example, `{}`, `[]`, `[1, 2, 3]`, `"hello"`, `42`, `-10`, `true`, or `null`.

Note how root disambiguation requires zero backtracking:
- `-` followed by `' '` or `<EOL>` starts a Root Block Array; `-` followed immediately by `'0'..'9'` is a negative root number.
- `"` starts a JSON string; if immediately followed by `:`, it is the first quoted key of a Root Block Object, otherwise it is a root string literal.

### 8. Canonical Serialization Rules

To produce deterministic LMON from any JSON value:

1. Use `\n` line endings, 4 spaces per indentation level, and a single trailing `\n` at end of document.
2. Serialize keys unquoted when matching `^[A-Za-z_][A-Za-z0-9_-]*$` and not in `{"true", "false", "null"}`; otherwise serialize as a JSON-escaped double-quoted string.
3. Serialize empty objects as `{}` and empty arrays as `[]`.
4. Serialize a non-empty array **inline** (`[a, b, c]` with `, ` separators) if and only if:
   - It contains no non-empty object at any nesting depth, **and**
   - Its UTF-8 inline representation is at most **80 bytes**.
   Otherwise, serialize it as a **Block Array** (`- ...`).

### 9. Formal Grammar (EBNF)

```ebnf
document       = blank_lines ( block_object(0) | block_array(0) | inline_root ) blank_lines ;
inline_root    = inline_value ( line_end | EOF ) ;

block(n)       = block_object(n) | block_array(n) ;

block_object(n)= obj_entry(n) { blank_lines obj_entry(n) } ;
obj_entry(n)   = indent(n) key ":" ( " " inline_value ( line_end | EOF )
                                   | line_end blank_lines block(n + 1) ) ;

block_array(n) = arr_entry(n) { blank_lines arr_entry(n) } ;
arr_entry(n)   = indent(n) "-" ( " " inline_value ( line_end | EOF )
                               | line_end blank_lines block(n + 1) ) ;

key            = unquoted_key | json_string ;
unquoted_key   = ( ALPHA | "_" ) { ALPHA | DIGIT | "_" | "-" } - ( "true" | "false" | "null" ) ;

inline_value   = json_string | json_number | "true" | "false" | "null" | "{}" | inline_array ;
inline_array   = "[" ws [ inline_value ws { "," ws inline_value ws } ] "]" ;

indent(n)      = (4 * n) * %x20 ;
ws             = { %x20 } ;
blank_lines    = { ws line_end } ;
line_end       = [ %x0D ] %x0A ;
```

---

## Why LMON Is Fast to Parse

| Property | YAML 1.2 | JSON (RFC 8259) | LMON 1.0 |
| :--- | :--- | :--- | :--- |
| **Indentation scanner** | Arbitrary spaces, chomping/folding states | Arbitrary whitespace skip | **4-byte word compare (`0x20202020`)** |
| **Object delimiters** | Flow `{}` + block mapping + complex keys | `{`, `}`, `,` on every object/field | **None (newline + indent only)** |
| **Object key scanning** | Multi-rule implicit scalar / colon lookahead | Quote + `\` escape scan on every key | **Raw ASCII scan to `:` (zero escapes)** |
| **Type resolution** | Regex/tag inference (`yes`, `1.0`, ISO dates) | **1st-byte dispatch** | **1st-byte dispatch** |
| **Anchors / aliases** | Arbitrary graph references (`&id`, `*id`) | None | **None (strict tree)** |

1. **Word-Stride Indentation:** Checking one indentation level is a single 32-bit comparison against `0x20202020` (`"    "`), turning structural nesting into integer division (`spaces >> 2`).
2. **Zero-Copy Unquoted Keys:** The vast majority of object keys match `[A-Za-z_][A-Za-z0-9_-]*`, meaning no opening/closing quotes and **zero backslash-escape branches**—parsers slice key bytes directly from the input buffer.
3. **Single-Byte Dispatch:** At any value position, the first byte (`"`, `-`/`0-9`, `t`, `f`, `n`, `[`, `{`) uniquely determines the parser branch with zero backtracking.
4. **Smaller Byte Footprint:** Removing `{`, `}`, `,`, and key quotes typically shrinks pretty-printed JSON payloads by 20–35%, reducing memory bandwidth and lexer iterations.

---

## Single-File Reference Implementations

This repository provides self-contained, zero-dependency single-file libraries and CLIs in five languages:

| Language | File | Library API | Build / Run |
| :--- | :--- | :--- | :--- |
| **Python** | [`lmon.py`](lmon.py) | `loads`, `load`, `dumps`, `dump` | `python3 lmon.py examples/gateway.lmon` |
| **Python (C-ext)** | [`lmon_c.py`](lmon_c.py) | `loads`, `load`, `dumps`, `dump` _(auto-builds `lmon.c`)_ | `python3 lmon_c.py examples/gateway.lmon` |
| **Ruby** | [`lmon.rb`](lmon.rb) | `LMON.loads`, `LMON.load`, `LMON.dumps` | `ruby lmon.rb examples/gateway.lmon` |
| **JavaScript** | [`lmon.js`](lmon.js) | `LMON.parse`, `LMON.stringify` | `node lmon.js examples/gateway.lmon` |
| **C (C11)** | [`lmon.c`](lmon.c) | Arena DOM (`lmon_parse`, `lmon_to_lmon`, `lmon_to_json`) | `cc -O3 lmon.c -o lmon_c && ./lmon_c examples/gateway.lmon` |
| **Rust** | [`lmon.rs`](lmon.rs) | `parse`, `to_lmon`, `to_json`, `Value` | `rustc -O lmon.rs -o lmon_rs && ./lmon_rs examples/gateway.lmon` |

### CLI Usage

Every implementation supports the same command-line interface:

```sh
# Parse an .lmon file and display formatted JSON on the console
python3 lmon.py examples/gateway.lmon
python3 lmon_c.py examples/gateway.lmon
ruby lmon.rb examples/gateway.lmon
node lmon.js examples/gateway.lmon
./lmon_c examples/gateway.lmon
./lmon_rs examples/gateway.lmon

# Output compact JSON
node lmon.js --compact examples/gateway.lmon

# Format/canonicalize LMON or convert a .json file to LMON
python3 lmon_c.py --lmon examples/gateway.lmon
python3 lmon_c.py --from-json input.json

# Validate syntax only
./lmon_c --check examples/gateway.lmon

# Run built-in conformance tests & parse benchmark
python3 lmon.py --test
python3 lmon_c.py --test && python3 lmon_c.py --bench
ruby lmon.rb --test
node lmon.js --test && node lmon.js --bench
./lmon_c --test && ./lmon_c --bench
./lmon_rs --test && ./lmon_rs --bench
```

### Examples Directory

See [`examples/`](examples/) for sample `.lmon` documents ready to parse and inspect on the console:

- [`examples/hello.lmon`](examples/hello.lmon) — Minimal introduction to LMON types and nesting
- [`examples/gateway.lmon`](examples/gateway.lmon) — Service configuration with upstream pools and quoted HTTP header keys
- [`examples/catalog.lmon`](examples/catalog.lmon) — Product catalog with nested variants, pricing, and inline arrays
- [`examples/matrix.lmon`](examples/matrix.lmon) — Inline and block multidimensional arrays and coordinates
- [`examples/edge_cases.lmon`](examples/edge_cases.lmon) — Unicode escapes, surrogate pairs, empty containers, reserved-word keys, and scientific notation

---

## Benchmarks: LMON vs JSON vs YAML

All benchmarks below were measured on an **Apple M4** (macOS) parsing the **exact same 500-node structured cluster document** (nested objects, strings, integers, floats, booleans, empty containers, and inline arrays):

- **LMON payload:** `114.3 KiB` (`117,066` bytes)
- **JSON payload (2-space formatted):** `138.8 KiB` (`142,089` bytes)
- **YAML payload:** `81.1 KiB` (`83,064` bytes)

Because the three formats encode the same logical document in different byte sizes, **`docs/s` (documents parsed per second)** is the primary end-to-end comparison metric, alongside raw byte throughput (**`MiB/s`**).

| Language & Runtime | LMON Implementation | JSON Implementation | YAML Implementation | LMON vs YAML (`docs/s`) | LMON vs JSON (`docs/s`) |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **C** (`Clang 21 -O3`) | **`5,348 docs/s`** (`597.1 MiB/s`)<br>_`lmon.c` arena DOM_ | `5,145 docs/s` (`697.2 MiB/s`)<br>_C arena DOM_ | `378 docs/s` (`30.0 MiB/s`)<br>_`libyaml 0.2.5` C DOM_ | **14.1x faster** | **1.04x faster** |
| **Rust** (`rustc 1.99.1 -O`) | **`3,902 docs/s`** (`435.6 MiB/s`)<br>_`lmon.rs` (`Cow<str>`)_ | `1,715 docs/s` (`232.3 MiB/s`) _(`serde_json::Value`)_<br>`3,651 docs/s` (`494.7 MiB/s`) _(`lmon.rs` zero-copy JSON)_ | `255 docs/s` (`20.2 MiB/s`)<br>_`yaml-rust2`_ | **15.3x faster** | **2.28x faster** _(vs `serde_json`)_<br>**1.07x faster** _(same DOM)_ |
| **Python + C** (`Python 3.14`) | **`2,107 docs/s`** (`235.3 MiB/s`)<br>_`lmon_c.py` (CPython C API + `lmon.c`)_ | `1,294 docs/s` (`175.3 MiB/s`)<br>_stdlib `json` (native C `_json`)_ | `39.1 docs/s` (`3.1 MiB/s`) _(`PyYAML CSafeLoader` C)_<br>`5.5 docs/s` (`0.44 MiB/s`) _(`PyYAML SafeLoader` Py)_ | **53.9x faster** _(vs C `libyaml`)_<br>**383x faster** _(vs pure Py)_ | **1.63x faster** _(beats stdlib C `json`!)_ |
| **JavaScript** (`Node v26.13`) | **`1,458 docs/s`** (`162.8 MiB/s`)<br>_`lmon.js` (pure JS)_ | `2,717 docs/s` (`368.2 MiB/s`)<br>_V8 `JSON.parse` (native C++)_ | `337 docs/s` (`26.7 MiB/s`)<br>_`js-yaml` (pure JS)_ | **4.3x faster** _(6.1x `MiB/s`)_ | **0.54x** _(pure JS vs native C++)_ |
| **Python** (`Python 3.14`) | **`106 docs/s`** (`11.8 MiB/s`)<br>_`lmon.py` (pure Python)_ | `1,287 docs/s` (`174.4 MiB/s`)<br>_stdlib `json` (native C `_json`)_ | `39.1 docs/s` (`3.1 MiB/s`) _(`PyYAML CSafeLoader` C)_<br>`5.5 docs/s` (`0.44 MiB/s`) _(`PyYAML SafeLoader` Py)_ | **2.7x faster** _(vs C `libyaml`)_<br>**19.2x faster** _(vs pure Py)_ | **0.08x** _(pure Py vs native C)_ |
| **Ruby** (`Ruby 2.6`) | **`65.0 docs/s`** (`7.3 MiB/s`)<br>_`lmon.rb` (pure Ruby)_ | `586 docs/s` (`79.4 MiB/s`)<br>_stdlib `JSON` (native C ext)_ | `32.9 docs/s` (`2.6 MiB/s`)<br>_stdlib `Psych` (native C `libyaml`)_ | **2.0x faster** _(vs C `libyaml`)_ | **0.11x** _(pure Ruby vs native C)_ |

### Key Benchmark Takeaways

1. **Beats Built-In C `json.loads` in Python (`1.63x` Faster):** When backed by `lmon.c` on equal footing with CPython's C `_json` extension, [`lmon_c.py`](lmon_c.py) parses **2,107 docs/s (`235.3 MiB/s`)** vs stdlib `json.loads` at **1,294 docs/s (`175.3 MiB/s`)**—**1.63x faster per document (1.34x in raw MiB/s)** than Python's own built-in JSON parser, and **53.9x faster** than `PyYAML CSafeLoader` (`libyaml`).
2. **Consistently Faster Than YAML Across Every Language:** LMON parses **4.3x to 53.9x faster** than equivalent-runtime YAML parsers. Even the **pure-Python (`lmon.py`)** and **pure-Ruby (`lmon.rb`)** single-file interpreters beat **native C `libyaml`** bindings (`CSafeLoader` and `Psych`) by **2.7x** and **2.0x**, respectively.
3. **Faster Per Document Than JSON in Native Compiled Languages:** In C and Rust, LMON parses **1.04x faster than C JSON**, **1.07x faster than zero-copy Rust JSON**, and **2.28x faster than `serde_json::Value`**—thanks to 4-byte `0x20202020` indentation strides, escape-free unquoted keys, exact pre-sized containers, and ~18% smaller document size.
4. **High Pure-JS Throughput on V8:** Zero-dependency pure-JavaScript `lmon.js` reaches **162.8 MiB/s** (`1,458 docs/s`)—over **half the speed of V8's built-in C++ `JSON.parse`** without native addons, and **4.3x faster per document (6.1x in MiB/s)** than `js-yaml`.

---

## License

Copyright (c) 2026 Lucian Marin. All Rights Reserved. See [LICENSE](LICENSE).

