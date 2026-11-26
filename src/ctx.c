#include "luvr.h"

bool ctx_init(Ctx* ctx) {
  memset(ctx, 0, sizeof(*ctx));
  // on_error isn't set yet, so adding the runtime's symbols can't be allowed
  // to grow syms.
  ctx->syms.cap = SYM_NUM_RUNTIME;
  ctx->syms.items = (Sym*)malloc(ctx->syms.cap * sizeof(Sym));
  if (!ctx->syms.items || !lex_intern_init(ctx) || !namemap_init(&ctx->module_ids, 16)) {
    ctx_free(ctx);
    return false;
  }
  static const char* const runtime_syms[] = {
#define RUNTIME_SYM_NAME_(name) #name,
      RUNTIME_SYMS(RUNTIME_SYM_NAME_)
#undef RUNTIME_SYM_NAME_
  };
  for (u32 i = 0; i < SYM_NUM_RUNTIME; ++i) {
    ctx_sym_add(ctx, lex_intern_safe(ctx, runtime_syms[i]), NULL);
  }
  return true;
}

void ctx_free(Ctx* ctx) {
  hashtable_free(&ctx->name_table);
  free(ctx->names.items);
  free(ctx->strs.items);
  free(ctx->strbytes.items);
  free(ctx->syms.items);
  free(ctx->data.items);
  free(ctx->databytes.items);
  free(ctx->relocs.items);
  free(ctx->aggs.items);
  for (u32 i = 0; i < ctx->modules.num; ++i) {
    Module* m = ctx->modules.items[i];
    namemap_free(&m->globals);
    namemap_free(&m->imports);
    free(m->name_buf);
    if (!m->src.is_static) {
      free((void*)m->src.filename);
      free((void*)m->src.buf);
    }
    free(m);
  }
  free(ctx->modules.items);
  namemap_free(&ctx->module_ids);
  memset(ctx, 0, sizeof(*ctx));
}

SymId ctx_sym_add(Ctx* ctx, NameId name, const Module* module) {
  return ctx_method_sym_add(ctx, (NameId){SYM_NO_RECEIVER}, name, module);
}

SymId ctx_method_sym_add(Ctx* ctx, NameId receiver, NameId name, const Module* module) {
  *vec_push(ctx, &ctx->syms) = (Sym){name, receiver, module};
  return (SymId){ctx->syms.num - 1};
}

const char* ctx_err_str(Ctx* ctx, const char* fmt, ...) {
  ASSERT(ctx->err_scratch_used <= sizeof(ctx->err_scratch));
  size_t avail = sizeof(ctx->err_scratch) - ctx->err_scratch_used;
  char* s = ctx->err_scratch + ctx->err_scratch_used;
  va_list ap;
  va_start(ap, fmt);
  int len = vsnprintf(s, avail, fmt, ap);
  va_end(ap);
  // A string that didn't fit isn't kept, so err_scratch_used only ever grows
  // by a complete string and its terminator.
  if (len < 0 || (size_t)len >= avail) {
    return "<out of error string space>";
  }
  ctx->err_scratch_used += (u32)len + 1;
  return s;
}

// Whether mant * 10^exp reads back as v.
static bool float_reads_back(u64 mant, int exp, double v, bool is_f32) {
  char s[FLOAT_STR_MAX];
  snprintf(s, sizeof(s), "%" PRIu64 "e%d", mant, exp);
  double back = strtod(s, NULL);
  return is_f32 ? (float)back == (float)v : back == v;
}

void float_as_str_precise_slow(char buf[FLOAT_STR_MAX], double v, bool is_f32) {
  if (isnan(v)) {
    strcpy(buf, "nan");
    return;
  }
  if (isinf(v)) {
    strcpy(buf, v < 0 ? "-inf" : "inf");
    return;
  }
  // %.*e rounds to nearest, so the first precision whose digits read back as
  // v is the shortest. The digits are kept as mant * 10^exp.
  double mag = fabs(v);
  u64 mant = 0;
  int exp = 0;
  for (int prec = 0;; ++prec) {
    char sci[FLOAT_STR_MAX];
    snprintf(sci, sizeof(sci), "%.*e", prec, mag);
    mant = 0;
    const char* s = sci;
    for (; *s != 'e'; ++s) {
      if (*s != '.') {
        mant = mant * 10 + (u64)(*s - '0');
      }
    }
    exp = atoi(s + 1) - prec;
    if (float_reads_back(mant, exp, mag, is_f32) || prec == 16) {
      break;
    }
    // When v is a power of two, the next float down is closer than the next
    // one up, so the nearest digits can be just too low while the next digits
    // up still read back as v.
    if (strtod(sci, NULL) < mag && float_reads_back(mant + 1, exp, mag, is_f32)) {
      ++mant;
      break;
    }
  }
  char digits[FLOAT_STR_MAX];
  int num_digits = snprintf(digits, sizeof(digits), "%" PRIu64, mant);
  for (; num_digits > 1 && digits[num_digits - 1] == '0'; --num_digits) {
    ++exp;
  }
  // The decimal exponent of the first digit, as in d.ddde+XX.
  exp += num_digits - 1;
  char* out = buf;
  if (signbit(v)) {
    *out++ = '-';
  }
  // Fixed notation is used up to about the type's precision, after which it
  // would need zeros that look like digits that aren't there.
  if (exp < -4 || exp >= (is_f32 ? 7 : 16)) {
    *out++ = digits[0];
    if (num_digits > 1) {
      *out++ = '.';
      memcpy(out, digits + 1, num_digits - 1);
      out += num_digits - 1;
    }
    snprintf(out, FLOAT_STR_MAX - (out - buf), "e%c%02d", exp < 0 ? '-' : '+', abs(exp));
    return;
  }
  if (exp < 0) {
    *out++ = '0';
    *out++ = '.';
    for (int i = -1; i > exp; --i) {
      *out++ = '0';
    }
    memcpy(out, digits, num_digits);
    out += num_digits;
  } else {
    for (int i = 0; i <= exp; ++i) {
      *out++ = i < num_digits ? digits[i] : '0';
    }
    *out++ = '.';
    if (num_digits > exp + 1) {
      memcpy(out, digits + exp + 1, num_digits - exp - 1);
      out += num_digits - exp - 1;
    } else {
      *out++ = '0';
    }
  }
  *out = 0;
}

void ctx_error(Ctx* ctx, const char* file, const u8* buf, SrcLoc loc, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(ctx->err_msg, sizeof(ctx->err_msg), fmt, ap);
  va_end(ap);
  ctx->err_scratch_used = 0;
  ctx->err_file = file;
  ctx->err_buf = buf;
  ctx->err_loc = loc;
  longjmp(*ctx->on_error, 1);
}

static void src_location(const u8* buf, SrcLoc loc, u32* line, u32* col) {
  u32 ln = 1;
  const u8* bol = buf;
  for (const u8* p = buf; p < buf + loc.u; ++p) {
    if (*p == '\n') {
      ++ln;
      bol = p + 1;
    }
  }
  *line = ln;
  *col = (u32)(buf + loc.u - bol) + 1;
}

void ctx_error_location(const Ctx* ctx, u32* line, u32* col) {
  if (!ctx->err_buf) {
    *line = *col = 0;
    return;
  }
  src_location(ctx->err_buf, ctx->err_loc, line, col);
}

void ctx_error_unclosed(const LexState* ls,
                        TokenKind what,
                        TokenKind who,
                        SrcLoc who_loc,
                        const char* found) {
  u32 who_line, cur_line, col;
  src_location(ls->src.buf, who_loc, &who_line, &col);
  src_location(ls->src.buf, ls->pos.t.loc, &cur_line, &col);
  if (who_line == cur_line) {
    ctx_error(ls->ctx, ls->src.filename, ls->src.buf, ls->pos.t.loc, "Expected '%s', found %s.",
              tok_kind_name(what), found);
  }
  ctx_error(ls->ctx, ls->src.filename, ls->src.buf, ls->pos.t.loc,
            "Expected '%s' (to close '%s' at line %u), found %s.", tok_kind_name(what),
            tok_kind_name(who), who_line, found);
}

bool hashtable_init(HashTable* t, u32 cap) {
  t->slots = (HashSlot*)calloc(cap, sizeof(HashSlot));
  t->num = 0;
  t->mask = cap - 1;
  return t->slots != NULL;
}

void hashtable_free(HashTable* t) {
  free(t->slots);
  memset(t, 0, sizeof(*t));
}

void hashtable_clear(HashTable* t) {
  memset(t->slots, 0, ((size_t)t->mask + 1) * sizeof(HashSlot));
  t->num = 0;
}

static void hashtable_grow(Ctx* ctx, HashTable* t) {
  u32 mask = t->mask * 2 + 1;
  // Testing whether each slot is empty is unpredictable at half load. Instead,
  // empty slots are all copied to an extra slot after the end, which stays
  // empty.
  HashSlot* grown = (HashSlot*)calloc((size_t)mask + 2, sizeof(HashSlot));
  if (!grown) {
    ctx_error(ctx, NULL, NULL, (SrcLoc){0}, "Out of memory.");
  }
  for (u32 j = 0; j <= t->mask; ++j) {
    HashSlot s = t->slots[j];
    u32 i = s.tag != 0 ? s.tag & mask : mask + 1;
    while (grown[i].tag != 0) {
      i = (i + 1) & mask;
    }
    grown[i] = s;
  }
  free(t->slots);
  t->slots = grown;
  t->mask = mask;
}

void hashtable_insert(Ctx* ctx, HashTable* t, HashSlot* s, u32 tag, u32 val) {
  ASSERT(s->tag == 0 && tag != 0);
  s->tag = tag;
  s->val = val;
  if (++t->num * 2 > t->mask + 1) {
    hashtable_grow(ctx, t);
  }
}

void namemap_set(Ctx* ctx, NameMap* m, NameId name, u32 val) {
  u32 tag = name.u + 1;
  HashSlot* s = hashtable_first(&m->table, tag);
  if (s->tag != 0) {
    s->val = val;
    return;
  }
  hashtable_insert(ctx, &m->table, s, tag, val);
}

void* vec_grow_(Ctx* ctx, void* v, u32* cap, size_t need, size_t elem_size) {
  size_t c = *cap ? (size_t)*cap * 2 : 16;
  while (c < need) {
    c *= 2;
  }
  if (c > UINT32_MAX) {
    if (need > UINT32_MAX) {
      ctx_error(ctx, NULL, NULL, (SrcLoc){0}, "Out of memory.");
    }
    c = UINT32_MAX;
  }
  v = realloc(v, c * elem_size);
  if (!v) {
    ctx_error(ctx, NULL, NULL, (SrcLoc){0}, "Out of memory.");
  }
  *cap = (u32)c;
  return v;
}
