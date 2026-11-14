// Driver for lex.c.
//   lexdump -d file...   dump tokens (line:col kind payload)
//   lexdump -r file...   check that rewinding the lexer at each open bracket
//                        and lexing again reaches the same state
//   lexdump file...      count tokens and time lexing (no dump)

#include "luvr.h"

static u8* load_file(const char* path, size_t* len) {
  FILE* f = fopen(path, "rb");
  if (!f) {
    return NULL;
  }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n < 0) {
    fclose(f);
    return NULL;
  }
  u8* buf = (u8*)malloc((size_t)n + LEX_PADDING);
  if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
    fclose(f);
    free(buf);
    return NULL;
  }
  fclose(f);
  memset(buf + n, 0, LEX_PADDING);
  *len = (size_t)n;
  return buf;
}

static double now_sec(void) {
  struct timespec ts;
  timespec_get(&ts, TIME_UTC);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static const char* const suffix_names[] = {"",    "i8",  "i16", "i32", "i64", "u8",
                                           "u16", "u32", "u64", "f32", "f64"};

static void print_escaped(const u8* s, u32 len) {
  putchar('"');
  for (u32 i = 0; i < len; ++i) {
    u8 c = s[i];
    if (c == '\n') {
      fputs("\\n", stdout);
    } else if (c == '\t') {
      fputs("\\t", stdout);
    } else if (c == '"') {
      fputs("\\\"", stdout);
    } else if (c == '\\') {
      fputs("\\\\", stdout);
    } else if (c < 0x20) {
      printf("\\x%02x", c);
    } else {
      putchar(c);
    }
  }
  putchar('"');
}

static void dump_token(const LexState* ls, u32 line, u32 col) {
  const Token* t = &ls->pos.t;
  printf("%u:%u\t%s", line, col, tok_kind_name(t->kind));
  switch (t->kind) {
    case TK_NAME:
    case TK_TYPENAME:
    case TK_TYPEPARAM: {
      const Name* n = lex_name(ls->ctx, t->v.name);
      printf("\t%.*s", (int)n->len, (const char*)n->str);
      break;
    }
    case TK_INTLIT:
      printf("\t%llu%s", (unsigned long long)t->v.i, suffix_names[t->flags & TF_SUFFIX_MASK]);
      break;
    case TK_FLOATLIT:
      printf("\t%.17g%s", t->v.f, suffix_names[t->flags & TF_SUFFIX_MASK]);
      break;
    case TK_STRING:
    case TK_STRING_BEGIN:
    case TK_STRING_MID:
    case TK_STRING_END: {
      u32 len;
      const u8* s = lex_str(ls, t, &len);
      putchar('\t');
      print_escaped(s, len);
      if (t->flags & TF_ESCAPED) {
        fputs(" (escaped)", stdout);
      }
      break;
    }
    default:
      break;
  }
  putchar('\n');
}

static void lex_past_close(LexState* ls, u32 depth) {
  for (;;) {
    lex_next(ls);
    if (ls->pos.bracket_depth < depth || ls->pos.t.kind == TK_EOS) {
      break;
    }
  }
}

// ls has just opened a bracket. It's rewound to there afterwards so that the
// brackets nested inside are also checked.
static void check_rewind(LexState* ls) {
  u32 depth = ls->pos.bracket_depth;
  LexPos mark, first;
  lex_mark(ls, &mark);
  lex_past_close(ls, depth);
  lex_mark(ls, &first);
  lex_rewind(ls, &mark);
  lex_past_close(ls, depth);
  if (!lex_same_pos_for_testing(ls, &first, &ls->pos)) {
    ctx_error(ls->ctx, ls->src.filename, ls->src.buf, mark.t.offset,
              "Lexing again after rewinding gave a different state.");
  }
  lex_rewind(ls, &mark);
}

typedef enum Mode {
  MODE_TIME,
  MODE_DUMP,
  MODE_REWIND,
} Mode;

static int run_file(Ctx* ctx,
                    const char* path,
                    Mode mode,
                    u64* total_tokens,
                    size_t* total_bytes,
                    double* total_time) {
  size_t len;
  u8* buf = load_file(path, &len);
  if (!buf) {
    fprintf(stderr, "%s: cannot read\n", path);
    return 1;
  }
  jmp_buf jb;
  ctx->on_error = &jb;
  LexState ls;
  u64 ntok = 0;
  if (setjmp(jb)) {
    u32 line, col;
    ctx_error_location(ctx, &line, &col);
    fprintf(stderr, "%s:%u:%u: error: %s\n", ctx->err_file ? ctx->err_file : path, line, col,
            ctx->err_msg);
    // Interned names may point into buf, so it's leaked rather than freed.
    return 1;
  }
  double t0 = now_sec();
  FileBuf src = {path, buf, buf + len};
  lex_init(&ls, ctx, &src);
  if (mode == MODE_DUMP) {
    // Token offsets only increase, so line and column are tracked
    // incrementally.
    u32 line = 1, last = 0;
    const u8* bol = buf;
    for (;;) {
      lex_next(&ls);
      ++ntok;
      for (; last < ls.pos.t.offset && last < len; ++last) {
        if (buf[last] == '\n') {
          ++line;
          bol = buf + last + 1;
        }
      }
      dump_token(&ls, line, (u32)(buf + ls.pos.t.offset - bol) + 1);
      if (ls.pos.t.kind == TK_EOS) {
        break;
      }
    }
  } else if (mode == MODE_REWIND) {
    for (;;) {
      lex_next(&ls);
      ++ntok;
      TokenKind k = ls.pos.t.kind;
      if (k == TK_EOS) {
        break;
      }
      // Not at a \(, because lexing past its ')' closes the interpolation,
      // and a checkpoint can't be restored after that.
      if (k == TK_LPAREN || k == TK_LSQUARE || k == TK_LBRACE) {
        check_rewind(&ls);
      }
    }
  } else {
    for (;;) {
      lex_next(&ls);
      ++ntok;
      if (ls.pos.t.kind == TK_EOS) {
        break;
      }
    }
  }
  double t1 = now_sec();
  *total_tokens += ntok;
  *total_bytes += len;
  *total_time += t1 - t0;
  // buf isn't freed because interned names point into it.
  return 0;
}

int main(int argc, char** argv) {
  Mode mode = MODE_TIME;
  int first = 1;
  if (argc > 1 && strcmp(argv[1], "-d") == 0) {
    mode = MODE_DUMP;
    first = 2;
  } else if (argc > 1 && strcmp(argv[1], "-r") == 0) {
    mode = MODE_REWIND;
    first = 2;
  }
  if (first >= argc) {
    fprintf(stderr, "usage: lexdump [-d|-r] file...\n");
    return 2;
  }
  Ctx ctx;
  if (!ctx_init(&ctx)) {
    fprintf(stderr, "out of memory\n");
    return 1;
  }
  u64 ntok = 0;
  size_t nbytes = 0;
  double secs = 0;
  int failures = 0;
  for (int i = first; i < argc; ++i) {
    failures += run_file(&ctx, argv[i], mode, &ntok, &nbytes, &secs);
  }
  if (mode != MODE_DUMP) {
    fprintf(stderr, "%llu tokens, %.1f MB, %u names, %.3f s (%.0f MB/s, %.1f Mtok/s)\n",
            (unsigned long long)ntok, (double)nbytes / 1e6, ctx.names.num, secs,
            secs > 0 ? (double)nbytes / 1e6 / secs : 0.0,
            secs > 0 ? (double)ntok / 1e6 / secs : 0.0);
  }
  ctx_free(&ctx);
  return failures ? 1 : 0;
}
