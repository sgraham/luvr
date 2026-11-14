#include "luvr.h"

bool ctx_init(Ctx* ctx) {
  memset(ctx, 0, sizeof(*ctx));
  if (!lex_intern_init(ctx) || !namemap_init(&ctx->module_ids, 16)) {
    ctx_free(ctx);
    return false;
  }
  return true;
}

void ctx_free(Ctx* ctx) {
  free(ctx->slots);
  free(ctx->names.v);
  free(ctx->strs.v);
  free(ctx->strbytes.v);
  for (u32 i = 0; i < ctx->modules.num; ++i) {
    Module* m = ctx->modules.v[i];
    namemap_free(&m->globals);
    free(m->name_buf);
    free(m);
  }
  free(ctx->modules.v);
  namemap_free(&ctx->module_ids);
  memset(ctx, 0, sizeof(*ctx));
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

bool namemap_init(NameMap* m, u32 cap) {
  m->v = (NameMapSlot*)calloc(cap, sizeof(NameMapSlot));
  m->num = 0;
  m->cap = cap;
  return m->v != NULL;
}

void namemap_free(NameMap* m) {
  free(m->v);
  memset(m, 0, sizeof(*m));
}

static void namemap_grow(Ctx* ctx, NameMap* m) {
  u32 cap = m->cap * 2;
  NameMapSlot* v = (NameMapSlot*)calloc(cap, sizeof(NameMapSlot));
  if (!v) {
    ctx_error(ctx, NULL, NULL, 0, "Out of memory.");
  }
  u32 mask = cap - 1;
  for (u32 j = 0; j < m->cap; ++j) {
    const NameMapSlot* s = &m->v[j];
    if (s->key == 0) {
      continue;
    }
    u32 i = (s->key - 1) & mask;
    while (v[i].key != 0) {
      i = (i + 1) & mask;
    }
    v[i] = *s;
  }
  free(m->v);
  m->v = v;
  m->cap = cap;
}

void namemap_set(Ctx* ctx, NameMap* m, NameId name, u32 val) {
  u32 mask = m->cap - 1;
  u32 i = name.idx & mask;
  for (;;) {
    NameMapSlot* s = &m->v[i];
    if (s->key == name.idx + 1) {
      s->val = val;
      return;
    }
    if (s->key == 0) {
      s->key = name.idx + 1;
      s->val = val;
      if (++m->num * 2 > m->cap) {
        namemap_grow(ctx, m);
      }
      return;
    }
    i = (i + 1) & mask;
  }
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
