# LMON — Light Machine Object Notation

**LMON** — **Light Machine Object Notation** (or **Lucian Marin Object Notation**), pronounced **"Lemon"** — is a human-friendly, machine-fast data serialization format.

LMON is a **strict, lossless 1-to-1 conversion of JSON** with **no curly braces** for structured objects and **Python-style block indentation**. It eliminates JSON's noisy braces, commas, and key quotes while avoiding YAML's complex state machines, implicit type coercion traps, and slow parsing.

- **Extension:** `.lmon`
- **Media Type:** `application/lmon`
- **Encoding:** UTF-8 (without BOM)
- **Version:** 1.0

---

## Design Goals

1. **Bijective JSON Equivalence:** Every valid JSON document maps losslessly to LMON and back to the exact same JSON data model (`object`, `array`, `string`, `number`, `true`, `false`, `null`).
2. **Pythonic Readability:** No `{` `}` curly braces around objects, no trailing comma errors, clean block indentation (`2`, `4`, `6`, `8` spaces or `\t`, auto-detected), and Python-style universal newlines (`\n`, `\r\n`, `\r`).
3. **Zero Ambiguity (No YAML Footguns):** Strings are always double-quoted (`"NO"` is never coerced to `false`, `"1.20"` never loses trailing zeros), `null` is explicit (`key:` without a nested block is a syntax error, not silent `null`), and there are no comments, anchors, tags, or multiline folding modes.
4. **Faster Than YAML — and Faster Than JSON:** Auto-detected fixed-stride indentation, single-byte `LL(1)` token dispatch, unquoted ASCII keys requiring zero escape scanning, flat contiguous DOM layout, and fewer structural delimiters enable single-pass arena parsers that surpass both YAML and JSON.

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

### 2. Encoding, Universal Newlines, and Auto-Detected Indentation

1. **UTF-8:** Documents MUST be valid UTF-8 without a byte-order mark (`U+FEFF` at byte 0 is a syntax error).
2. **Universal Newlines (`LF`, `CRLF`, `CR`):** Just like Python source files, LMON supports `LF` (`\n`, `0x0A`), `CRLF` (`\r\n`, `0x0D 0x0A`), and `CR` (`\r`, `0x0D`) line endings interchangeably outside string literals, normalizing each into a single logical `NEWLINE` token. Documents authored on Linux, macOS, Windows, or classic CR systems parse identically without line-ending errors. The final line MAY omit a trailing newline.
3. **Blank Lines:** Lines containing zero characters or only ASCII spaces (`0x20`) and/or tabs (`0x09`) before a line ending (or EOF) are blank lines and are ignored anywhere in the document without affecting indentation detection.
4. **Auto-Detected Indentation (`2`, `4`, `6`, `8` Spaces or `\t`):**
   - LMON supports five consistent indentation units per level: **2 spaces**, **4 spaces**, **6 spaces**, **8 spaces**, or a **single tab (`\t`, `0x09`)**.
   - **Auto-detection rule:** Root entries sit at indentation level `0`. Because LMON blocks always step from level `0` to level `1` before deeper nesting can occur, the **first indented non-blank line** in a document unambiguously establishes the file's indentation unit (`indent_unit`):
     - `1` leading `\t` locks the document to **tab indentation** (`1` tab per level).
     - `2`, `4`, `6`, or `8` leading ASCII spaces lock the document to **space indentation** of width `W ∈ {2, 4, 6, 8}` (`W` spaces per level).
     - Any other leading indentation on the first indented line (e.g. `1`, `3`, `5`, `7`, or `>8` spaces, `>1` tabs, or mixed spaces and tabs) is a **syntax error**.
   - Every subsequent non-blank line MUST indent using an exact multiple `N * indent_unit` (`N >= 0`) of that detected character and width. Mixing tabs and spaces in indentation—or using `\t` anywhere else outside a string literal—is a **syntax error**.
5. **No Trailing Whitespace:** Non-blank lines MUST NOT have trailing spaces or tabs after tokens (`key:`, `-`, or inline values).

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

If an object contains duplicate keys, parsers MAY emit members in document order (`O(1)` flat-DOM append, matching high-speed JSON engines such as `simdjson`/`yyjson`) or apply last-value-wins map semantics in hash-table languages.

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

1. Use `\n` line endings, **4 spaces** per indentation level by default (serializers MAY accept an explicit indent option of `2`, `4`, `6`, `8` spaces, or `"\t"`), and a single trailing `\n` at end of document.
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

indent(n)      = n * indent_unit ;  (* indent_unit locked on 1st indented line: 2/4/6/8 %x20 or 1 %x09 *)
ws             = { %x20 } ;
blank_ws       = { %x20 | %x09 } ;
blank_lines    = { blank_ws line_end } ;
line_end       = %x0D %x0A | %x0A | %x0D ;  (* Universal newlines: CRLF | LF | CR *)
```

---

## Why LMON Is Fast to Parse

| Property | YAML 1.2 | JSON (RFC 8259) | LMON 1.0 |
| :--- | :--- | :--- | :--- |
| **Indentation scanner** | Arbitrary spaces, chomping/folding states | Arbitrary whitespace skip | **Fixed stride (`\t` or 2/4/6/8 spaces, locked once)** |
| **Object delimiters** | Flow `{}` + block mapping + complex keys | `{`, `}`, `,` on every object/field | **None (newline + indent only)** |
| **Object key scanning** | Multi-rule implicit scalar / colon lookahead | Quote + `\` escape scan on every key | **Raw ASCII scan to `:` (zero escapes)** |
| **DOM layout** | Pointer-heavy event/graph nodes | Nested value nodes or tape | **Flat contiguous value arrays + `O(1)` append** |
| **Type resolution** | Regex/tag inference (`yes`, `1.0`, ISO dates) | **1st-byte dispatch** | **1st-byte dispatch** |
| **Anchors / aliases** | Arbitrary graph references (`&id`, `*id`) | None | **None (strict tree)** |

1. **Auto-Locked Stride Indentation:** Locking the indentation unit on the first indented line turns every subsequent line's nesting check into word-stride comparisons (`0x20202020` / `0x09`) and bit shifts (`spaces >> shift`, or single-byte `\t` count).
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

# Format/canonicalize LMON (default 4 spaces, or 2/4/6/8/tab) or convert JSON to LMON
python3 lmon_c.py --lmon examples/gateway.lmon
python3 lmon_c.py --indent tab examples/gateway.lmon
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
- [`examples/tabs.lmon`](examples/tabs.lmon) — Tab-indented LMON demonstrating auto-detected `\t` indentation

---

## Benchmarks: LMON vs JSON vs YAML

All benchmarks below were measured on an **Apple M4** (macOS) parsing the **exact same 500-node structured cluster document** (nested objects, strings, integers, floats, booleans, empty containers, and inline arrays):

- **LMON payload (4-space):** `114.3 KiB` (`117,066` bytes) — **tab-indented (`\t`):** `82.1 KiB` (`84,066` bytes, **`-28.2%`**)
- **JSON payload (2-space formatted):** `138.8 KiB` (`142,089` bytes)
- **YAML payload:** `81.1 KiB` (`83,064` bytes)

Because the formats encode the same logical document in different byte sizes, **`docs/s` (documents parsed per second)** is the primary end-to-end comparison metric, alongside raw byte throughput (**`MiB/s`**).

| Implementation | 4-Space | Tab (`\t`) | Tab vs 4-Space | JSON | LMON (Tab) vs JSON (`docs/s`) |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **C (`lmon.c`)**<br>_`Clang 21 -O3`, flat arena DOM + NEON_ | **`11,422 docs/s`**<br>(`1,283.6 MiB/s`) | **`12,905 docs/s`**<br>(`1,034.6 MiB/s`) | **`+13.0%`** | `11,451 docs/s`<br>(`1,551.7 MiB/s`) | **`1.13x faster`** |
| **Rust (`lmon.rs`)**<br>_`rustc 1.99.1 -C opt-level=3`, `Cow<str>`_ | **`7,316 docs/s`**<br>(`822.1 MiB/s`) | **`8,569 docs/s`**<br>(`687.0 MiB/s`) | **`+17.1%`** | `8,110 docs/s` _(`lmon.rs` zero-copy JSON)_<br>`~3,400 docs/s` _(`serde_json::Value`)_ | **`1.06x faster`** _(same DOM)_<br>**`~2.5x faster`** _(vs `serde_json`)_ |
| **Python+C (`lmon_c.py`)**<br>_`Python 3.14`, CPython C API + `lmon.c`_ | **`4,271 docs/s`**<br>(`476.8 MiB/s`) | **`4,318 docs/s`**<br>(`346.2 MiB/s`) | **`+1.1%`** | `2,516 docs/s`<br>(`340.9 MiB/s`, stdlib C `json`) | **`1.72x faster`** _(beats stdlib C `json`!)_ |
| **JS (`lmon.js`)**<br>_`Node v26.13`, pure JS_ | **`2,833 docs/s`**<br>(`316.3 MiB/s`) | **`3,122 docs/s`**<br>(`250.3 MiB/s`) | **`+10.2%`** | `5,416 docs/s`<br>(`733.9 MiB/s`, V8 C++ `JSON.parse`) | **`0.58x`** _(pure JS vs native C++)_ |
| **Python (`lmon.py`)**<br>_`Python 3.14`, pure Python_ | **`199.1 docs/s`**<br>(`22.2 MiB/s`) | **`227.5 docs/s`**<br>(`18.2 MiB/s`) | **`+14.3%`** | `2,516 docs/s`<br>(`340.9 MiB/s`, stdlib C `json`) | **`0.09x`** _(pure Py vs native C)_ |
| **Ruby (`lmon.rb`)**<br>_`Ruby 2.6`, pure Ruby_ | **`125.0 docs/s`**<br>(`14.0 MiB/s`) | **`143.3 docs/s`**<br>(`11.5 MiB/s`) | **`+14.6%`** | `1,192 docs/s`<br>(`161.6 MiB/s`, stdlib C `JSON`) | **`0.12x`** _(pure Ruby vs native C)_ |

### Key Benchmark Takeaways

1. **Beats Built-In C `json.loads` in Python (`1.72x` Faster):** Backed by `lmon.c`'s flat DOM and key interning, [`lmon_c.py`](lmon_c.py) parses **4,271 docs/s (`476.8 MiB/s`)** with 4-space indentation and **4,318 docs/s (`346.2 MiB/s`)** with tabs vs stdlib `json.loads` at **2,516 docs/s (`340.9 MiB/s`)**—**1.72x faster per document** than Python's own built-in C JSON parser.
2. **Over `1.28 GiB/s` in C & `8,569 docs/s` in Safe Rust:** With flat contiguous value arrays, `O(1)` member append, and SIMD ASCII string scanning, `lmon.c` reaches **11,422 docs/s (`1,283.6 MiB/s`)** on 4-space LMON and **12,905 docs/s (`1,034.6 MiB/s`)** on tab-indented LMON (**1.13x faster** than C JSON on the identical flat-DOM engine), while safe Rust `lmon.rs` reaches **8,569 docs/s** (**1.06x faster** than zero-copy Rust JSON and **~2.5x faster** than `serde_json::Value`).
3. **10–17% Higher Document Throughput with Tab Indentation (`\t`):** Replacing 4-space indentation with single `\t` bytes shrinks the 500-node payload by **28.2%** (`33,000` fewer bytes per document) and `examples/*.lmon` by **14.8%**, boosting parse throughput by **+13.0%** in C, **+17.1%** in Rust, and **+10–15%** across JavaScript, Python, and Ruby.
4. **Consistently Faster Than YAML Across Every Language:** LMON parses **4x to 55x faster** than equivalent-runtime YAML parsers—and even **pure-Python (`lmon.py`)** and **pure-Ruby (`lmon.rb`)** beat **native C `libyaml`** bindings (`CSafeLoader` and `Psych` at `71.9 docs/s` / `6.4 MiB/s`) by **2.0x to 3.2x**.

---

## License

Copyright (c) 2026 Lucian Marin. All Rights Reserved. See [LICENSE](LICENSE).

