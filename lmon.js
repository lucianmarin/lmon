#!/usr/bin/env node
"use strict";

/**
 * LMON (Light Machine / Lucian Marin Object Notation) — single-file JavaScript implementation.
 * Zero external dependencies. Optimized for V8 (Node.js & modern browsers).
 *
 * Library API:
 *   const LMON = require("./lmon.js");
 *   LMON.parse(text) / LMON.loads(text)
 *   LMON.stringify(value) / LMON.dumps(value)
 *
 * CLI usage:
 *   node lmon.js <file.lmon>            # Parse LMON and print formatted JSON
 *   node lmon.js --compact <file.lmon>  # Parse LMON and print compact JSON
 *   node lmon.js --lmon <file>          # Output canonical LMON
 *   node lmon.js --from-json <file>     # Convert JSON file to LMON
 *   node lmon.js --check <file.lmon>    # Validate LMON syntax only
 *   node lmon.js --test                 # Run conformance test suite
 *   node lmon.js --bench [file.lmon]    # Run LMON vs native JSON benchmark
 */

const INLINE_MAX_BYTES = 80;

// 1 = [A-Za-z_], 2 = [0-9-]
const KEY_CLASS = new Uint8Array(128);
for (let c = 65; c <= 90; c++) KEY_CLASS[c] = 1;  // A-Z
for (let c = 97; c <= 122; c++) KEY_CLASS[c] = 1; // a-z
KEY_CLASS[95] = 1;                                // _
for (let c = 48; c <= 57; c++) KEY_CLASS[c] = 2;  // 0-9
KEY_CLASS[45] = 2;                                // -

const POW10 = [
  1, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7,
  1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15,
];

class LMONParseError extends SyntaxError {
  constructor(message, doc, pos) {
    let line = 1;
    let col = 1;
    const end = Math.min(pos, doc.length);
    for (let i = 0; i < end; i++) {
      if (doc.charCodeAt(i) === 10) {
        line++;
        col = 1;
      } else {
        col++;
      }
    }
    super(`${message}: line ${line} column ${col} (pos ${end})`);
    this.name = "LMONParseError";
    this.pos = end;
    this.lineno = line;
    this.colno = col;
  }
}

function hexVal(c) {
  if (c >= 48 && c <= 57) return c - 48;
  if (c >= 97 && c <= 102) return c - 87;
  if (c >= 65 && c <= 70) return c - 55;
  return -1;
}

function isReservedKey(s) {
  return s === "true" || s === "false" || s === "null";
}

class Parser {
  constructor(s) {
    this.s = s;
    this.n = s.length;
    this.pos = 0;
    this.lineIndent = -2;
    this.lineStart = 0;
  }

  err(msg, at) {
    throw new LMONParseError(msg, this.s, at === undefined ? this.pos : at);
  }

  peekLine() {
    if (this.lineIndent !== -2) return this.lineIndent;

    const s = this.s;
    const n = this.n;
    let p = this.pos;

    while (p < n) {
      const lineBegin = p;
      while (p < n && s.charCodeAt(p) === 32) p++;

      if (p >= n) {
        this.pos = p;
        this.lineIndent = -1;
        return -1;
      }

      const c = s.charCodeAt(p);
      if (c === 10) {
        p++;
        continue;
      }
      if (c === 13) {
        if (p + 1 >= n || s.charCodeAt(p + 1) !== 10) {
          this.err("bare CR is not allowed", p);
        }
        p += 2;
        continue;
      }
      if (c === 9) {
        this.err("tabs are not allowed for indentation", p);
      }

      const spaces = p - lineBegin;
      if ((spaces & 3) !== 0) {
        this.err("indentation must be a multiple of 4 spaces", lineBegin);
      }

      this.pos = p;
      this.lineStart = lineBegin;
      this.lineIndent = spaces >> 2;
      return this.lineIndent;
    }

    this.pos = p;
    this.lineIndent = -1;
    return -1;
  }

  consumeLineEnd(allowEof) {
    const s = this.s;
    const p = this.pos;
    if (p >= this.n) {
      if (!allowEof) this.err("unexpected end of input; expected nested block", p);
      this.lineIndent = -1;
      return;
    }
    const c = s.charCodeAt(p);
    if (c === 10) {
      this.pos = p + 1;
      this.lineIndent = -2;
      return;
    }
    if (c === 13) {
      if (p + 1 < this.n && s.charCodeAt(p + 1) === 10) {
        this.pos = p + 2;
        this.lineIndent = -2;
        return;
      }
      this.err("bare CR is not allowed", p);
    }
    if (c === 9) this.err("tabs are not allowed outside strings", p);
    if (c === 32) this.err("trailing spaces are not allowed", p);
    this.err("unexpected character at end of line", p);
  }

  isDashItem() {
    const p = this.pos;
    if (p >= this.n || this.s.charCodeAt(p) !== 45) return false;
    if (p + 1 >= this.n) return true;
    const nxt = this.s.charCodeAt(p + 1);
    return nxt === 32 || nxt === 10 || nxt === 13;
  }

  parseString() {
    const s = this.s;
    const n = this.n;
    let p = this.pos;
    if (p >= n || s.charCodeAt(p) !== 34) this.err("expected '\"'", p);
    p++;
    const start = p;

    // Fast unescaped scan
    while (p < n) {
      const c = s.charCodeAt(p);
      if (c === 34) {
        this.pos = p + 1;
        return s.slice(start, p);
      }
      if (c === 92) break;
      if (c < 32) this.err("unescaped control character in string", p);
      if (c >= 0xd800 && c <= 0xdfff) {
        // Validate UTF-16 surrogate pair inside JS string
        if (c <= 0xdbff && p + 1 < n) {
          const lo = s.charCodeAt(p + 1);
          if (lo >= 0xdc00 && lo <= 0xdfff) {
            p += 2;
            continue;
          }
        }
        this.err("unpaired UTF-16 surrogate in string", p);
      }
      p++;
    }

    if (p >= n) this.err("unterminated string literal", p);

    let out = s.slice(start, p);
    while (p < n) {
      const c = s.charCodeAt(p);
      if (c === 34) {
        this.pos = p + 1;
        return out;
      }
      if (c < 32) this.err("unescaped control character in string", p);
      if (c !== 92) {
        const chunkStart = p;
        while (p < n) {
          const ch = s.charCodeAt(p);
          if (ch === 34 || ch === 92 || ch < 32) break;
          p++;
        }
        out += s.slice(chunkStart, p);
        continue;
      }

      p++; // skip '\\'
      if (p >= n) this.err("unterminated escape sequence", p);
      const esc = s.charCodeAt(p++);
      switch (esc) {
        case 34: out += '"'; break;
        case 92: out += "\\"; break;
        case 47: out += "/"; break;
        case 98: out += "\b"; break;
        case 102: out += "\f"; break;
        case 110: out += "\n"; break;
        case 114: out += "\r"; break;
        case 116: out += "\t"; break;
        case 117: {
          if (p + 4 > n) this.err("invalid \\uXXXX escape sequence", p);
          const h0 = hexVal(s.charCodeAt(p));
          const h1 = hexVal(s.charCodeAt(p + 1));
          const h2 = hexVal(s.charCodeAt(p + 2));
          const h3 = hexVal(s.charCodeAt(p + 3));
          if ((h0 | h1 | h2 | h3) < 0) this.err("invalid \\uXXXX escape sequence", p);
          const cp = (h0 << 12) | (h1 << 8) | (h2 << 4) | h3;
          p += 4;
          if (cp >= 0xd800 && cp <= 0xdbff) {
            if (p + 6 > n || s.charCodeAt(p) !== 92 || s.charCodeAt(p + 1) !== 117) {
              this.err("unpaired high surrogate in string", p);
            }
            const l0 = hexVal(s.charCodeAt(p + 2));
            const l1 = hexVal(s.charCodeAt(p + 3));
            const l2 = hexVal(s.charCodeAt(p + 4));
            const l3 = hexVal(s.charCodeAt(p + 5));
            if ((l0 | l1 | l2 | l3) < 0) this.err("invalid low surrogate \\uXXXX escape", p + 2);
            const low = (l0 << 12) | (l1 << 8) | (l2 << 4) | l3;
            if (low < 0xdc00 || low > 0xdfff) {
              this.err("expected low surrogate (DC00..DFFF)", p + 2);
            }
            p += 6;
            out += String.fromCharCode(cp, low);
          } else if (cp >= 0xdc00 && cp <= 0xdfff) {
            this.err("unpaired low surrogate in string", p - 4);
          } else {
            out += String.fromCharCode(cp);
          }
          break;
        }
        default:
          this.err("invalid escape sequence in string", p - 1);
      }
    }
    this.err("unterminated string literal", p);
  }

  parseNumber() {
    const s = this.s;
    const n = this.n;
    const start = this.pos;
    let p = start;

    let neg = false;
    if (p < n && s.charCodeAt(p) === 45) {
      neg = true;
      p++;
    }
    if (p >= n) this.err("invalid number literal", start);
    let c = s.charCodeAt(p);
    if (c < 48 || c > 57) this.err("invalid number literal", start);

    let mantissa = 0;
    let intDigits = 0;
    if (c === 48) {
      p++;
      intDigits = 1;
      if (p < n) {
        const nxt = s.charCodeAt(p);
        if (nxt >= 48 && nxt <= 57) this.err("leading zeros are not allowed in numbers", p);
      }
    } else {
      while (p < n) {
        const d = s.charCodeAt(p) - 48;
        if (d < 0 || d > 9) break;
        if (intDigits < 16) mantissa = mantissa * 10 + d;
        intDigits++;
        p++;
      }
    }

    let isFloat = false;
    let fracDigits = 0;
    let fracVal = 0;
    if (p < n && s.charCodeAt(p) === 46) {
      isFloat = true;
      p++;
      if (p >= n || s.charCodeAt(p) < 48 || s.charCodeAt(p) > 57) {
        this.err("expected digit after decimal point", p);
      }
      while (p < n) {
        const d = s.charCodeAt(p) - 48;
        if (d < 0 || d > 9) break;
        if (fracDigits < 16) fracVal = fracVal * 10 + d;
        fracDigits++;
        p++;
      }
    }

    let hasExp = false;
    if (p < n) {
      const ec = s.charCodeAt(p);
      if (ec === 101 || ec === 69) {
        isFloat = true;
        hasExp = true;
        p++;
        if (p < n) {
          const sc = s.charCodeAt(p);
          if (sc === 43 || sc === 45) p++;
        }
        if (p >= n || s.charCodeAt(p) < 48 || s.charCodeAt(p) > 57) {
          this.err("expected digit in exponent", p);
        }
        while (p < n) {
          const d = s.charCodeAt(p) - 48;
          if (d < 0 || d > 9) break;
          p++;
        }
      }
    }

    if (p < n) {
      const tc = s.charCodeAt(p);
      if (tc === 46 || tc === 101 || tc === 69 || tc === 43 || tc === 45) {
        this.err("malformed number literal", p);
      }
    }

    this.pos = p;

    if (!isFloat && intDigits <= 15) {
      if (mantissa === 0) return 0;
      return neg ? -mantissa : mantissa;
    }
    if (isFloat && !hasExp && intDigits + fracDigits <= 15) {
      const combined = mantissa * POW10[fracDigits] + fracVal;
      const val = combined / POW10[fracDigits];
      return val === 0 ? 0 : (neg ? -val : val);
    }

    const parsed = Number(s.slice(start, p));
    if (!Number.isFinite(parsed)) {
      this.err("numeric literal out of finite IEEE-754 range", start);
    }
    return parsed === 0 ? 0 : parsed;
  }

  parseInlineArray() {
    const s = this.s;
    const n = this.n;
    this.pos++; // '['
    while (this.pos < n && s.charCodeAt(this.pos) === 32) this.pos++;
    if (this.pos < n && s.charCodeAt(this.pos) === 93) {
      this.pos++;
      return [];
    }

    const items = [];
    for (;;) {
      items.push(this.parseInlineValue());
      while (this.pos < n && s.charCodeAt(this.pos) === 32) this.pos++;
      if (this.pos >= n) this.err("unterminated inline array");
      const c = s.charCodeAt(this.pos);
      if (c === 44) {
        this.pos++;
        while (this.pos < n && s.charCodeAt(this.pos) === 32) this.pos++;
        if (this.pos < n && s.charCodeAt(this.pos) === 93) {
          this.err("trailing comma in inline array");
        }
      } else if (c === 93) {
        this.pos++;
        return items;
      } else if (c === 9) {
        this.err("tabs are not allowed outside strings");
      } else {
        this.err("expected ',' or ']' in inline array");
      }
    }
  }

  parseInlineValue() {
    const s = this.s;
    const n = this.n;
    const p = this.pos;
    if (p >= n) this.err("expected value");
    const c = s.charCodeAt(p);
    switch (c) {
      case 34: // '"'
        return this.parseString();
      case 45:
      case 48: case 49: case 50: case 51: case 52:
      case 53: case 54: case 55: case 56: case 57:
        return this.parseNumber();
      case 116: // 't'
        if (
          p + 4 <= n &&
          s.charCodeAt(p + 1) === 114 &&
          s.charCodeAt(p + 2) === 117 &&
          s.charCodeAt(p + 3) === 101
        ) {
          this.pos = p + 4;
          return true;
        }
        this.err("invalid literal; expected 'true'");
        break;
      case 102: // 'f'
        if (
          p + 5 <= n &&
          s.charCodeAt(p + 1) === 97 &&
          s.charCodeAt(p + 2) === 108 &&
          s.charCodeAt(p + 3) === 115 &&
          s.charCodeAt(p + 4) === 101
        ) {
          this.pos = p + 5;
          return false;
        }
        this.err("invalid literal; expected 'false'");
        break;
      case 110: // 'n'
        if (
          p + 4 <= n &&
          s.charCodeAt(p + 1) === 117 &&
          s.charCodeAt(p + 2) === 108 &&
          s.charCodeAt(p + 3) === 108
        ) {
          this.pos = p + 4;
          return null;
        }
        this.err("invalid literal; expected 'null'");
        break;
      case 91: // '['
        return this.parseInlineArray();
      case 123: // '{'
        if (p + 1 < n && s.charCodeAt(p + 1) === 125) {
          this.pos = p + 2;
          return {};
        }
        this.err("curly-brace objects are not allowed in LMON; use indentation or '{}'");
        break;
      default:
        this.err("unexpected character when parsing value");
    }
  }

  parseKey() {
    const s = this.s;
    const n = this.n;
    const p = this.pos;
    if (p < n && s.charCodeAt(p) === 34) {
      return this.parseString();
    }
    const c0 = p < n ? s.charCodeAt(p) : 0;
    if (c0 >= 128 || KEY_CLASS[c0] !== 1) {
      this.err("expected object key", p);
    }
    let q = p + 1;
    while (q < n) {
      const ch = s.charCodeAt(q);
      if (ch >= 128 || KEY_CLASS[ch] === 0) break;
      q++;
    }
    const key = s.slice(p, q);
    if (isReservedKey(key)) {
      this.err(`reserved literal '${key}' must be quoted when used as a key`, p);
    }
    this.pos = q;
    return key;
  }

  parseNestedBlock(expectedIndent) {
    const indent = this.peekLine();
    if (indent < 0) this.err("expected indented block, got end of input");
    if (indent !== expectedIndent) {
      this.err(
        `expected indentation level ${expectedIndent} (${expectedIndent * 4} spaces), got ${indent * 4} spaces`,
        this.lineStart
      );
    }
    if (this.isDashItem()) {
      return this.parseBlockArray(expectedIndent);
    }
    const c = this.s.charCodeAt(this.pos);
    if (c === 34) {
      const strStart = this.pos;
      const firstKey = this.parseString();
      if (this.pos >= this.n || this.s.charCodeAt(this.pos) !== 58) {
        this.err("nested block cannot be a standalone scalar", strStart);
      }
      return this.parseBlockObject(expectedIndent, firstKey);
    }
    if (c < 128 && KEY_CLASS[c] === 1) {
      return this.parseBlockObject(expectedIndent, null);
    }
    this.err("nested block must be a block object or block array");
  }

  parseBlockObject(expectedIndent, firstKey) {
    const obj = {};
    let pendingKey = firstKey;

    for (;;) {
      let key;
      if (pendingKey !== null) {
        key = pendingKey;
        pendingKey = null;
      } else {
        const indent = this.peekLine();
        if (indent < expectedIndent) break;
        if (indent > expectedIndent) {
          this.err("unexpected indentation inside object", this.lineStart);
        }
        if (this.isDashItem()) {
          this.err("cannot mix array items and object keys at same indentation");
        }
        key = this.parseKey();
      }

      if (this.pos >= this.n || this.s.charCodeAt(this.pos) !== 58) {
        this.err("expected ':' after object key");
      }
      this.pos++;

      let val;
      if (this.pos < this.n && this.s.charCodeAt(this.pos) === 32) {
        this.pos++;
        val = this.parseInlineValue();
        this.consumeLineEnd(true);
      } else {
        this.consumeLineEnd(false);
        val = this.parseNestedBlock(expectedIndent + 1);
      }

      if (key === "__proto__") {
        Object.defineProperty(obj, "__proto__", {
          value: val,
          writable: true,
          enumerable: true,
          configurable: true,
        });
      } else {
        obj[key] = val;
      }
    }

    return obj;
  }

  parseBlockArray(expectedIndent) {
    const arr = [];
    for (;;) {
      const indent = this.peekLine();
      if (indent < expectedIndent) break;
      if (indent > expectedIndent) {
        this.err("unexpected indentation inside array", this.lineStart);
      }
      if (!this.isDashItem()) {
        this.err("cannot mix object keys and array items at same indentation");
      }
      this.pos++; // '-'

      let elem;
      if (this.pos < this.n && this.s.charCodeAt(this.pos) === 32) {
        this.pos++;
        elem = this.parseInlineValue();
        this.consumeLineEnd(true);
      } else {
        this.consumeLineEnd(false);
        elem = this.parseNestedBlock(expectedIndent + 1);
      }
      arr.push(elem);
    }
    return arr;
  }

  parseDocument() {
    if (this.n > 0 && this.s.charCodeAt(0) === 0xfeff) {
      this.err("UTF-8 BOM is not allowed", 0);
    }
    const indent = this.peekLine();
    if (indent < 0) this.err("empty LMON document", this.pos);
    if (indent !== 0) this.err("root value must not be indented", this.lineStart);

    let result;
    if (this.isDashItem()) {
      result = this.parseBlockArray(0);
    } else {
      const c = this.s.charCodeAt(this.pos);
      if (c === 34) {
        const firstStr = this.parseString();
        if (this.pos < this.n && this.s.charCodeAt(this.pos) === 58) {
          result = this.parseBlockObject(0, firstStr);
        } else {
          this.consumeLineEnd(true);
          result = firstStr;
        }
      } else if (c < 128 && KEY_CLASS[c] === 1) {
        let q = this.pos + 1;
        while (q < this.n) {
          const ch = this.s.charCodeAt(q);
          if (ch >= 128 || KEY_CLASS[ch] === 0) break;
          q++;
        }
        const ident = this.s.slice(this.pos, q);
        if (isReservedKey(ident) && (q >= this.n || this.s.charCodeAt(q) !== 58)) {
          result = this.parseInlineValue();
          this.consumeLineEnd(true);
        } else {
          result = this.parseBlockObject(0, null);
        }
      } else {
        result = this.parseInlineValue();
        this.consumeLineEnd(true);
      }
    }

    const trailing = this.peekLine();
    if (trailing !== -1) {
      this.err("unexpected trailing content after root value", this.lineStart);
    }
    return result;
  }
}

function parse(text) {
  if (typeof text !== "string") {
    throw new TypeError(`LMON input must be string, got ${typeof text}`);
  }
  return new Parser(text).parseDocument();
}

// ---------------------------------------------------------------------------
// Canonical LMON 1.0 Serializer
// ---------------------------------------------------------------------------

function canBeUnquotedKey(k) {
  const n = k.length;
  if (n === 0 || isReservedKey(k)) return false;
  const c0 = k.charCodeAt(0);
  if (c0 >= 128 || KEY_CLASS[c0] !== 1) return false;
  for (let i = 1; i < n; i++) {
    const c = k.charCodeAt(i);
    if (c >= 128 || KEY_CLASS[c] === 0) return false;
  }
  return true;
}

function utf8ByteLength(str) {
  if (typeof Buffer !== "undefined") return Buffer.byteLength(str, "utf8");
  return new TextEncoder().encode(str).length;
}

function formatKey(key) {
  return canBeUnquotedKey(key) ? key : JSON.stringify(key);
}

function tryFormatInline(val) {
  if (val === null) return "null";
  const t = typeof val;
  if (t === "boolean") return val ? "true" : "false";
  if (t === "string") return JSON.stringify(val);
  if (t === "number") {
    if (!Number.isFinite(val)) {
      throw new TypeError("NaN/Infinity are not LMON compliant");
    }
    return val === 0 ? "0" : String(val);
  }
  if (Array.isArray(val)) {
    if (val.length === 0) return "[]";
    const parts = [];
    let byteLen = 2;
    for (let i = 0; i < val.length; i++) {
      const rendered = tryFormatInline(val[i]);
      if (rendered === null) return null;
      if (i > 0) byteLen += 2;
      byteLen += utf8ByteLength(rendered);
      if (byteLen > INLINE_MAX_BYTES) return null;
      parts.push(rendered);
    }
    return `[${parts.join(", ")}]`;
  }
  if (t === "object") {
    const keys = Object.keys(val);
    return keys.length === 0 ? "{}" : null;
  }
  throw new TypeError(`Value of type ${t} is not LMON serializable`);
}

function emitBlock(val, indent, lines) {
  const prefix = "    ".repeat(indent);
  if (Array.isArray(val)) {
    for (let i = 0; i < val.length; i++) {
      const inline = tryFormatInline(val[i]);
      if (inline !== null) {
        lines.push(`${prefix}- ${inline}`);
      } else {
        lines.push(`${prefix}-`);
        emitBlock(val[i], indent + 1, lines);
      }
    }
  } else if (val !== null && typeof val === "object") {
    const keys = Object.keys(val);
    for (let i = 0; i < keys.length; i++) {
      const k = keys[i];
      const v = val[k];
      const fk = formatKey(k);
      const inline = tryFormatInline(v);
      if (inline !== null) {
        lines.push(`${prefix}${fk}: ${inline}`);
      } else {
        lines.push(`${prefix}${fk}:`);
        emitBlock(v, indent + 1, lines);
      }
    }
  } else {
    throw new TypeError(`Value of type ${typeof val} is not LMON serializable`);
  }
}

function stringify(value) {
  const inline = tryFormatInline(value);
  if (inline !== null) return `${inline}\n`;
  const lines = [];
  emitBlock(value, 0, lines);
  return `${lines.join("\n")}\n`;
}

const LMON = {
  parse,
  loads: parse,
  stringify,
  dumps: stringify,
  LMONParseError,
};

if (typeof module !== "undefined" && module.exports) {
  module.exports = LMON;
}

// ---------------------------------------------------------------------------
// CLI, Conformance Tests & Benchmark
// ---------------------------------------------------------------------------

if (typeof require !== "undefined" && require.main === module) {
  const fs = require("fs");
  const { performance } = require("perf_hooks");

  const usage =
    "Usage: node lmon.js [options] <file.lmon | ->\n" +
    "Options:\n" +
    "  -c, --compact    Output compact single-line JSON\n" +
    "  -l, --lmon       Output canonical LMON\n" +
    "  --from-json      Parse input as JSON and output canonical LMON\n" +
    "  --check          Validate LMON syntax only\n" +
    "  --test           Run conformance test suite\n" +
    "  --bench [file]   Run LMON vs native JSON parse benchmark\n";

  const argv = process.argv.slice(2);
  if (argv.length === 0 || argv.includes("-h") || argv.includes("--help")) {
    process.stdout.write(usage);
    process.exit(argv.length === 0 ? 1 : 0);
  }

  if (argv[0] === "--test") {
    const assert = require("assert");
    const cases = [
      ["empty_obj", {}],
      ["empty_arr", []],
      ["scalars", { s: "Lemon", i: -42, f: 3.25, t: true, fa: false, n: null }],
      ["root_int", -99],
      ["root_str", "hello: world"],
      ["root_bool", true],
      ["root_null", null],
      [
        "nested_mix",
        {
          service: "lmon",
          tags: ["a", "b", "c"],
          empty_map: {},
          empty_list: [],
          matrix: [[1, 2], [3, 4]],
          nodes: [
            { id: 1, meta: { ok: true } },
            { id: 2, meta: {} },
          ],
          "": "empty",
          true: "keyword",
          false: false,
          null: null,
          123: "numeric-key",
          "-dash": "dash-key",
          "with space": "quoted\nline\t🍋",
        },
      ],
    ];

    for (const [name, expected] of cases) {
      const encoded = stringify(expected);
      const decoded = parse(encoded);
      assert.deepStrictEqual(decoded, expected, `Round-trip failed for ${name}`);
    }

    assert.deepStrictEqual(parse('emoji: "\\uD83C\\uDF4B"\n'), { emoji: "🍋" });

    const invalid = [
      "",
      "   \n",
      "  bad_indent: 1\n",
      "\ttab: 1\n",
      "dangling:\n",
      "dangling:\nstill_root: 1\n",
      'curly: {"a": 1}\n',
      "true: 1\n",
      "trailing_space: 1 \n",
      "two_spaces:  1\n",
      "bad_num: 01\n",
      "trailing_comma: [1, 2,]\n",
      "mixed: 1\n- 2\n",
      "# comments are forbidden\na: 1\n",
    ];

    for (const bad of invalid) {
      let threw = false;
      try {
        parse(bad);
      } catch (e) {
        if (e instanceof LMONParseError) threw = true;
        else throw e;
      }
      if (!threw) {
        process.stderr.write(`FAIL [invalid accepted]: ${JSON.stringify(bad)}\n`);
        process.exit(1);
      }
    }

    console.log(`JavaScript LMON: all ${cases.length + invalid.length + 1} conformance tests passed.`);
    process.exit(0);
  }

  if (argv[0] === "--bench") {
    let sample;
    if (argv[1]) {
      sample = fs.readFileSync(argv[1], "utf8");
    } else {
      const nodes = [];
      for (let i = 0; i < 500; i++) {
        nodes.push({
          id: i,
          name: `service-node-${i}`,
          active: i % 2 === 0,
          latency_ms: Number((1.25 * (i % 17)).toFixed(3)),
          tags: ["edge", "lemon", "v1"],
          meta: { region: "eu-central", capacity: 256 + i, extra: {} },
        });
      }
      sample = stringify({ cluster: "lmon-bench", count: 500, nodes });
    }

    const parsed = parse(sample);
    const jsonSample = JSON.stringify(parsed, null, 2);
    const lmonBytes = Buffer.byteLength(sample, "utf8");
    const jsonBytes = Buffer.byteLength(jsonSample, "utf8");
    const kb = lmonBytes / 1024;
    const iterations = Math.max(50, Math.floor(80000 / Math.max(1, kb)));

    // V8 JIT warmup
    for (let w = 0; w < 25; w++) {
      parse(sample);
      JSON.parse(jsonSample);
    }

    let sink = 0;
    const t0 = performance.now();
    for (let i = 0; i < iterations; i++) {
      const v = parse(sample);
      if (v) sink++;
    }
    const lmonSec = (performance.now() - t0) / 1000;

    const t1 = performance.now();
    for (let i = 0; i < iterations; i++) {
      const v = JSON.parse(jsonSample);
      if (v) sink++;
    }
    const jsonSec = (performance.now() - t1) / 1000;
    if (sink === -1) console.log(sink);

    const lmonMbs = (lmonBytes * iterations) / (1024 * 1024 * lmonSec);
    const jsonMbs = (jsonBytes * iterations) / (1024 * 1024 * jsonSec);
    console.log(`JavaScript (Node ${process.version}) benchmark (${iterations} iterations):`);
    console.log(`  LMON (pure JS):   ${kb.toFixed(1)} KiB/doc -> ${lmonSec.toFixed(3)}s (${lmonMbs.toFixed(1)} MiB/s)`);
    console.log(`  JSON (native C++): ${(jsonBytes / 1024).toFixed(1)} KiB/doc -> ${jsonSec.toFixed(3)}s (${jsonMbs.toFixed(1)} MiB/s)`);
    process.exit(0);
  }

  let compact = false;
  let outLmon = false;
  let fromJson = false;
  let checkOnly = false;
  let target = null;

  for (const arg of argv) {
    if (arg === "-c" || arg === "--compact") compact = true;
    else if (arg === "-l" || arg === "--lmon") outLmon = true;
    else if (arg === "--from-json") {
      fromJson = true;
      outLmon = true;
    } else if (arg === "--check") checkOnly = true;
    else if (arg.startsWith("-") && arg !== "-") {
      process.stderr.write(`Unknown option: ${arg}\n${usage}`);
      process.exit(2);
    } else {
      target = arg;
    }
  }

  if (!target) {
    process.stderr.write(usage);
    process.exit(2);
  }

  try {
    let raw;
    if (target === "-") {
      raw = fs.readFileSync(0, "utf8");
    } else {
      raw = fs.readFileSync(target, "utf8");
      if (target.endsWith(".json")) fromJson = true;
    }

    const data = fromJson ? JSON.parse(raw) : parse(raw);
    if (checkOnly) process.exit(0);

    if (outLmon) {
      process.stdout.write(stringify(data));
    } else if (compact) {
      process.stdout.write(`${JSON.stringify(data)}\n`);
    } else {
      process.stdout.write(`${JSON.stringify(data, null, 2)}\n`);
    }
  } catch (err) {
    process.stderr.write(`lmon error: ${err.message}\n`);
    process.exit(1);
  }
}
