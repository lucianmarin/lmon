// LMON (Light Machine / Lucian Marin Object Notation) — single-file Rust implementation.
// Zero external dependencies (pure Rust std, Edition 2021+).
//
// Build & CLI usage:
//   rustc -O lmon.rs -o lmon_rs
//   ./lmon_rs examples/gateway.lmon
//   ./lmon_rs --compact examples/gateway.lmon
//   ./lmon_rs --lmon examples/gateway.lmon
//   ./lmon_rs --from-json input.json
//   ./lmon_rs --check examples/gateway.lmon
//   ./lmon_rs --test
//   ./lmon_rs --bench [file.lmon]

#![allow(dead_code)]
#![forbid(unsafe_code)]

use std::borrow::Cow;
use std::env;
use std::fmt;
use std::fs;
use std::io::{self, Read, Write};
use std::process;
use std::time::Instant;

const INLINE_MAX_BYTES: usize = 80;

#[derive(Debug, Clone, PartialEq)]
pub enum Value<'a> {
    Null,
    Bool(bool),
    Int(i64),
    Float(f64),
    String(Cow<'a, str>),
    Array(Vec<Value<'a>>),
    Object(Vec<(Cow<'a, str>, Value<'a>)>),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ParseError {
    pub message: String,
    pub pos: usize,
    pub line: usize,
    pub col: usize,
}

impl fmt::Display for ParseError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "{}: line {} column {} (byte {})",
            self.message, self.line, self.col, self.pos
        )
    }
}

impl std::error::Error for ParseError {}

const KEY_CLASS: [u8; 256] = {
    let mut table = [0u8; 256];
    let mut i = b'A';
    while i <= b'Z' {
        table[i as usize] = 1;
        i += 1;
    }
    let mut i = b'a';
    while i <= b'z' {
        table[i as usize] = 1;
        i += 1;
    }
    table[b'_' as usize] = 1;
    let mut i = b'0';
    while i <= b'9' {
        table[i as usize] = 2;
        i += 1;
    }
    table[b'-' as usize] = 2;
    table
};

const POW10_F64: [f64; 16] = [
    1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15,
];

const POW10_U64: [u64; 16] = [
    1,
    10,
    100,
    1_000,
    10_000,
    100_000,
    1_000_000,
    10_000_000,
    100_000_000,
    1_000_000_000,
    10_000_000_000,
    100_000_000_000,
    1_000_000_000_000,
    10_000_000_000_000,
    100_000_000_000_000,
    1_000_000_000_000_000,
];

#[inline]
fn is_ident_start(b: u8) -> bool {
    KEY_CLASS[b as usize] == 1
}

#[inline]
fn is_ident_cont(b: u8) -> bool {
    KEY_CLASS[b as usize] != 0
}

#[inline]
fn is_reserved_key(s: &str) -> bool {
    match s.len() {
        4 => s == "true" || s == "null",
        5 => s == "false",
        _ => false,
    }
}

#[inline]
fn hex_val(b: u8) -> Option<u32> {
    match b {
        b'0'..=b'9' => Some((b - b'0') as u32),
        b'a'..=b'f' => Some((b - b'a' + 10) as u32),
        b'A'..=b'F' => Some((b - b'A' + 10) as u32),
        _ => None,
    }
}

struct Parser<'a> {
    src: &'a str,
    bytes: &'a [u8],
    pos: usize,
    line_start: usize,
    line_indent: i32, // -2 = uncached, -1 = EOF, >= 0 = indent level
}

impl<'a> Parser<'a> {
    fn new(src: &'a str) -> Self {
        Self {
            src,
            bytes: src.as_bytes(),
            pos: 0,
            line_start: 0,
            line_indent: -2,
        }
    }

    fn err<T>(&self, msg: &str, at: usize) -> Result<T, ParseError> {
        let clamped = at.min(self.bytes.len());
        let mut line = 1usize;
        let mut col = 1usize;
        for &b in &self.bytes[..clamped] {
            if b == b'\n' {
                line += 1;
                col = 1;
            } else if (b & 0xC0) != 0x80 {
                // Count UTF-8 scalar values for column numbers
                col += 1;
            }
        }
        Err(ParseError {
            message: msg.to_string(),
            pos: clamped,
            line,
            col,
        })
    }

    #[inline]
    fn peek_line(&mut self) -> Result<i32, ParseError> {
        if self.line_indent != -2 {
            return Ok(self.line_indent);
        }

        let bytes = self.bytes;
        let len = bytes.len();
        let mut p = self.pos;

        while p < len {
            let line_begin = p;

            // Fast 4-byte stride for 4-space indentation blocks
            while p + 4 <= len && &bytes[p..p + 4] == b"    " {
                p += 4;
            }
            while p < len && bytes[p] == b' ' {
                p += 1;
            }

            if p >= len {
                self.pos = p;
                self.line_indent = -1;
                return Ok(-1);
            }

            match bytes[p] {
                b'\n' => {
                    p += 1;
                    continue;
                }
                b'\r' => {
                    if p + 1 >= len || bytes[p + 1] != b'\n' {
                        return self.err("bare CR is not allowed", p);
                    }
                    p += 2;
                    continue;
                }
                b'\t' => {
                    return self.err("tabs are not allowed for indentation", p);
                }
                _ => {}
            }

            let spaces = p - line_begin;
            if (spaces & 3) != 0 {
                return self.err("indentation must be a multiple of 4 spaces", line_begin);
            }

            self.pos = p;
            self.line_start = line_begin;
            self.line_indent = (spaces >> 2) as i32;
            return Ok(self.line_indent);
        }

        self.pos = p;
        self.line_indent = -1;
        Ok(-1)
    }

    #[inline]
    fn consume_line_end(&mut self, allow_eof: bool) -> Result<(), ParseError> {
        let bytes = self.bytes;
        let p = self.pos;
        if p >= bytes.len() {
            if !allow_eof {
                return self.err("unexpected end of input; expected nested block", p);
            }
            self.line_indent = -1;
            return Ok(());
        }

        match bytes[p] {
            b'\n' => {
                self.pos = p + 1;
                self.line_indent = -2;
                Ok(())
            }
            b'\r' => {
                if p + 1 < bytes.len() && bytes[p + 1] == b'\n' {
                    self.pos = p + 2;
                    self.line_indent = -2;
                    Ok(())
                } else {
                    self.err("bare CR is not allowed", p)
                }
            }
            b'\t' => self.err("tabs are not allowed outside strings", p),
            b' ' => self.err("trailing spaces are not allowed", p),
            _ => self.err("unexpected character at end of line", p),
        }
    }

    #[inline]
    fn is_dash_item(&self) -> bool {
        let p = self.pos;
        if p >= self.bytes.len() || self.bytes[p] != b'-' {
            return false;
        }
        if p + 1 >= self.bytes.len() {
            return true;
        }
        matches!(self.bytes[p + 1], b' ' | b'\n' | b'\r')
    }

    fn parse_string(&mut self) -> Result<Cow<'a, str>, ParseError> {
        let src = self.src;
        let bytes = self.bytes;
        let len = bytes.len();
        let mut p = self.pos;

        if p >= len || bytes[p] != b'"' {
            return self.err("expected '\"'", p);
        }
        p += 1;
        let start = p;

        // Fast zero-copy path: string with no backslash escapes
        while p < len {
            let b = bytes[p];
            if b == b'"' {
                self.pos = p + 1;
                return Ok(Cow::Borrowed(&src[start..p]));
            }
            if b == b'\\' {
                break;
            }
            if b < 0x20 {
                return self.err("unescaped control character in string", p);
            }
            p += 1;
        }

        if p >= len {
            return self.err("unterminated string literal", p);
        }

        let mut out = String::with_capacity((p - start) + 16);
        out.push_str(&src[start..p]);

        while p < len {
            let b = bytes[p];
            if b == b'"' {
                self.pos = p + 1;
                return Ok(Cow::Owned(out));
            }
            if b < 0x20 {
                return self.err("unescaped control character in string", p);
            }
            if b != b'\\' {
                let chunk_start = p;
                p += 1;
                while p < len && bytes[p] != b'"' && bytes[p] != b'\\' && bytes[p] >= 0x20 {
                    p += 1;
                }
                out.push_str(&src[chunk_start..p]);
                continue;
            }

            p += 1; // skip '\\'
            if p >= len {
                return self.err("unterminated escape sequence", p);
            }
            let esc = bytes[p];
            p += 1;
            match esc {
                b'"' => out.push('"'),
                b'\\' => out.push('\\'),
                b'/' => out.push('/'),
                b'b' => out.push('\u{0008}'),
                b'f' => out.push('\u{000C}'),
                b'n' => out.push('\n'),
                b'r' => out.push('\r'),
                b't' => out.push('\t'),
                b'u' => {
                    if p + 4 > len {
                        return self.err("invalid \\uXXXX escape sequence", p);
                    }
                    let cp = match (
                        hex_val(bytes[p]),
                        hex_val(bytes[p + 1]),
                        hex_val(bytes[p + 2]),
                        hex_val(bytes[p + 3]),
                    ) {
                        (Some(a), Some(b), Some(c), Some(d)) => {
                            (a << 12) | (b << 8) | (c << 4) | d
                        }
                        _ => return self.err("invalid \\uXXXX escape sequence", p),
                    };
                    p += 4;

                    let scalar = if (0xD800..=0xDBFF).contains(&cp) {
                        if p + 6 > len || bytes[p] != b'\\' || bytes[p + 1] != b'u' {
                            return self.err("unpaired high surrogate in string", p);
                        }
                        let low = match (
                            hex_val(bytes[p + 2]),
                            hex_val(bytes[p + 3]),
                            hex_val(bytes[p + 4]),
                            hex_val(bytes[p + 5]),
                        ) {
                            (Some(a), Some(b), Some(c), Some(d)) => {
                                (a << 12) | (b << 8) | (c << 4) | d
                            }
                            _ => return self.err("invalid low surrogate \\uXXXX escape", p + 2),
                        };
                        if !(0xDC00..=0xDFFF).contains(&low) {
                            return self.err("expected low surrogate (DC00..DFFF)", p + 2);
                        }
                        p += 6;
                        0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00)
                    } else if (0xDC00..=0xDFFF).contains(&cp) {
                        return self.err("unpaired low surrogate in string", p - 4);
                    } else {
                        cp
                    };

                    match char::from_u32(scalar) {
                        Some(ch) => out.push(ch),
                        None => return self.err("invalid Unicode scalar value", p),
                    }
                }
                _ => return self.err("invalid escape sequence in string", p - 1),
            }
        }

        self.err("unterminated string literal", p)
    }

    fn parse_number(&mut self) -> Result<Value<'a>, ParseError> {
        let src = self.src;
        let bytes = self.bytes;
        let len = bytes.len();
        let start = self.pos;
        let mut p = start;

        let mut neg = false;
        if p < len && bytes[p] == b'-' {
            neg = true;
            p += 1;
        }
        if p >= len || !bytes[p].is_ascii_digit() {
            return self.err("invalid number literal", start);
        }

        let mut mantissa: u64 = 0;
        let mut int_digits: usize = 0;
        if bytes[p] == b'0' {
            p += 1;
            int_digits = 1;
            if p < len && bytes[p].is_ascii_digit() {
                return self.err("leading zeros are not allowed in numbers", p);
            }
        } else {
            while p < len && bytes[p].is_ascii_digit() {
                if int_digits < 19 {
                    mantissa = mantissa * 10 + (bytes[p] - b'0') as u64;
                }
                int_digits += 1;
                p += 1;
            }
        }

        let mut is_float = false;
        let mut frac_digits: usize = 0;
        let mut frac_val: u64 = 0;
        if p < len && bytes[p] == b'.' {
            is_float = true;
            p += 1;
            if p >= len || !bytes[p].is_ascii_digit() {
                return self.err("expected digit after decimal point", p);
            }
            while p < len && bytes[p].is_ascii_digit() {
                if frac_digits < 16 {
                    frac_val = frac_val * 10 + (bytes[p] - b'0') as u64;
                }
                frac_digits += 1;
                p += 1;
            }
        }

        let mut has_exp = false;
        if p < len && (bytes[p] == b'e' || bytes[p] == b'E') {
            is_float = true;
            has_exp = true;
            p += 1;
            if p < len && (bytes[p] == b'+' || bytes[p] == b'-') {
                p += 1;
            }
            if p >= len || !bytes[p].is_ascii_digit() {
                return self.err("expected digit in exponent", p);
            }
            while p < len && bytes[p].is_ascii_digit() {
                p += 1;
            }
        }

        if p < len && matches!(bytes[p], b'.' | b'e' | b'E' | b'+' | b'-') {
            return self.err("malformed number literal", p);
        }

        self.pos = p;

        if !is_float {
            if int_digits <= 18 {
                let iv = if neg { -(mantissa as i64) } else { mantissa as i64 };
                return Ok(Value::Int(iv));
            }
            let raw = &src[start..p];
            if let Ok(iv) = raw.parse::<i64>() {
                return Ok(Value::Int(iv));
            }
        } else if !has_exp && int_digits + frac_digits <= 15 {
            let combined = mantissa * POW10_U64[frac_digits] + frac_val;
            let mut fv = (combined as f64) / POW10_F64[frac_digits];
            if neg {
                fv = -fv;
            }
            return Ok(Value::Float(fv));
        }

        let raw = &src[start..p];
        match raw.parse::<f64>() {
            Ok(fv) if fv.is_finite() => Ok(Value::Float(fv)),
            _ => self.err("numeric literal out of finite IEEE-754 range", start),
        }
    }

    fn parse_inline_array(&mut self) -> Result<Value<'a>, ParseError> {
        self.pos += 1; // '['
        while self.pos < self.bytes.len() && self.bytes[self.pos] == b' ' {
            self.pos += 1;
        }
        if self.pos < self.bytes.len() && self.bytes[self.pos] == b']' {
            self.pos += 1;
            return Ok(Value::Array(Vec::new()));
        }

        let mut items = Vec::with_capacity(4);
        loop {
            items.push(self.parse_inline_value()?);
            while self.pos < self.bytes.len() && self.bytes[self.pos] == b' ' {
                self.pos += 1;
            }
            if self.pos >= self.bytes.len() {
                return self.err("unterminated inline array", self.pos);
            }
            match self.bytes[self.pos] {
                b',' => {
                    self.pos += 1;
                    while self.pos < self.bytes.len() && self.bytes[self.pos] == b' ' {
                        self.pos += 1;
                    }
                    if self.pos < self.bytes.len() && self.bytes[self.pos] == b']' {
                        return self.err("trailing comma in inline array", self.pos);
                    }
                }
                b']' => {
                    self.pos += 1;
                    return Ok(Value::Array(items));
                }
                b'\t' => return self.err("tabs are not allowed outside strings", self.pos),
                _ => return self.err("expected ',' or ']' in inline array", self.pos),
            }
        }
    }

    fn parse_inline_value(&mut self) -> Result<Value<'a>, ParseError> {
        if self.pos >= self.bytes.len() {
            return self.err("expected value", self.pos);
        }
        match self.bytes[self.pos] {
            b'"' => Ok(Value::String(self.parse_string()?)),
            b'-' | b'0'..=b'9' => self.parse_number(),
            b't' => {
                if self.bytes[self.pos..].starts_with(b"true") {
                    self.pos += 4;
                    Ok(Value::Bool(true))
                } else {
                    self.err("invalid literal; expected 'true'", self.pos)
                }
            }
            b'f' => {
                if self.bytes[self.pos..].starts_with(b"false") {
                    self.pos += 5;
                    Ok(Value::Bool(false))
                } else {
                    self.err("invalid literal; expected 'false'", self.pos)
                }
            }
            b'n' => {
                if self.bytes[self.pos..].starts_with(b"null") {
                    self.pos += 4;
                    Ok(Value::Null)
                } else {
                    self.err("invalid literal; expected 'null'", self.pos)
                }
            }
            b'[' => self.parse_inline_array(),
            b'{' => {
                if self.bytes[self.pos..].starts_with(b"{}") {
                    self.pos += 2;
                    Ok(Value::Object(Vec::new()))
                } else {
                    self.err(
                        "curly-brace objects are not allowed in LMON; use indentation or '{}'",
                        self.pos,
                    )
                }
            }
            _ => self.err("unexpected character when parsing value", self.pos),
        }
    }

    fn parse_key(&mut self) -> Result<Cow<'a, str>, ParseError> {
        let src = self.src;
        let p = self.pos;
        if p < self.bytes.len() && self.bytes[p] == b'"' {
            return self.parse_string();
        }
        if p >= self.bytes.len() || !is_ident_start(self.bytes[p]) {
            return self.err("expected object key", p);
        }
        let mut q = p + 1;
        while q < self.bytes.len() && is_ident_cont(self.bytes[q]) {
            q += 1;
        }
        let key = &src[p..q];
        if is_reserved_key(key) {
            return self.err("reserved literal must be quoted when used as a key", p);
        }
        self.pos = q;
        Ok(Cow::Borrowed(key))
    }

    fn parse_nested_block(&mut self, expected_indent: i32) -> Result<Value<'a>, ParseError> {
        let indent = self.peek_line()?;
        if indent < 0 {
            return self.err("expected indented block, got end of input", self.pos);
        }
        if indent != expected_indent {
            return self.err("unexpected block indentation level", self.line_start);
        }
        if self.is_dash_item() {
            return self.parse_block_array(expected_indent);
        }
        let b = self.bytes[self.pos];
        if b == b'"' {
            let str_start = self.pos;
            let first_key = self.parse_string()?;
            if self.pos >= self.bytes.len() || self.bytes[self.pos] != b':' {
                return self.err("nested block cannot be a standalone scalar", str_start);
            }
            return self.parse_block_object(expected_indent, Some(first_key));
        }
        if is_ident_start(b) {
            return self.parse_block_object(expected_indent, None);
        }
        self.err("nested block must be a block object or block array", self.pos)
    }

    fn parse_block_object(
        &mut self,
        expected_indent: i32,
        mut first_key: Option<Cow<'a, str>>,
    ) -> Result<Value<'a>, ParseError> {
        let mut members: Vec<(Cow<'a, str>, Value<'a>)> = Vec::with_capacity(8);

        loop {
            let key = if let Some(k) = first_key.take() {
                k
            } else {
                let indent = self.peek_line()?;
                if indent < expected_indent {
                    break;
                }
                if indent > expected_indent {
                    return self.err("unexpected indentation inside object", self.line_start);
                }
                if self.is_dash_item() {
                    return self.err(
                        "cannot mix array items and object keys at same indentation",
                        self.pos,
                    );
                }
                self.parse_key()?
            };

            if self.pos >= self.bytes.len() || self.bytes[self.pos] != b':' {
                return self.err("expected ':' after object key", self.pos);
            }
            self.pos += 1;

            let val = if self.pos < self.bytes.len() && self.bytes[self.pos] == b' ' {
                self.pos += 1;
                let v = self.parse_inline_value()?;
                self.consume_line_end(true)?;
                v
            } else {
                self.consume_line_end(false)?;
                self.parse_nested_block(expected_indent + 1)?
            };

            if let Some(existing) = members.iter_mut().find(|(k, _)| k.as_ref() == key.as_ref()) {
                existing.1 = val;
            } else {
                members.push((key, val));
            }
        }

        Ok(Value::Object(members))
    }

    fn parse_block_array(&mut self, expected_indent: i32) -> Result<Value<'a>, ParseError> {
        let mut items: Vec<Value<'a>> = Vec::with_capacity(8);

        loop {
            let indent = self.peek_line()?;
            if indent < expected_indent {
                break;
            }
            if indent > expected_indent {
                return self.err("unexpected indentation inside array", self.line_start);
            }
            if !self.is_dash_item() {
                return self.err(
                    "cannot mix object keys and array items at same indentation",
                    self.pos,
                );
            }
            self.pos += 1; // '-'

            let elem = if self.pos < self.bytes.len() && self.bytes[self.pos] == b' ' {
                self.pos += 1;
                let v = self.parse_inline_value()?;
                self.consume_line_end(true)?;
                v
            } else {
                self.consume_line_end(false)?;
                self.parse_nested_block(expected_indent + 1)?
            };
            items.push(elem);
        }

        Ok(Value::Array(items))
    }

    fn parse_document(&mut self) -> Result<Value<'a>, ParseError> {
        if self.src.starts_with('\u{FEFF}') {
            return self.err("UTF-8 BOM is not allowed", 0);
        }

        let indent = self.peek_line()?;
        if indent < 0 {
            return self.err("empty LMON document", self.pos);
        }
        if indent != 0 {
            return self.err("root value must not be indented", self.line_start);
        }

        let root = if self.is_dash_item() {
            self.parse_block_array(0)?
        } else {
            let b = self.bytes[self.pos];
            if b == b'"' {
                let first_s = self.parse_string()?;
                if self.pos < self.bytes.len() && self.bytes[self.pos] == b':' {
                    self.parse_block_object(0, Some(first_s))?
                } else {
                    self.consume_line_end(true)?;
                    Value::String(first_s)
                }
            } else if is_ident_start(b) {
                let src = self.src;
                let mut q = self.pos + 1;
                while q < self.bytes.len() && is_ident_cont(self.bytes[q]) {
                    q += 1;
                }
                let ident = &src[self.pos..q];
                if is_reserved_key(ident) && (q >= self.bytes.len() || self.bytes[q] != b':') {
                    let v = self.parse_inline_value()?;
                    self.consume_line_end(true)?;
                    v
                } else {
                    self.parse_block_object(0, None)?
                }
            } else {
                let v = self.parse_inline_value()?;
                self.consume_line_end(true)?;
                v
            }
        };

        let trailing = self.peek_line()?;
        if trailing != -1 {
            return self.err("unexpected trailing content after root value", self.line_start);
        }
        Ok(root)
    }

    // Minimal RFC 8259 JSON parser for `--from-json` and head-to-head benchmarking
    fn skip_json_ws(&mut self) {
        while self.pos < self.bytes.len() {
            match self.bytes[self.pos] {
                b' ' | b'\t' | b'\n' | b'\r' => self.pos += 1,
                _ => break,
            }
        }
    }

    fn parse_json_value(&mut self) -> Result<Value<'a>, ParseError> {
        self.skip_json_ws();
        if self.pos >= self.bytes.len() {
            return self.err("expected JSON value", self.pos);
        }
        match self.bytes[self.pos] {
            b'{' => {
                self.pos += 1;
                self.skip_json_ws();
                if self.pos < self.bytes.len() && self.bytes[self.pos] == b'}' {
                    self.pos += 1;
                    return Ok(Value::Object(Vec::new()));
                }
                let mut members: Vec<(Cow<'a, str>, Value<'a>)> = Vec::with_capacity(8);
                loop {
                    self.skip_json_ws();
                    let k = self.parse_string()?;
                    self.skip_json_ws();
                    if self.pos >= self.bytes.len() || self.bytes[self.pos] != b':' {
                        return self.err("expected ':' in JSON object", self.pos);
                    }
                    self.pos += 1;
                    let v = self.parse_json_value()?;
                    if let Some(existing) = members.iter_mut().find(|(ek, _)| ek.as_ref() == k.as_ref()) {
                        existing.1 = v;
                    } else {
                        members.push((k, v));
                    }
                    self.skip_json_ws();
                    if self.pos >= self.bytes.len() {
                        return self.err("unterminated JSON object", self.pos);
                    }
                    match self.bytes[self.pos] {
                        b',' => self.pos += 1,
                        b'}' => {
                            self.pos += 1;
                            return Ok(Value::Object(members));
                        }
                        _ => return self.err("expected ',' or '}' in JSON object", self.pos),
                    }
                }
            }
            b'[' => {
                self.pos += 1;
                self.skip_json_ws();
                if self.pos < self.bytes.len() && self.bytes[self.pos] == b']' {
                    self.pos += 1;
                    return Ok(Value::Array(Vec::new()));
                }
                let mut items = Vec::with_capacity(4);
                loop {
                    items.push(self.parse_json_value()?);
                    self.skip_json_ws();
                    if self.pos >= self.bytes.len() {
                        return self.err("unterminated JSON array", self.pos);
                    }
                    match self.bytes[self.pos] {
                        b',' => self.pos += 1,
                        b']' => {
                            self.pos += 1;
                            return Ok(Value::Array(items));
                        }
                        _ => return self.err("expected ',' or ']' in JSON array", self.pos),
                    }
                }
            }
            b'"' => Ok(Value::String(self.parse_string()?)),
            b't' if self.bytes[self.pos..].starts_with(b"true") => {
                self.pos += 4;
                Ok(Value::Bool(true))
            }
            b'f' if self.bytes[self.pos..].starts_with(b"false") => {
                self.pos += 5;
                Ok(Value::Bool(false))
            }
            b'n' if self.bytes[self.pos..].starts_with(b"null") => {
                self.pos += 4;
                Ok(Value::Null)
            }
            _ => self.parse_number(),
        }
    }
}

pub fn parse<'a>(src: &'a str) -> Result<Value<'a>, ParseError> {
    Parser::new(src).parse_document()
}

pub fn parse_json<'a>(src: &'a str) -> Result<Value<'a>, ParseError> {
    let mut p = Parser::new(src);
    let val = p.parse_json_value()?;
    p.skip_json_ws();
    if p.pos != p.bytes.len() {
        return p.err("trailing data after JSON value", p.pos);
    }
    Ok(val)
}

// ---------------------------------------------------------------------------
// Canonical LMON & JSON Serializers
// ---------------------------------------------------------------------------

fn write_escaped_string(out: &mut String, s: &str) {
    out.push('"');
    for ch in s.chars() {
        match ch {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\u{0008}' => out.push_str("\\b"),
            '\u{000C}' => out.push_str("\\f"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if (c as u32) < 0x20 => {
                out.push_str(&format!("\\u{:04x}", c as u32));
            }
            c => out.push(c),
        }
    }
    out.push('"');
}

fn write_float(out: &mut String, f: f64) -> Result<(), String> {
    if !f.is_finite() {
        return Err("NaN/Infinity are not LMON compliant".to_string());
    }
    if f == 0.0 {
        out.push_str("0.0");
        return Ok(());
    }
    let s = f.to_string();
    out.push_str(&s);
    if !s.contains('.') && !s.contains('e') && !s.contains('E') {
        out.push_str(".0");
    }
    Ok(())
}

fn can_be_unquoted_key(k: &str) -> bool {
    let b = k.as_bytes();
    !b.is_empty()
        && !is_reserved_key(k)
        && is_ident_start(b[0])
        && b[1..].iter().all(|&c| is_ident_cont(c))
}

fn try_write_inline(out: &mut String, v: &Value<'_>) -> Result<bool, String> {
    match v {
        Value::Null => {
            out.push_str("null");
            Ok(true)
        }
        Value::Bool(true) => {
            out.push_str("true");
            Ok(true)
        }
        Value::Bool(false) => {
            out.push_str("false");
            Ok(true)
        }
        Value::Int(i) => {
            out.push_str(&i.to_string());
            Ok(true)
        }
        Value::Float(f) => {
            write_float(out, *f)?;
            Ok(true)
        }
        Value::String(s) => {
            write_escaped_string(out, s);
            Ok(true)
        }
        Value::Object(members) => {
            if members.is_empty() {
                out.push_str("{}");
                Ok(true)
            } else {
                Ok(false)
            }
        }
        Value::Array(items) => {
            if items.is_empty() {
                out.push_str("[]");
                return Ok(true);
            }
            let start_len = out.len();
            out.push('[');
            for (idx, item) in items.iter().enumerate() {
                if idx > 0 {
                    out.push_str(", ");
                }
                if !try_write_inline(out, item)? {
                    out.truncate(start_len);
                    return Ok(false);
                }
                if out.len() - start_len + 1 > INLINE_MAX_BYTES {
                    out.truncate(start_len);
                    return Ok(false);
                }
            }
            out.push(']');
            if out.len() - start_len > INLINE_MAX_BYTES {
                out.truncate(start_len);
                return Ok(false);
            }
            Ok(true)
        }
    }
}

fn emit_block(out: &mut String, v: &Value<'_>, indent: usize) -> Result<(), String> {
    let pad = "    ".repeat(indent);
    match v {
        Value::Object(members) => {
            for (k, val) in members {
                out.push_str(&pad);
                if can_be_unquoted_key(k) {
                    out.push_str(k);
                } else {
                    write_escaped_string(out, k);
                }
                out.push(':');
                let colon_pos = out.len();
                out.push(' ');
                if try_write_inline(out, val)? {
                    out.push('\n');
                } else {
                    out.truncate(colon_pos);
                    out.push('\n');
                    emit_block(out, val, indent + 1)?;
                }
            }
            Ok(())
        }
        Value::Array(items) => {
            for item in items {
                out.push_str(&pad);
                out.push('-');
                let dash_pos = out.len();
                out.push(' ');
                if try_write_inline(out, item)? {
                    out.push('\n');
                } else {
                    out.truncate(dash_pos);
                    out.push('\n');
                    emit_block(out, item, indent + 1)?;
                }
            }
            Ok(())
        }
        _ => Err("internal error: scalar passed to emit_block".to_string()),
    }
}

pub fn to_lmon(v: &Value<'_>) -> Result<String, String> {
    let mut out = String::with_capacity(256);
    if try_write_inline(&mut out, v)? {
        out.push('\n');
    } else {
        emit_block(&mut out, v, 0)?;
    }
    Ok(out)
}

fn write_json_rec(out: &mut String, v: &Value<'_>, pretty: bool, depth: usize) -> Result<(), String> {
    match v {
        Value::Null => out.push_str("null"),
        Value::Bool(true) => out.push_str("true"),
        Value::Bool(false) => out.push_str("false"),
        Value::Int(i) => out.push_str(&i.to_string()),
        Value::Float(f) => write_float(out, *f)?,
        Value::String(s) => write_escaped_string(out, s),
        Value::Array(items) => {
            if items.is_empty() {
                out.push_str("[]");
            } else {
                out.push('[');
                for (i, item) in items.iter().enumerate() {
                    if i > 0 {
                        out.push(',');
                    }
                    if pretty {
                        out.push('\n');
                        for _ in 0..((depth + 1) * 2) {
                            out.push(' ');
                        }
                    }
                    write_json_rec(out, item, pretty, depth + 1)?;
                }
                if pretty {
                    out.push('\n');
                    for _ in 0..(depth * 2) {
                        out.push(' ');
                    }
                }
                out.push(']');
            }
        }
        Value::Object(members) => {
            if members.is_empty() {
                out.push_str("{}");
            } else {
                out.push('{');
                for (i, (k, val)) in members.iter().enumerate() {
                    if i > 0 {
                        out.push(',');
                    }
                    if pretty {
                        out.push('\n');
                        for _ in 0..((depth + 1) * 2) {
                            out.push(' ');
                        }
                    }
                    write_escaped_string(out, k);
                    if pretty {
                        out.push_str(": ");
                    } else {
                        out.push(':');
                    }
                    write_json_rec(out, val, pretty, depth + 1)?;
                }
                if pretty {
                    out.push('\n');
                    for _ in 0..(depth * 2) {
                        out.push(' ');
                    }
                }
                out.push('}');
            }
        }
    }
    Ok(())
}

pub fn to_json(v: &Value<'_>, pretty: bool) -> Result<String, String> {
    let mut out = String::with_capacity(256);
    write_json_rec(&mut out, v, pretty, 0)?;
    out.push('\n');
    Ok(out)
}

// ---------------------------------------------------------------------------
// Conformance Tests & CLI
// ---------------------------------------------------------------------------

fn run_tests() -> i32 {
    let json_cases = [
        "{}",
        "[]",
        "-99",
        "\"hello: world\"",
        "true",
        "null",
        "{\"s\":\"Lemon\",\"i\":-42,\"f\":3.25,\"t\":true,\"fa\":false,\"n\":null}",
        "{\"service\":\"lmon\",\"tags\":[\"a\",\"b\",\"c\"],\"empty_map\":{},\"empty_list\":[],\
         \"matrix\":[[1,2],[3,4]],\"nodes\":[{\"id\":1,\"meta\":{\"ok\":true}},{\"id\":2,\"meta\":{}}],\
         \"\":\"empty\",\"true\":\"keyword\",\"false\":false,\"null\":null,\"123\":\"numeric-key\",\
         \"-dash\":\"dash-key\",\"with space\":\"quoted\\nline\\t\\uD83C\\uDF4B\"}",
    ];

    for (idx, jc) in json_cases.iter().enumerate() {
        let v1 = match parse_json(jc) {
            Ok(v) => v,
            Err(e) => {
                eprintln!("FAIL [case {idx} json parse]: {e}");
                return 1;
            }
        };
        let lmon_txt = match to_lmon(&v1) {
            Ok(s) => s,
            Err(e) => {
                eprintln!("FAIL [case {idx} to_lmon]: {e}");
                return 1;
            }
        };
        let v2 = match parse(&lmon_txt) {
            Ok(v) => v,
            Err(e) => {
                eprintln!("FAIL [case {idx} lmon parse]: {e}\n{lmon_txt}");
                return 1;
            }
        };
        if v1 != v2 {
            eprintln!("FAIL [case {idx} round-trip mismatch]:\nGot: {v2:?}\nExp: {v1:?}");
            return 1;
        }
    }

    let invalid_cases = [
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
    ];

    for (idx, bad) in invalid_cases.iter().enumerate() {
        if parse(bad).is_ok() {
            eprintln!("FAIL [invalid {idx} accepted]: {bad:?}");
            return 1;
        }
    }

    println!(
        "Rust LMON: all {} conformance tests passed.",
        json_cases.len() + invalid_cases.len()
    );
    0
}

fn run_bench(path: Option<&str>) -> i32 {
    let sample = if let Some(p) = path {
        match fs::read_to_string(p) {
            Ok(s) => s,
            Err(e) => {
                eprintln!("lmon error: cannot read '{p}': {e}");
                return 1;
            }
        }
    } else {
        let mut s = String::from("cluster: \"lmon-bench\"\ncount: 500\nnodes:\n");
        for i in 0..500 {
            let active = if i % 2 == 0 { "true" } else { "false" };
            let latency = 1.25 * ((i % 17) as f64);
            s.push_str(&format!(
                "    -\n        id: {i}\n        name: \"service-node-{i}\"\n        active: {active}\n        latency_ms: {latency:.3}\n        tags: [\"edge\", \"lemon\", \"v1\"]\n        meta:\n            region: \"eu-central\"\n            capacity: {}\n            extra: {{}}\n",
                256 + i
            ));
        }
        s
    };

    let parsed = match parse(&sample) {
        Ok(v) => v,
        Err(e) => {
            eprintln!("lmon bench parse error: {e}");
            return 1;
        }
    };
    let json_sample = to_json(&parsed, true).unwrap();

    let kb = (sample.len() as f64) / 1024.0;
    let iterations = ((100_000.0 / kb.max(1.0)) as usize).max(50);

    let t0 = Instant::now();
    for _ in 0..iterations {
        let v = parse(&sample).unwrap();
        std::hint::black_box(v);
    }
    let lmon_sec = t0.elapsed().as_secs_f64();

    let t1 = Instant::now();
    for _ in 0..iterations {
        let v = parse_json(&json_sample).unwrap();
        std::hint::black_box(v);
    }
    let json_sec = t1.elapsed().as_secs_f64();

    let lmon_mbs = (sample.len() * iterations) as f64 / (1024.0 * 1024.0 * lmon_sec);
    let json_mbs = (json_sample.len() * iterations) as f64 / (1024.0 * 1024.0 * json_sec);
    println!("Rust LMON benchmark ({iterations} iterations):");
    println!("  LMON: {:.1} KiB/doc -> {:.3}s ({:.1} MiB/s)", kb, lmon_sec, lmon_mbs);
    println!(
        "  JSON: {:.1} KiB/doc -> {:.3}s ({:.1} MiB/s)",
        json_sample.len() as f64 / 1024.0,
        json_sec,
        json_mbs
    );
    println!(
        "  Result: LMON parsed {:.2}x faster per document than equivalent JSON.",
        json_sec / lmon_sec
    );
    0
}

fn usage() -> &'static str {
    "Usage: lmon_rs [options] <file.lmon | ->\n\
     Options:\n\
     \x20 -c, --compact    Output compact single-line JSON\n\
     \x20 -l, --lmon       Output canonical LMON\n\
     \x20 --from-json      Parse input as JSON and output canonical LMON\n\
     \x20 --check          Validate LMON syntax only\n\
     \x20 --test           Run conformance test suite\n\
     \x20 --bench [file]   Run LMON vs JSON parse benchmark\n"
}

fn main() {
    let args: Vec<String> = env::args().skip(1).collect();
    if args.is_empty() || args.iter().any(|a| a == "-h" || a == "--help") {
        print!("{}", usage());
        process::exit(if args.is_empty() { 1 } else { 0 });
    }
    if args[0] == "--test" {
        process::exit(run_tests());
    }
    if args[0] == "--bench" {
        process::exit(run_bench(args.get(1).map(|s| s.as_str())));
    }

    let mut compact = false;
    let mut out_lmon = false;
    let mut from_json = false;
    let mut check_only = false;
    let mut target: Option<&str> = None;

    for arg in &args {
        match arg.as_str() {
            "-c" | "--compact" => compact = true,
            "-l" | "--lmon" => out_lmon = true,
            "--from-json" => {
                from_json = true;
                out_lmon = true;
            }
            "--check" => check_only = true,
            s if s.starts_with('-') && s != "-" => {
                eprintln!("Unknown option: {s}\n{}", usage());
                process::exit(2);
            }
            s => target = Some(s),
        }
    }

    let target_path = match target {
        Some(s) => s,
        None => {
            eprint!("{}", usage());
            process::exit(2);
        }
    };

    let raw = if target_path == "-" {
        let mut buf = String::new();
        if let Err(e) = io::stdin().read_to_string(&mut buf) {
            eprintln!("lmon error: {e}");
            process::exit(1);
        }
        buf
    } else {
        if target_path.ends_with(".json") {
            from_json = true;
        }
        match fs::read_to_string(target_path) {
            Ok(s) => s,
            Err(e) => {
                eprintln!("lmon error: failed to read '{target_path}': {e}");
                process::exit(1);
            }
        }
    };

    let value = if from_json {
        parse_json(&raw)
    } else {
        parse(&raw)
    };

    let value = match value {
        Ok(v) => v,
        Err(e) => {
            eprintln!("lmon error: {e}");
            process::exit(1);
        }
    };

    if check_only {
        return;
    }

    let rendered = if out_lmon {
        to_lmon(&value)
    } else {
        to_json(&value, !compact)
    };

    match rendered {
        Ok(s) => {
            let _ = io::stdout().write_all(s.as_bytes());
        }
        Err(e) => {
            eprintln!("lmon error: {e}");
            process::exit(1);
        }
    }
}
