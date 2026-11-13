#include "luvr.h"

bool ctx_init(Ctx* ctx) {
  memset(ctx, 0, sizeof(*ctx));
  if (!lex_intern_init(ctx)) {
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
  src_location(ls->buf, who_offset, &who_line, &col);
  src_location(ls->buf, ls->pos.t.offset, &cur_line, &col);
  if (who_line == cur_line) {
    ctx_error(ls->ctx, ls->filename, ls->buf, ls->pos.t.offset, "Expected '%s', found %s.",
              tok_kind_name(what), found);
  }
  ctx_error(ls->ctx, ls->filename, ls->buf, ls->pos.t.offset,
            "Expected '%s' (to close '%s' at line %u), found %s.", tok_kind_name(what),
            tok_kind_name(who), who_line, found);
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
