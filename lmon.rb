#!/usr/bin/env ruby
# frozen_string_literal: true

# LMON (Light Machine / Lucian Marin Object Notation) — single-file Ruby implementation.
# Compatible with Ruby 2.6+ with zero external gems.
#
# Library API:
#   LMON.loads(str) / LMON.parse(str)
#   LMON.load(io)
#   LMON.dumps(obj) / LMON.generate(obj)
#
# CLI usage:
#   ruby lmon.rb <file.lmon>            # Parse LMON and print formatted JSON
#   ruby lmon.rb --compact <file.lmon>  # Parse LMON and print compact JSON
#   ruby lmon.rb --lmon <file>          # Output canonical LMON
#   ruby lmon.rb --from-json <file>     # Convert JSON file to LMON
#   ruby lmon.rb --check <file.lmon>    # Validate LMON syntax only
#   ruby lmon.rb --test                 # Run conformance test suite
#   ruby lmon.rb --bench [file.lmon]    # Run parse benchmark

require "json"
require "strscan"

module LMON
  class ParseError < StandardError
    attr_reader :lineno, :colno, :pos

    def initialize(message, doc, pos)
        prefix = doc.byteslice(0, pos) || ""
      line = 1
      col = 1
      i = 0
      chars = prefix.chars
      while i < chars.length
        ch = chars[i]
        if ch == "\n"
          line += 1
          col = 1
        elsif ch == "\r"
          line += 1
          col = 1
          i += 1 if i + 1 < chars.length && chars[i + 1] == "\n"
        else
          col += 1
        end
        i += 1
      end
      @lineno = line
      @colno = col
      @pos = pos
      super("#{message}: line #{@lineno} column #{@colno} (byte #{pos})")
    end
  end

  UNQUOTED_KEY_RE = /\A[A-Za-z_][A-Za-z0-9_-]*/.freeze
  VALID_KEY_FULL_RE = /\A[A-Za-z_][A-Za-z0-9_-]*\z/.freeze
  NUMBER_RE = /\A-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?/.freeze
  STRING_SAFE_RE = /[^"\\\x00-\x1f]+/.freeze
  HEX4_RE = /[0-9A-Fa-f]{4}/.freeze
  RESERVED_KEYS = { "true" => true, "false" => true, "null" => true }.freeze
  INLINE_MAX_BYTES = 80

  ESCAPE_MAP = {
    '"' => '"',
    "\\" => "\\",
    "/" => "/",
    "b" => "\b",
    "f" => "\f",
    "n" => "\n",
    "r" => "\r",
    "t" => "\t"
  }.freeze

  class Parser
    def initialize(source)
      raise TypeError, "expected String, got #{source.class}" unless source.is_a?(String)

      @s = source.encoding == Encoding::UTF_8 ? source : source.encode(Encoding::UTF_8)
      raise ParseError.new("invalid UTF-8 byte sequence", "", 0) unless @s.valid_encoding?

      @sc = StringScanner.new(@s)
      @line_indent = -2
      @line_start = 0
      @indent_byte = nil
      @indent_width = 0
    end

    def parse_document
      error("UTF-8 BOM is not allowed", 0) if @s.start_with?("\uFEFF")

      indent = peek_line
      error("empty LMON document", @sc.pos) if indent < 0
      error("root value must not be indented", @line_start) if indent != 0

      if dash_item?
        result = parse_block_array(0)
      else
        b = @s.getbyte(@sc.pos)
        if b == 0x22 # '"'
          first_str = parse_string
          if @s.getbyte(@sc.pos) == 0x3A # ':'
            result = parse_block_object(0, first_str)
          else
            consume_line_end(true)
            result = first_str
          end
        elsif ident_start?(b)
          ident = @sc.check(UNQUOTED_KEY_RE)
          after_byte = ident ? @s.getbyte(@sc.pos + ident.bytesize) : nil
          if ident && RESERVED_KEYS.key?(ident) && after_byte != 0x3A
            result = parse_inline_value
            consume_line_end(true)
          else
            result = parse_block_object(0, nil)
          end
        else
          result = parse_inline_value
          consume_line_end(true)
        end
      end

      trailing = peek_line
      error("unexpected trailing content after root value", @line_start) if trailing != -1
      result
    end

    private

    def error(msg, pos = @sc.pos)
      raise ParseError.new(msg, @s, pos)
    end

    def ident_start?(byte)
      return false if byte.nil?

      (byte >= 0x41 && byte <= 0x5A) || (byte >= 0x61 && byte <= 0x7A) || byte == 0x5F
    end

    def peek_line
      return @line_indent if @line_indent != -2

      len = @s.bytesize
      p = @sc.pos

      while p < len
        line_begin = p
        first = @s.getbyte(p)

        if first == 0x20 # ' '
          p += 1 while p < len && @s.getbyte(p) == 0x20
          if p >= len
            @sc.pos = p
            @line_indent = -1
            return -1
          end

          b = @s.getbyte(p)
          if b == 0x0A # \n
            p += 1
            next
          elsif b == 0x0D # \r
            p += (p + 1 < len && @s.getbyte(p + 1) == 0x0A) ? 2 : 1
            next
          elsif b == 0x09 # \t
            q = p + 1
            q += 1 while q < len && (@s.getbyte(q) == 0x20 || @s.getbyte(q) == 0x09)
            if q >= len
              @sc.pos = q
              @line_indent = -1
              return -1
            end
            qb = @s.getbyte(q)
            if qb == 0x0A
              p = q + 1
              next
            elsif qb == 0x0D
              p = (q + 1 < len && @s.getbyte(q + 1) == 0x0A) ? (q + 2) : (q + 1)
              next
            end
            error("mixed spaces and tabs in indentation", p)
          end

          count = p - line_begin
          if @indent_byte.nil?
            unless count == 2 || count == 4 || count == 6 || count == 8
              error("indentation unit must be 2, 4, 6, or 8 spaces (or 1 tab)", line_begin)
            end
            @indent_byte = 0x20
            @indent_width = count
            level = 1
          elsif @indent_byte != 0x20
            error("inconsistent indentation: document uses tabs, got spaces", line_begin)
          else
            error("indentation must be a multiple of #{@indent_width} spaces", line_begin) if (count % @indent_width) != 0
            level = count / @indent_width
          end

          @sc.pos = p
          @line_start = line_begin
          @line_indent = level
          return level
        elsif first == 0x09 # \t
          p += 1 while p < len && @s.getbyte(p) == 0x09
          if p >= len
            @sc.pos = p
            @line_indent = -1
            return -1
          end

          b = @s.getbyte(p)
          if b == 0x0A
            p += 1
            next
          elsif b == 0x0D
            p += (p + 1 < len && @s.getbyte(p + 1) == 0x0A) ? 2 : 1
            next
          elsif b == 0x20
            q = p + 1
            q += 1 while q < len && (@s.getbyte(q) == 0x20 || @s.getbyte(q) == 0x09)
            if q >= len
              @sc.pos = q
              @line_indent = -1
              return -1
            end
            qb = @s.getbyte(q)
            if qb == 0x0A
              p = q + 1
              next
            elsif qb == 0x0D
              p = (q + 1 < len && @s.getbyte(q + 1) == 0x0A) ? (q + 2) : (q + 1)
              next
            end
            error("mixed tabs and spaces in indentation", p)
          end

          count = p - line_begin
          if @indent_byte.nil?
            error("first indented block must start at 1 tab", line_begin) if count != 1
            @indent_byte = 0x09
            @indent_width = 1
          elsif @indent_byte != 0x09
            error("inconsistent indentation: document uses spaces, got tab", line_begin)
          end

          @sc.pos = p
          @line_start = line_begin
          @line_indent = count
          return count
        elsif first == 0x0A
          p += 1
          next
        elsif first == 0x0D
          p += (p + 1 < len && @s.getbyte(p + 1) == 0x0A) ? 2 : 1
          next
        end

        @sc.pos = p
        @line_start = line_begin
        @line_indent = 0
        return 0
      end

      @sc.pos = p
      @line_indent = -1
      -1
    end

    def consume_line_end(allow_eof)
      p = @sc.pos
      len = @s.bytesize
      if p >= len
        error("unexpected end of input; expected nested block", p) unless allow_eof
        @line_indent = -1
        return
      end

      b = @s.getbyte(p)
      if b == 0x0A
        @sc.pos = p + 1
        @line_indent = -2
      elsif b == 0x0D
        @sc.pos = (p + 1 < len && @s.getbyte(p + 1) == 0x0A) ? (p + 2) : (p + 1)
        @line_indent = -2
      elsif b == 0x09
        error("tabs are not allowed outside strings", p)
      elsif b == 0x20
        error("trailing spaces are not allowed", p)
      else
        error("unexpected character at end of line", p)
      end
    end

    def dash_item?
      p = @sc.pos
      return false if @s.getbyte(p) != 0x2D # '-'

      nxt = @s.getbyte(p + 1)
      nxt.nil? || nxt == 0x20 || nxt == 0x0A || nxt == 0x0D
    end

    def parse_string
      error("expected '\"'") if @s.getbyte(@sc.pos) != 0x22
      @sc.pos += 1
      out = +""

      loop do
        error("unterminated string literal") if @sc.eos?
        chunk = @sc.scan(STRING_SAFE_RE)
        out << chunk if chunk
        error("unterminated string literal") if @sc.eos?

        b = @s.getbyte(@sc.pos)
        if b == 0x22 # '"'
          @sc.pos += 1
          return out
        elsif b == 0x5C # '\\'
          @sc.pos += 1
          error("unterminated escape sequence") if @sc.eos?
          esc = @sc.getch
          mapped = ESCAPE_MAP[esc]
          if mapped
            out << mapped
          elsif esc == "u"
            hex = @sc.scan(HEX4_RE)
            error("invalid \\uXXXX escape sequence") unless hex
            code = hex.to_i(16)
            if code >= 0xD800 && code <= 0xDBFF
              error("unpaired high surrogate in string") unless @sc.scan(/\\u/)
              hex2 = @sc.scan(HEX4_RE)
              error("invalid low surrogate \\uXXXX escape") unless hex2
              low = hex2.to_i(16)
              error("expected low surrogate (DC00..DFFF)") unless low >= 0xDC00 && low <= 0xDFFF
              code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00)
            elsif code >= 0xDC00 && code <= 0xDFFF
              error("unpaired low surrogate in string", @sc.pos - 4)
            end
            out << code.chr(Encoding::UTF_8)
          else
            error("invalid escape sequence '\\#{esc}'", @sc.pos - 1)
          end
        elsif b < 0x20
          error("unescaped control character in string")
        end
      end
    end

    def parse_number
      start_pos = @sc.pos
      raw = @sc.scan(NUMBER_RE)
      error("invalid number literal") unless raw

      nxt = @s.getbyte(@sc.pos)
      if nxt && ((nxt >= 0x30 && nxt <= 0x39) || nxt == 0x2E || nxt == 0x65 || nxt == 0x45 || nxt == 0x2B || nxt == 0x2D)
        error("malformed number literal")
      end

      if raw.include?(".") || raw.include?("e") || raw.include?("E")
        val = Float(raw)
        error("numeric literal out of finite IEEE-754 range", start_pos) if val.nan? || val.infinite?
        val
      else
        raw.to_i
      end
    end

    def skip_inline_spaces
      @sc.pos += 1 while @s.getbyte(@sc.pos) == 0x20
    end

    def parse_inline_array
      @sc.pos += 1 # '['
      skip_inline_spaces
      if @s.getbyte(@sc.pos) == 0x5D # ']'
        @sc.pos += 1
        return []
      end

      items = []
      loop do
        items << parse_inline_value
        skip_inline_spaces
        error("unterminated inline array") if @sc.eos?
        b = @s.getbyte(@sc.pos)
        if b == 0x2C # ','
          @sc.pos += 1
          skip_inline_spaces
          error("trailing comma in inline array") if @s.getbyte(@sc.pos) == 0x5D
        elsif b == 0x5D # ']'
          @sc.pos += 1
          return items
        elsif b == 0x09
          error("tabs are not allowed outside strings")
        else
          error("expected ',' or ']' in inline array")
        end
      end
    end

    def parse_inline_value
      error("expected value") if @sc.eos?
      b = @s.getbyte(@sc.pos)
      case b
      when 0x22 # '"'
        parse_string
      when 0x2D, 0x30..0x39
        parse_number
      when 0x74 # 't'
        error("invalid literal; expected 'true'") unless @sc.scan(/true/)
        true
      when 0x66 # 'f'
        error("invalid literal; expected 'false'") unless @sc.scan(/false/)
        false
      when 0x6E # 'n'
        error("invalid literal; expected 'null'") unless @sc.scan(/null/)
        nil
      when 0x5B # '['
        parse_inline_array
      when 0x7B # '{'
        if @s.getbyte(@sc.pos + 1) == 0x7D
          @sc.pos += 2
          {}
        else
          error("curly-brace objects are not allowed in LMON; use indentation or '{}'")
        end
      else
        error("unexpected character when parsing value")
      end
    end

    def parse_key
      return parse_string if @s.getbyte(@sc.pos) == 0x22

      start_pos = @sc.pos
      key = @sc.scan(UNQUOTED_KEY_RE)
      error("expected object key") unless key
      error("reserved literal '#{key}' must be quoted when used as a key", start_pos) if RESERVED_KEYS.key?(key)
      key
    end

    def parse_nested_block(expected_indent)
      indent = peek_line
      error("expected indented block, got end of input") if indent < 0
      if indent != expected_indent
        error("expected indentation level #{expected_indent} (#{expected_indent * 4} spaces), got #{indent * 4} spaces", @line_start)
      end

      return parse_block_array(expected_indent) if dash_item?

      b = @s.getbyte(@sc.pos)
      if b == 0x22
        str_start = @sc.pos
        first_key = parse_string
        error("nested block cannot be a standalone scalar", str_start) if @s.getbyte(@sc.pos) != 0x3A
        return parse_block_object(expected_indent, first_key)
      elsif ident_start?(b)
        return parse_block_object(expected_indent, nil)
      end

      error("nested block must be a block object or block array")
    end

    def parse_block_object(expected_indent, first_key)
      obj = {}
      loop do
        if first_key
          key = first_key
          first_key = nil
        else
          indent = peek_line
          break if indent < expected_indent
          error("unexpected indentation inside object", @line_start) if indent > expected_indent
          error("cannot mix array items and object keys at same indentation") if dash_item?
          key = parse_key
        end

        error("expected ':' after object key") if @s.getbyte(@sc.pos) != 0x3A
        @sc.pos += 1

        if @s.getbyte(@sc.pos) == 0x20
          @sc.pos += 1
          val = parse_inline_value
          consume_line_end(true)
        else
          consume_line_end(false)
          val = parse_nested_block(expected_indent + 1)
        end
        obj[key] = val
      end
      obj
    end

    def parse_block_array(expected_indent)
      arr = []
      loop do
        indent = peek_line
        break if indent < expected_indent
        error("unexpected indentation inside array", @line_start) if indent > expected_indent
        error("cannot mix object keys and array items at same indentation") unless dash_item?
        @sc.pos += 1 # '-'

        if @s.getbyte(@sc.pos) == 0x20
          @sc.pos += 1
          elem = parse_inline_value
          consume_line_end(true)
        else
          consume_line_end(false)
          elem = parse_nested_block(expected_indent + 1)
        end
        arr << elem
      end
      arr
    end
  end

  module_function

  def loads(text)
    Parser.new(text).parse_document
  end
  alias parse loads

  def load(io)
    loads(io.read)
  end

  def escape_string(str)
    s = str.encoding == Encoding::UTF_8 ? str : str.encode(Encoding::UTF_8)
    out = +'"'
    s.each_char do |ch|
      ord = ch.ord
      case ord
      when 0x22 then out << '\"'
      when 0x5C then out << "\\\\"
      when 0x08 then out << '\b'
      when 0x0C then out << '\f'
      when 0x0A then out << '\n'
      when 0x0D then out << '\r'
      when 0x09 then out << '\t'
      else
        if ord < 0x20
          out << format('\u%04x', ord)
        else
          out << ch
        end
      end
    end
    out << '"'
  end

  def format_key(key)
    k = key.is_a?(Symbol) ? key.to_s : key
    raise TypeError, "keys must be String or Symbol, got #{key.class}" unless k.is_a?(String)

    if !RESERVED_KEYS.key?(k) && VALID_KEY_FULL_RE.match?(k)
      k
    else
      escape_string(k)
    end
  end

  def try_format_inline(val)
    case val
    when nil then "null"
    when true then "true"
    when false then "false"
    when Integer then val.to_s
    when Float
      raise ArgumentError, "NaN/Infinity are not LMON compliant" if val.nan? || val.infinite?
      val.zero? ? "0.0" : val.to_s
    when String then escape_string(val)
    when Symbol then escape_string(val.to_s)
    when Hash
      val.empty? ? "{}" : nil
    when Array
      return "[]" if val.empty?

      parts = []
      byte_len = 2
      val.each_with_index do |item, idx|
        rendered = try_format_inline(item)
        return nil if rendered.nil?

        byte_len += 2 if idx > 0
        byte_len += rendered.bytesize
        return nil if byte_len > INLINE_MAX_BYTES

        parts << rendered
      end
      "[#{parts.join(', ')}]"
    else
      raise TypeError, "Object of type #{val.class} is not LMON serializable"
    end
  end

  def normalize_indent(indent)
    if indent.is_a?(Integer) && [2, 4, 6, 8].include?(indent)
      " " * indent
    elsif indent.is_a?(String) && (indent == "\t" || ([2, 4, 6, 8].include?(indent.bytesize) && indent.delete(" ").empty?))
      indent
    else
      raise ArgumentError, "indent must be 2, 4, 6, or 8 (spaces) or '\\t'"
    end
  end

  def emit_block(val, depth, unit, lines)
    prefix = unit * depth
    if val.is_a?(Hash)
      val.each do |k, v|
        fkey = format_key(k)
        inline = try_format_inline(v)
        if inline
          lines << "#{prefix}#{fkey}: #{inline}"
        else
          lines << "#{prefix}#{fkey}:"
          emit_block(v, depth + 1, unit, lines)
        end
      end
    elsif val.is_a?(Array)
      val.each do |item|
        inline = try_format_inline(item)
        if inline
          lines << "#{prefix}- #{inline}"
        else
          lines << "#{prefix}-"
          emit_block(item, depth + 1, unit, lines)
        end
      end
    else
      raise TypeError, "Object of type #{val.class} is not LMON serializable"
    end
  end

  def dumps(obj, indent: 4)
    unit = normalize_indent(indent)
    inline = try_format_inline(obj)
    return "#{inline}\n" if inline

    lines = []
    emit_block(obj, 0, unit, lines)
    "#{lines.join("\n")}\n"
  end
  alias generate dumps

  def dump(obj, io, indent: 4)
    io.write(dumps(obj, indent: indent))
  end
end

if __FILE__ == $PROGRAM_NAME
  usage = <<~USAGE
    Usage: ruby lmon.rb [options] <file.lmon | ->
    Options:
      -c, --compact          Output compact single-line JSON
      -l, --lmon             Output canonical LMON
      --indent <2|4|6|8|tab> Set LMON output indentation (default: 4)
      --from-json            Parse input as JSON and output LMON
      --check                Validate LMON syntax only
      --test                 Run conformance test suite
      --bench [file]         Run parse benchmark
  USAGE

  if ARGV.empty? || ARGV.include?("-h") || ARGV.include?("--help")
    $stdout.write(usage)
    exit(ARGV.empty? ? 1 : 0)
  end

  if ARGV[0] == "--test"
    cases = [
      ["empty_obj", {}],
      ["empty_arr", []],
      ["scalars", { "s" => "Lemon", "i" => -42, "f" => 3.25, "t" => true, "fa" => false, "n" => nil }],
      ["root_int", -99],
      ["root_str", "hello: world"],
      ["root_bool", true],
      ["root_null", nil],
      [
        "nested_mix",
        {
          "service" => "lmon",
          "tags" => %w[a b c],
          "empty_map" => {},
          "empty_list" => [],
          "matrix" => [[1, 2], [3, 4]],
          "nodes" => [
            { "id" => 1, "meta" => { "ok" => true } },
            { "id" => 2, "meta" => {} }
          ],
          "" => "empty",
          "true" => "keyword",
          "false" => false,
          "null" => nil,
          "123" => "numeric-key",
          "-dash" => "dash-key",
          "with space" => "quoted\nline\t🍋"
        }
      ]
    ]

    cases.each do |name, expected|
      [2, 4, 6, 8, "\t"].each do |ind|
        encoded = LMON.dumps(expected, indent: ind)
        decoded = LMON.loads(encoded)
        if decoded != expected
          warn "FAIL [#{name} indent=#{ind.inspect}]: round-trip mismatch"
          exit 1
        end
      end
    end

    if LMON.loads("emoji: \"\\uD83C\\uDF4B\"\n") != { "emoji" => "🍋" }
      warn "FAIL [surrogate_pair]"
      exit 1
    end

    if LMON.loads("a:\r\n\tb: 1\rc:\r\t-\r\n\t\tx: true\r\t- 2\r") != { "a" => { "b" => 1 }, "c" => [{ "x" => true }, 2] }
      warn "FAIL [universal_newlines]"
      exit 1
    end

    invalid = [
      "",
      "   \n",
      "   bad_indent_3: 1\n",
      "a:\n bad_indent_1: 1\n",
      "a:\n     bad_indent_5: 1\n",
      "a:\n\t\ttwo_tabs_first: 1\n",
      "a:\n  b: 1\n\tc: 2\n",
      "a:\n\tb: 1\n  c: 2\n",
      "a:\n \tb: 1\n",
      "dangling:\n",
      "dangling:\nstill_root: 1\n",
      "curly: {\"a\": 1}\n",
      "true: 1\n",
      "trailing_space: 1 \n",
      "two_spaces:  1\n",
      "bad_num: 01\n",
      "trailing_comma: [1, 2,]\n",
      "mixed: 1\n- 2\n",
      "# comments are forbidden\na: 1\n"
    ]
    invalid.each do |bad|
      begin
        LMON.loads(bad)
        warn "FAIL [invalid accepted]: #{bad.inspect}"
        exit 1
      rescue LMON::ParseError
        # expected
      end
    end

    puts "Ruby LMON: all #{cases.length * 5 + invalid.length + 2} conformance tests passed."
    exit 0
  end

  if ARGV[0] == "--bench"
    sample = if ARGV[1]
               File.read(ARGV[1], encoding: "UTF-8")
             else
               nodes = (0...500).map do |i|
                 {
                   "id" => i,
                   "name" => "service-node-#{i}",
                   "active" => i.even?,
                   "latency_ms" => (1.25 * (i % 17)).round(3),
                   "tags" => %w[edge lemon v1],
                   "meta" => { "region" => "eu-central", "capacity" => 256 + i, "extra" => {} }
                 }
               end
               LMON.dumps({ "cluster" => "lmon-bench", "count" => 500, "nodes" => nodes })
             end
    size_kb = sample.bytesize / 1024.0
    iterations = [10, (10_000 / [1.0, size_kb].max).to_i].max
    t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
    iterations.times { LMON.loads(sample) }
    elapsed = Process.clock_gettime(Process::CLOCK_MONOTONIC) - t0
    mb_s = (sample.bytesize * iterations) / (1024.0 * 1024.0 * elapsed)
    puts format("Ruby LMON benchmark: %d iterations x %.1f KiB in %.3fs (%.2f MiB/s)",
                iterations, size_kb, elapsed, mb_s)
    exit 0
  end

  compact = false
  out_lmon = false
  from_json = false
  check_only = false
  indent_opt = 4
  target = nil

  idx = 0
  while idx < ARGV.length
    arg = ARGV[idx]
    case arg
    when "-c", "--compact" then compact = true
    when "-l", "--lmon" then out_lmon = true
    when "--indent"
      idx += 1
      val = ARGV[idx]
      if %w[2 4 6 8].include?(val)
        indent_opt = val.to_i
      elsif ["tab", "\\t", "\t"].include?(val)
        indent_opt = "\t"
      else
        warn "lmon error: --indent must be 2, 4, 6, 8, or tab"
        exit 2
      end
      out_lmon = true
    when "--from-json"
      from_json = true
      out_lmon = true
    when "--check" then check_only = true
    else
      if arg.start_with?("-") && arg != "-"
        warn "Unknown option: #{arg}\n#{usage}"
        exit 2
      end
      target = arg
    end
    idx += 1
  end

  if target.nil?
    warn usage
    exit 2
  end

  begin
    if target == "-"
      raw = $stdin.binmode.read.force_encoding("UTF-8")
    else
      raw = File.binread(target).force_encoding("UTF-8")
      from_json = true if target.end_with?(".json")
    end

    data = from_json ? JSON.parse(raw) : LMON.loads(raw)
    exit 0 if check_only

    if out_lmon
      $stdout.write(LMON.dumps(data, indent: indent_opt))
    elsif compact
      puts JSON.generate(data)
    else
      puts JSON.pretty_generate(data)
    end
  rescue LMON::ParseError, JSON::ParserError, SystemCallError, ArgumentError => e
    warn "lmon error: #{e.message}"
    exit 1
  end
end
