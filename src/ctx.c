#include "luvr.h"

bool ctx_init(Ctx* ctx) {
  memset(ctx, 0, sizeof(*ctx));
  // on_error isn't set yet, so adding the runtime's symbols can't be allowed
  // to grow syms.
  ctx->syms.cap = SYM_NUM_RUNTIME;
  ctx->syms.v = (NameId*)malloc(ctx->syms.cap * sizeof(NameId));
  if (!ctx->syms.v || !lex_intern_init(ctx) || !namemap_init(&ctx->module_ids, 16)) {
    ctx_free(ctx);
    return false;
  }
  static const char* const runtime_syms[] = {
#define RUNTIME_SYM_NAME_(name) #name,
      RUNTIME_SYMS(RUNTIME_SYM_NAME_)
#undef RUNTIME_SYM_NAME_
  };
  for (u32 i = 0; i < SYM_NUM_RUNTIME; ++i) {
    ctx_sym_add(ctx, lex_intern_safe(ctx, runtime_syms[i]));
  }
  return true;
}

void ctx_free(Ctx* ctx) {
  hashtable_free(&ctx->name_table);
  free(ctx->names.v);
  free(ctx->strs.v);
  free(ctx->strbytes.v);
  free(ctx->syms.v);
  free(ctx->data.v);
  free(ctx->databytes.v);
  for (u32 i = 0; i < ctx->modules.num; ++i) {
    Module* m = ctx->modules.v[i];
    namemap_free(&m->globals);
    namemap_free(&m->imports);
    free(m->name_buf);
    free((void*)m->src.filename);
    free((void*)m->src.buf);
    free(m);
  }
  free(ctx->modules.v);
  namemap_free(&ctx->module_ids);
  memset(ctx, 0, sizeof(*ctx));
}

u32 ctx_sym_add(Ctx* ctx, NameId name) {
  *vec_push(ctx, &ctx->syms) = name;
  return ctx->syms.num - 1;
}

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

void ctx_error_unclosed(const LexState* ls,
                        TokenKind what,
                        TokenKind who,
                        u32 who_offset,
                        const char* found) {
  u32 who_line, cur_line, col;
  src_location(ls->src.buf, who_offset, &who_line, &col);
  src_location(ls->src.buf, ls->pos.t.offset, &cur_line, &col);
  if (who_line == cur_line) {
    ctx_error(ls->ctx, ls->src.filename, ls->src.buf, ls->pos.t.offset, "Expected '%s', found %s.",
              tok_kind_name(what), found);
  }
  ctx_error(ls->ctx, ls->src.filename, ls->src.buf, ls->pos.t.offset,
            "Expected '%s' (to close '%s' at line %u), found %s.", tok_kind_name(what),
            tok_kind_name(who), who_line, found);
}

bool hashtable_init(HashTable* t, u32 cap) {
  t->v = (HashSlot*)calloc(cap, sizeof(HashSlot));
  t->num = 0;
  t->mask = cap - 1;
  return t->v != NULL;
}

void hashtable_free(HashTable* t) {
  free(t->v);
  memset(t, 0, sizeof(*t));
}

void hashtable_clear(HashTable* t) {
  memset(t->v, 0, ((size_t)t->mask + 1) * sizeof(HashSlot));
  t->num = 0;
}

static void hashtable_grow(Ctx* ctx, HashTable* t) {
  u32 mask = t->mask * 2 + 1;
  // Testing whether each slot is empty is unpredictable at half load. Instead,
  // empty slots are all copied to an extra slot after the end, which stays
  // empty.
  HashSlot* v = (HashSlot*)calloc((size_t)mask + 2, sizeof(HashSlot));
  if (!v) {
    ctx_error(ctx, NULL, NULL, 0, "Out of memory.");
  }
  for (u32 j = 0; j <= t->mask; ++j) {
    HashSlot s = t->v[j];
    u32 i = s.tag != 0 ? s.tag & mask : mask + 1;
    while (v[i].tag != 0) {
      i = (i + 1) & mask;
    }
    v[i] = s;
  }
  free(t->v);
  t->v = v;
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
  u32 tag = name.idx + 1;
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
      ctx_error(ctx, NULL, NULL, 0, "Out of memory.");
    }
    c = UINT32_MAX;
  }
  v = realloc(v, c * elem_size);
  if (!v) {
    ctx_error(ctx, NULL, NULL, 0, "Out of memory.");
  }
  *cap = (u32)c;
  return v;
}
