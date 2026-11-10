#include "luvr.h"

// Character classes

enum {
  C_IDSTART = 1,  // [A-Za-z_]
  C_IDCHAR = 2,   // [A-Za-z0-9_]
  C_DIGIT = 4,    // [0-9]
  C_UPPER = 8,    // [A-Z]
  C_LOWER = 16,   // [a-z]
  C_HEX = 32,     // [0-9A-Fa-f]
};

// Bytes >= 0x80 are 0 because identifiers are ASCII-only. UTF-8 is only
// allowed in strings and comments.
static const u8 cclass[256] = {
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,   // 00-0f
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,   // 10-1f
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,   // 20-2f
    38, 38, 38, 38, 38, 38, 38, 38, 38, 38, 0,  0,  0,  0,  0,  0,   // 30-3f
    0,  43, 43, 43, 43, 43, 43, 11, 11, 11, 11, 11, 11, 11, 11, 11,  // 40-4f
    11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 0,  0,  0,  0,  3,   // 50-5f
    0,  51, 51, 51, 51, 51, 51, 19, 19, 19, 19, 19, 19, 19, 19, 19,  // 60-6f
    19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 0,  0,  0,  0,  0,   // 70-7f
};

#define IS(c, cls) (cclass[(u8)(c)] & (cls))

// Token names

static const char* const token_names[TK_NUM_KINDS] = {
#define LEX_STR_(name, str) str,
    LEX_TOKENS(LEX_STR_, LEX_STR_, LEX_STR_)
#undef LEX_STR_
};

const char* tok_kind_name(TokenKind kind) {
  return (kind >= 0 && kind < TK_NUM_KINDS) ? token_names[kind] : "<?>";
}

// Errors

void ctx_error(Ctx* ctx, const char* file, const u8* buf, u32 offset, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(ctx->err_msg, sizeof(ctx->err_msg), fmt, ap);
  va_end(ap);
  ctx->err_file = file;
  ctx->err_buf = buf;
  ctx->err_offset = offset;
  longjmp(*ctx->on_error, 1);
}

static void src_location(const u8* buf, u32 offset, u32* line, u32* col) {
  u32 ln = 1;
  const u8* bol = buf;
  for (const u8* p = buf; p < buf + offset; ++p) {
    if (*p == '\n') {
      ++ln;
      bol = p + 1;
    }
  }
  *line = ln;
  *col = (u32)(buf + offset - bol) + 1;
}

void ctx_error_location(const Ctx* ctx, u32* line, u32* col) {
  if (!ctx->err_buf) {
    *line = *col = 0;
    return;
  }
  src_location(ctx->err_buf, ctx->err_offset, line, col);
}

#define lex_error(ls, at, ...) \
  ctx_error((ls)->ctx, (ls)->filename, (ls)->buf, (u32)((at) - (ls)->buf), __VA_ARGS__)

void lex_error_unclosed(LexState* ls,
                        TokenKind what,
                        TokenKind who,
                        u32 who_offset,
                        const char* found) {
  u32 who_line, cur_line, col;
  src_location(ls->buf, who_offset, &who_line, &col);
  src_location(ls->buf, ls->t.offset, &cur_line, &col);
  if (who_line == cur_line) {
    ctx_error(ls->ctx, ls->filename, ls->buf, ls->t.offset, "Expected '%s', found %s.",
              tok_kind_name(what), found);
  }
  ctx_error(ls->ctx, ls->filename, ls->buf, ls->t.offset,
            "Expected '%s' (to close '%s' at line %u), found %s.", tok_kind_name(what),
            tok_kind_name(who), who_line, found);
}

void* ctx_grow(Ctx* ctx, void* p, size_t bytes) {
  void* q = realloc(p, bytes);
  if (!q) {
    ctx_error(ctx, NULL, NULL, 0, "Out of memory.");
  }
  return q;
}

// Interning. The table is intentionally simple so that it's a baseline to
// benchmark alternatives against.

static u64 mix64(u64 h) {
  h ^= h >> 32;
  h *= 0xd6e8feb86659fd93ull;
  h ^= h >> 32;
  return h;
}

// Reads up to 7 bytes past s+len, which is safe for names in the source
// because of LEX_PADDING. The extra bytes are masked off, so the result only
// depends on s[0..len).
static u64 hash_ident(const u8* s, u32 len) {
  u64 h = 0x9e3779b97f4a7c15ull ^ len;
  while (len >= 8) {
    u64 w;
    memcpy(&w, s, 8);
    h = (h ^ w) * 0xff51afd7ed558ccdull;
    s += 8;
    len -= 8;
  }
  if (len) {
    u64 w;
    memcpy(&w, s, 8);
    w &= ~0ull >> (64 - 8 * len);
    h = (h ^ w) * 0xc4ceb9fe1a85ec53ull;
  }
  return mix64(h);
}

// Keyword spellings aren't padded, so they're copied into a padded buffer.
static u64 hash_ident_safe(const char* s, u32 len) {
  u8 tmp[64 + 8] = {0};
  memcpy(tmp, s, len);
  return hash_ident(tmp, len);
}

// Types are recognized lexically, so the parser never needs a symbol table
// to tell `Stuff s` from `stuff(s)`. After any leading underscores, an
// identifier is a type if it's a single capital letter, or starts with a
// capital and contains a lowercase letter. Everything else (lowercase first
// letter, ALL_CAPS constants, only underscores) is a name. The kind is stored
// in the Name, so this is only done once per distinct identifier.
static TokenKind classify_ident(const u8* s, u32 len, bool* is_const) {
  *is_const = false;
  u32 i = 0;
  while (i < len && s[i] == '_') {
    ++i;
  }
  if (i == len || !IS(s[i], C_UPPER)) {
    return TK_NAME;
  }
  if (i + 1 == len) {
    return TK_TYPENAME;
  }
  for (u32 j = i + 1; j < len; ++j) {
    if (IS(s[j], C_LOWER)) {
      return TK_TYPENAME;
    }
  }
  *is_const = true;
  return TK_NAME;
}

static void intern_rehash(Ctx* ctx) {
  u32 new_size = (ctx->slot_mask + 1) * 2;
  u64* slots = (u64*)calloc(new_size, sizeof(u64));
  if (!slots) {
    ctx_error(ctx, NULL, NULL, 0, "Out of memory.");
  }
  u32 mask = new_size - 1;
  for (u32 i = 0; i < ctx->num_names; ++i) {
    u64 h = ctx->names[i].hash;
    u32 pos = (u32)h & mask;
    while (slots[pos]) {
      pos = (pos + 1) & mask;
    }
    slots[pos] = (h & 0xffffffff00000000ull) | (u64)(i + 1);
  }
  free(ctx->slots);
  ctx->slots = slots;
  ctx->slot_mask = mask;
}

static u32 intern(Ctx* ctx, const u8* s, u32 len, u64 h, TokenKind kind_if_new) {
  u64 tag = h & 0xffffffff00000000ull;
  u32 mask = ctx->slot_mask;
  u32 pos = (u32)h & mask;
  for (;;) {
    u64 slot = ctx->slots[pos];
    if (!slot) {
      break;
    }
    if ((slot & 0xffffffff00000000ull) == tag) {
      u32 idx = (u32)slot - 1;
      const Name* n = &ctx->names[idx];
      if (n->len == len && memcmp(n->str, s, len) == 0) {
        return idx;
      }
    }
    pos = (pos + 1) & mask;
  }
  if (ctx->num_names == ctx->cap_names) {
    ctx->cap_names *= 2;
    ctx->names = (Name*)ctx_grow(ctx, ctx->names, ctx->cap_names * sizeof(Name));
  }
  u32 idx = ctx->num_names++;
  Name* n = &ctx->names[idx];
  n->str = s;
  n->len = len;
  n->is_const = false;
  n->global = 0;
  n->kind = kind_if_new ? kind_if_new : classify_ident(s, len, &n->is_const);
  n->hash = h;
  n->binding = NULL;
  ctx->slots[pos] = tag | (u64)(idx + 1);
  if (ctx->num_names * 2 > mask + 1) {
    intern_rehash(ctx);
  }
  return idx;
}

// Context

bool ctx_init(Ctx* ctx) {
  memset(ctx, 0, sizeof(*ctx));
  ctx->slot_mask = (1u << 12) - 1;
  ctx->slots = (u64*)calloc(ctx->slot_mask + 1, sizeof(u64));
  ctx->cap_names = 1u << 10;
  ctx->names = (Name*)malloc(ctx->cap_names * sizeof(Name));
  if (!ctx->slots || !ctx->names) {
    ctx_free(ctx);
    return false;
  }
  // Keywords and builtin types are in the same table, so one lookup is
  // enough to get both whether it's reserved and the Name. The table
  // is initialized with enough capacity that this can't rehash or grow.
  static const struct {
    const char* s;
    u8 kind;
  } kws[] = {
#define LEX_NONE_(name, str)
#define LEX_KW_(name, str) {str, TK_##name},
      LEX_TOKENS(LEX_NONE_, LEX_KW_, LEX_KW_)
#undef LEX_NONE_
#undef LEX_KW_
  };
  for (size_t i = 0; i < sizeof(kws) / sizeof(kws[0]); ++i) {
    u32 len = (u32)strlen(kws[i].s);
    intern(ctx, (const u8*)kws[i].s, len, hash_ident_safe(kws[i].s, len), kws[i].kind);
  }
  return true;
}

void ctx_free(Ctx* ctx) {
  free(ctx->slots);
  free(ctx->names);
  free(ctx->strs);
  free(ctx->strbytes);
  memset(ctx, 0, sizeof(*ctx));
}

// Decoded string storage

static void strbytes_append(Ctx* ctx, const u8* s, size_t n) {
  if ((size_t)ctx->num_strbytes + n > ctx->cap_strbytes) {
    size_t cap = ctx->cap_strbytes ? ctx->cap_strbytes : 4096;
    while (cap < (size_t)ctx->num_strbytes + n) {
      cap *= 2;
    }
    if (cap > 0xffffffffu) {
      ctx_error(ctx, NULL, NULL, 0, "Too much string literal data.");
    }
    ctx->strbytes = (u8*)ctx_grow(ctx, ctx->strbytes, cap);
    ctx->cap_strbytes = (u32)cap;
  }
  memcpy(ctx->strbytes + ctx->num_strbytes, s, n);
  ctx->num_strbytes += (u32)n;
}

static u32 strs_add(Ctx* ctx, u32 off, u32 len) {
  if (ctx->num_strs == ctx->cap_strs) {
    ctx->cap_strs = ctx->cap_strs ? ctx->cap_strs * 2 : 256;
    ctx->strs = (StrRef*)ctx_grow(ctx, ctx->strs, ctx->cap_strs * sizeof(StrRef));
  }
  ctx->strs[ctx->num_strs].off = off;
  ctx->strs[ctx->num_strs].len = len;
  return ctx->num_strs++;
}

const u8* lex_str(const LexState* ls, const Token* t, u32* len) {
  if (t->flags & TF_ESCAPED) {
    const StrRef* r = &ls->ctx->strs[t->v.str];
    *len = r->len;
    return ls->ctx->strbytes + r->off;
  }
  *len = t->v.len;
  return ls->buf + t->offset + 1;
}

// Layout. Each logical line boundary is a NEWLINE if the indent is unchanged,
// an INDENT if it's one level deeper, or one DEDENT per level if it's
// shallower. Indentation is fixed at 4 spaces. Newlines inside brackets are
// whitespace.

// p is at the start of a line. Lines that are blank or only a comment are
// skipped, regardless of their indentation. Returns the first non-space byte
// of the next line with content (or end), with the start of that line in
// *bol.
static const u8* next_content_line(LexState* ls, const u8* p, const u8** bol) {
  for (;;) {
    const u8* q = p;
    while (*q == ' ') {
      ++q;
    }
    if (*q == '\n') {
      p = q + 1;
      continue;
    }
    if (*q == '\r' && q[1] == '\n') {
      p = q + 2;
      continue;
    }
    if (*q == '#') {
      const u8* nl = (const u8*)memchr(q, '\n', (size_t)(ls->end - q));
      if (!nl) {
        *bol = ls->end;
        return ls->end;
      }
      p = nl + 1;
      continue;
    }
    *bol = p;
    return q;
  }
}

static void lex_eof(LexState* ls, Token* t) {
  t->offset = (u32)(ls->end - ls->buf);
  ls->p = ls->end;
  if (ls->indent) {
    ls->pending_dedents = (u16)(ls->indent - 1);
    ls->indent = 0;
    t->kind = TK_DEDENT;
  } else {
    t->kind = TK_EOS;
  }
}

// nl points at a '\n' outside brackets.
static void lex_layout(LexState* ls, Token* t, const u8* nl) {
  const u8* bol;
  const u8* q = next_content_line(ls, nl + 1, &bol);
  if (q >= ls->end) {
    lex_eof(ls, t);
    return;
  }
  if (*q == '\t') {
    lex_error(ls, q, "Indentation must use spaces, not tabs.");
  }
  u32 n = (u32)(q - bol);
  if (n & 3) {
    lex_error(ls, q, "Indentation must be a multiple of 4 spaces.");
  }
  u32 level = n >> 2;
  ls->p = q;
  if (level == ls->indent) {
    t->kind = TK_NEWLINE;
    t->offset = (u32)(nl - ls->buf);
  } else if (level == ls->indent + 1u) {
    ls->indent = (u16)level;
    t->kind = TK_INDENT;
    t->offset = (u32)(q - ls->buf);
  } else if (level > ls->indent) {
    lex_error(ls, q, "Unexpected indent of more than one level.");
  } else {
    ls->pending_dedents = (u16)(ls->indent - level - 1);
    ls->indent = (u16)level;
    t->kind = TK_DEDENT;
    t->offset = (u32)(q - ls->buf);
  }
}

// Numbers

static NumSuffix parse_suffix(const u8* s, u32 len) {
  if (len == 2) {
    if (s[0] == 'i' && s[1] == '8') {
      return SUF_I8;
    }
    if (s[0] == 'u' && s[1] == '8') {
      return SUF_U8;
    }
    return SUF_NONE;
  }
  if (len != 3) {
    return SUF_NONE;
  }
  static const struct {
    char s[4];
    u8 suf;
  } sufs[] = {
      {"i16", SUF_I16}, {"i32", SUF_I32}, {"i64", SUF_I64}, {"u16", SUF_U16},
      {"u32", SUF_U32}, {"u64", SUF_U64}, {"f32", SUF_F32}, {"f64", SUF_F64},
  };
  for (size_t i = 0; i < sizeof(sufs) / sizeof(sufs[0]); ++i) {
    if (memcmp(s, sufs[i].s, 3) == 0) {
      return sufs[i].suf;
    }
  }
  return SUF_NONE;
}

static int digit_value(u8 c) {
  if (c <= '9') {
    return c - '0';
  }
  return (c | 0x20) - 'a' + 10;
}

// Range checks for integer suffixes are done in the parser, so that unary
// minus can be folded first (-128i8). The lexer only rejects values that don't
// fit in a u64.
static const u8* lex_number(LexState* ls, Token* t, const u8* p) {
  const u8* start = p;
  u64 v = 0;
  bool overflow = false;
  bool is_float = false;
  t->offset = (u32)(p - ls->buf);

  if (p[0] == '0' && (p[1] == 'x' || p[1] == 'o' || p[1] == 'b')) {
    u32 base = p[1] == 'x' ? 16 : p[1] == 'o' ? 8 : 2;
    p += 2;
    int ndigits = 0;
    for (;; ++p) {
      u8 c = *p;
      if (c == '_') {
        continue;
      }
      if (!IS(c, C_HEX)) {
        break;
      }
      u32 d = (u32)digit_value(c);
      if (d >= base) {
        break;
      }
      if (v > (UINT64_MAX - d) / base) {
        overflow = true;
      }
      v = v * base + d;
      ++ndigits;
    }
    if (!ndigits) {
      lex_error(ls, start, "Missing digits after '0%c'.", start[1]);
    }
  } else {
    for (;; ++p) {
      u8 c = *p;
      if (c == '_') {
        continue;
      }
      if (!IS(c, C_DIGIT)) {
        break;
      }
      u32 d = c - '0';
      if (v > (UINT64_MAX - d) / 10) {
        overflow = true;
      }
      v = v * 10 + d;
    }
    // A float requires digits on both sides of the '.', so `1.foo()` is a
    // method call on an int, and `1.` and `.5` aren't floats.
    if (*p == '.' && IS(p[1], C_DIGIT)) {
      is_float = true;
      ++p;
      while (IS(*p, C_DIGIT) || *p == '_') {
        ++p;
      }
    }
    if ((*p == 'e' || *p == 'E') &&
        (IS(p[1], C_DIGIT) || ((p[1] == '+' || p[1] == '-') && IS(p[2], C_DIGIT)))) {
      is_float = true;
      p += 2;
      while (IS(*p, C_DIGIT) || *p == '_') {
        ++p;
      }
    }
  }

  const u8* num_end = p;
  NumSuffix suf = SUF_NONE;
  if (IS(*p, C_IDCHAR)) {
    const u8* s = p;
    while (IS(*p, C_IDCHAR)) {
      ++p;
    }
    suf = parse_suffix(s, (u32)(p - s));
    bool hex_like = start[0] == '0' && (start[1] == 'x' || start[1] == 'o' || start[1] == 'b');
    if (suf == SUF_NONE || (is_float && suf < SUF_F32) || (hex_like && suf >= SUF_F32)) {
      lex_error(ls, s, "Invalid suffix '%.*s' on number.", (int)(p - s), (const char*)s);
    }
    if (suf >= SUF_F32) {
      is_float = true;  // 1f32 is a float
    }
  }

  t->flags = suf;
  if (is_float) {
    // strtod doesn't accept '_' or the suffix, so they're dropped in a copy.
    // strtod uses the C locale's decimal point, so the host must not call
    // setlocale(LC_NUMERIC).
    char tmp[128];
    size_t n = 0;
    for (const u8* q = start; q < num_end; ++q) {
      if (*q == '_') {
        continue;
      }
      if (n >= sizeof(tmp) - 1) {
        lex_error(ls, start, "Float literal is too long.");
      }
      tmp[n++] = (char)*q;
    }
    tmp[n] = 0;
    t->kind = TK_FLOATLIT;
    t->v.f = strtod(tmp, NULL);
  } else {
    if (overflow) {
      lex_error(ls, start, "Integer literal is too large for 64 bits.");
    }
    t->kind = TK_INTLIT;
    t->v.i = v;
  }
  return p;
}

// Strings: "..." with escapes and \( ) interpolation

static void utf8_append(Ctx* ctx, u32 cp) {
  u8 b[4];
  size_t n;
  if (cp < 0x80) {
    b[0] = (u8)cp;
    n = 1;
  } else if (cp < 0x800) {
    b[0] = (u8)(0xc0 | (cp >> 6));
    b[1] = (u8)(0x80 | (cp & 0x3f));
    n = 2;
  } else if (cp < 0x10000) {
    b[0] = (u8)(0xe0 | (cp >> 12));
    b[1] = (u8)(0x80 | ((cp >> 6) & 0x3f));
    b[2] = (u8)(0x80 | (cp & 0x3f));
    n = 3;
  } else {
    b[0] = (u8)(0xf0 | (cp >> 18));
    b[1] = (u8)(0x80 | ((cp >> 12) & 0x3f));
    b[2] = (u8)(0x80 | ((cp >> 6) & 0x3f));
    b[3] = (u8)(0x80 | (cp & 0x3f));
    n = 4;
  }
  strbytes_append(ctx, b, n);
}

// q points at a backslash that is not `\(`. Appends the decoded escape and
// returns the position after it.
static const u8* decode_escape(LexState* ls, const u8* q) {
  Ctx* ctx = ls->ctx;
  u8 c = q[1];
  u8 out;
  switch (c) {
    case 'n':
      out = '\n';
      break;
    case 't':
      out = '\t';
      break;
    case 'r':
      out = '\r';
      break;
    case '\\':
      out = '\\';
      break;
    case '"':
      out = '"';
      break;
    case '0':
      out = 0;
      break;
    case 'x': {
      if (!IS(q[2], C_HEX) || !IS(q[3], C_HEX)) {
        lex_error(ls, q, "Escape \\x needs two hex digits.");
      }
      out = (u8)(digit_value(q[2]) * 16 + digit_value(q[3]));
      strbytes_append(ctx, &out, 1);
      return q + 4;
    }
    case 'u': {
      if (q[2] != '{') {
        lex_error(ls, q, "Expected '{' after \\u.");
      }
      const u8* r = q + 3;
      u32 cp = 0;
      int n = 0;
      while (IS(*r, C_HEX)) {
        cp = cp * 16 + (u32)digit_value(*r++);
        if (++n > 6) {
          lex_error(ls, q, "Escape \\u{...} has too many digits.");
        }
      }
      if (*r != '}' || n == 0) {
        lex_error(ls, q, "Malformed \\u{...} escape.");
      }
      if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) {
        lex_error(ls, q, "Escape \\u{%x} is not a valid code point.", cp);
      }
      utf8_append(ctx, cp);
      return r + 1;
    }
    default:
      lex_error(ls, q, "Invalid escape sequence '\\%c'.", c >= 0x20 && c < 0x7f ? c : '?');
  }
  strbytes_append(ctx, &out, 1);
  return q + 2;
}

// delim is the opening '"' (cont is false) or the ')' that closed a \( (cont
// is true). Reads up to the closing '"' or the next \(. Content without escapes
// isn't copied, and is referenced as a slice of the source.
static const u8* lex_string(LexState* ls, Token* t, const u8* delim, bool cont) {
  Ctx* ctx = ls->ctx;
  const u8* s = delim + 1;
  const u8* q = s;
  const u8* run = s;  // start of not-yet-copied bytes (escaped mode)
  bool escaped = false;
  u32 bstart = 0;
  t->offset = (u32)(delim - ls->buf);
  for (;;) {
    u8 c = *q;
    if (c == '"' || (c == '\\' && q[1] == '(')) {
      break;
    }
    if (c == '\\') {
      if (!escaped) {
        escaped = true;
        bstart = ctx->num_strbytes;
      }
      strbytes_append(ctx, run, (size_t)(q - run));
      q = decode_escape(ls, q);
      run = q;
      continue;
    }
    if (c == '\n' || (c == 0 && q >= ls->end)) {
      lex_error(ls, delim,
                cont ? "Unterminated string after interpolation." : "Unterminated string.");
    }
    ++q;
  }
  if (escaped) {
    strbytes_append(ctx, run, (size_t)(q - run));
    t->flags = TF_ESCAPED;
    t->v.str = strs_add(ctx, bstart, ctx->num_strbytes - bstart);
  } else {
    t->v.len = (u32)(q - s);
  }
  if (*q == '"') {
    t->kind = cont ? TK_STRING_END : TK_STRING;
    return q + 1;
  }
  // At a \(, so the string continues after the matching ')'.
  if (ls->interp_sp == LEX_MAX_INTERP) {
    lex_error(ls, q, "String interpolation is nested too deeply.");
  }
  ls->interp_stack[ls->interp_sp++] = ls->bracket_depth;
  ++ls->bracket_depth;
  t->kind = cont ? TK_STRING_MID : TK_STRING_BEGIN;
  return q + 2;
}

// Main lexer

void lex_init(LexState* ls, Ctx* ctx, const char* filename, const u8* buf, size_t len) {
  memset(ls, 0, sizeof(*ls));
  ls->ctx = ctx;
  ls->filename = filename;
  ls->buf = buf;
  ls->end = buf + len;
  if (len >= 0xfffffff0u) {
    ctx_error(ctx, filename, NULL, 0, "File is larger than the 4 GB limit.");
  }
  const u8* bol;
  const u8* q = next_content_line(ls, buf, &bol);
  if (q < ls->end && q != bol) {
    lex_error(ls, q, "Unexpected indent on the first line.");
  }
  ls->p = q;
}

#define TOK(k, n)                   \
  do {                              \
    t->kind = (k);                  \
    t->offset = (u32)(p - ls->buf); \
    ls->p = p + (n);                \
    return;                         \
  } while (0)

#define OPEN(k)                                            \
  do {                                                     \
    if (++ls->bracket_depth == 0)                          \
      lex_error(ls, p, "Brackets are nested too deeply."); \
    TOK(k, 1);                                             \
  } while (0)

#define CLOSE(k)           \
  do {                     \
    if (ls->bracket_depth) \
      --ls->bracket_depth; \
    TOK(k, 1);             \
  } while (0)

void lex_next(LexState* ls) {
  Token* t = &ls->t;
  t->flags = 0;
  if (ls->pending_dedents) {
    --ls->pending_dedents;
    t->kind = TK_DEDENT;
    return;
  }
  const u8* p = ls->p;
  for (;;) {
    switch (*p) {
      case ' ':
      case '\r':
        ++p;
        continue;

      case '\t':
        lex_error(ls, p, "Tab characters are only allowed in strings and comments.");

      case '#': {
        // The '\n' isn't skipped so that it's still seen as a line boundary.
        const u8* nl = (const u8*)memchr(p, '\n', (size_t)(ls->end - p));
        p = nl ? nl : ls->end;
        continue;
      }

      case '\n':
        if (ls->interp_sp) {
          lex_error(ls, p, "Newlines aren't allowed inside string interpolation.");
        }
        if (ls->bracket_depth) {
          ++p;
          continue;
        }
        lex_layout(ls, t, p);
        return;

      case 0:
        if (p >= ls->end) {
          lex_eof(ls, t);
          return;
        }
        lex_error(ls, p, "Source contains a NUL byte.");

      case '(':
        OPEN(TK_LPAREN);
      case '[':
        OPEN(TK_LSQUARE);
      case '{':
        OPEN(TK_LBRACE);
      case ']':
        CLOSE(TK_RSQUARE);
      case '}':
        CLOSE(TK_RBRACE);
      case ')':
        if (ls->interp_sp && ls->bracket_depth == ls->interp_stack[ls->interp_sp - 1] + 1u) {
          --ls->bracket_depth;
          --ls->interp_sp;
          ls->p = lex_string(ls, t, p, /*cont=*/true);
          return;
        }
        CLOSE(TK_RPAREN);

      case ',':
        TOK(TK_COMMA, 1);
      case ':':
        TOK(TK_COLON, 1);
      case '~':
        TOK(TK_TILDE, 1);
      case '.':
        if (IS(p[1], C_DIGIT)) {
          lex_error(ls, p, "Float literal needs a digit before '.'.");
        }
        TOK(TK_DOT, 1);

      case '+':
        if (p[1] == '=') {
          TOK(TK_PLUSEQ, 2);
        }
        TOK(TK_PLUS, 1);
      case '-':
        if (p[1] == '=') {
          TOK(TK_MINUSEQ, 2);
        }
        TOK(TK_MINUS, 1);
      case '*':
        if (p[1] == '=') {
          TOK(TK_STAREQ, 2);
        }
        TOK(TK_STAR, 1);
      case '/':
        if (p[1] == '=') {
          TOK(TK_SLASHEQ, 2);
        }
        TOK(TK_SLASH, 1);
      case '%':
        if (p[1] == '=') {
          TOK(TK_PERCENTEQ, 2);
        }
        TOK(TK_PERCENT, 1);
      case '&':
        if (p[1] == '=') {
          TOK(TK_AMPEQ, 2);
        }
        TOK(TK_AMP, 1);
      case '|':
        if (p[1] == '=') {
          TOK(TK_PIPEEQ, 2);
        }
        TOK(TK_PIPE, 1);
      case '^':
        if (p[1] == '=') {
          TOK(TK_CARETEQ, 2);
        }
        TOK(TK_CARET, 1);
      case '=':
        if (p[1] == '=') {
          TOK(TK_EQEQ, 2);
        }
        TOK(TK_EQ, 1);
      case '!':
        if (p[1] == '=') {
          TOK(TK_NE, 2);
        }
        lex_error(ls, p, "Unexpected '!'. Use 'not' for logical negation.");
      case '<':
        if (p[1] == '<') {
          if (p[2] == '=') {
            TOK(TK_SHLEQ, 3);
          }
          TOK(TK_SHL, 2);
        }
        if (p[1] == '=') {
          TOK(TK_LE, 2);
        }
        TOK(TK_LT, 1);
      case '>':
        if (p[1] == '>') {
          if (p[2] == '=') {
            TOK(TK_SHREQ, 3);
          }
          TOK(TK_SHR, 2);
        }
        if (p[1] == '=') {
          TOK(TK_GE, 2);
        }
        TOK(TK_GT, 1);

      case '"':
        ls->p = lex_string(ls, t, p, /*cont=*/false);
        return;

      case '0':
      case '1':
      case '2':
      case '3':
      case '4':
      case '5':
      case '6':
      case '7':
      case '8':
      case '9':
        ls->p = lex_number(ls, t, p);
        return;

      default:
        if (IS(*p, C_IDSTART)) {  // identifier or keyword
          const u8* s = p;
          do {
            ++p;
          } while (IS(*p, C_IDCHAR));
          u32 len = (u32)(p - s);
          u32 idx = intern(ls->ctx, s, len, hash_ident(s, len), 0);
          t->kind = ls->ctx->names[idx].kind;
          t->offset = (u32)(s - ls->buf);
          t->v.name = (NameId){idx};
          ls->p = p;
          return;
        }
        if (*p >= 0x20 && *p < 0x7f) {
          lex_error(ls, p, "Unexpected character '%c'.", *p);
        }
        lex_error(ls, p, "Unexpected byte 0x%02x.", *p);
    }
  }
}
