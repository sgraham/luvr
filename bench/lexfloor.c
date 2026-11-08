// Lower-bound comparison points for the lexer. Loads a file and then times
// passes over the same buffer repeatedly.

#include "../src/luvr.h"

#include "../src/lex.h"

static double now_sec(void) {
  struct timespec ts;
  timespec_get(&ts, TIME_UTC);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static const u8* g_buf;
static size_t g_len;
static volatile u64 g_sink;

// Sum all bytes, supposed to represent close to the bandwidth floor.
static u64 pass_sum(void) {
  u64 s = 0;
  for (size_t i = 0; i < g_len; ++i) s += g_buf[i];
  return s;
}

// Count lines with memchr.
static u64 pass_memchr(void) {
  u64 n = 0;
  const u8* p = g_buf;
  const u8* end = g_buf + g_len;
  while ((p = (const u8*)memchr(p, '\n', (size_t)(end - p))) != NULL) {
    ++n;
    ++p;
  }
  return n;
}

// Copy input to output buffer, but try to defeat auto-vectorization.
static u8* g_out;
static u64 pass_copy_scalar(void) {
  size_t o = 0;
  for (size_t i = 0; i < g_len; ++i) {
    u8 c = g_buf[i];
    g_out[o] = c;
    o += (c != ' ');  // dependent index: defeats vectorization
  }
  return o;
}

// Something close to a minimal tokenizer, branch on every byte, but don't
// actually intern or strtod, etc.
static u8 g_cls[256];
static u64 pass_mini_tokenize(void) {
  u64 n = 0;
  const u8* p = g_buf;
  for (;;) {
    u8 c = *p;
    if (g_cls[c] == 1) {
      do ++p; while (g_cls[*p] == 1);
      ++n;
    } else if (c == ' ' || c == '\n') {
      ++p;
    } else if (c == 0) {
      break;
    } else {
      ++p;
      ++n;
    }
  }
  return n;
}

static u64 pass_lex(void) {
  Ctx ctx;
  if (!ctx_init(&ctx)) exit(1);
  jmp_buf jb;
  ctx.on_error = &jb;
  if (setjmp(jb)) {
    fprintf(stderr, "lex error: %s\n", ctx.err_msg);
    exit(1);
  }
  LexState ls;
  u64 n = 0;
  lex_init(&ls, &ctx, "bench", g_buf, g_len);
  do {
    lex_next(&ls);
    ++n;
  } while (ls.t.kind != TK_EOS);
  ctx_free(&ctx);
  return n;
}

static void run(const char* name, u64 (*fn)(void)) {
  double best = 1e9;
  u64 result = 0;
  for (int i = 0; i < 5; ++i) {
    double t0 = now_sec();
    result = fn();
    double t = now_sec() - t0;
    if (t < best) best = t;
  }
  g_sink = result;
  printf("%-16s %8.1f ms  %7.2f GB/s  %6.2f ns/byte  (result %llu)\n", name, best * 1e3,
         (double)g_len / best / 1e9, best * 1e9 / (double)g_len, (unsigned long long)result);
}

int main(int argc, char** argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: lexfloor file\n");
    return 2;
  }
  double t0 = now_sec();
  FILE* f = fopen(argv[1], "rb");
  if (!f) return 1;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  u8* buf = (u8*)malloc((size_t)n + LEX_PADDING);
  if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) return 1;
  fclose(f);
  memset(buf + n, 0, LEX_PADDING);
  printf("%-16s %8.1f ms  (%.1f MB, includes page faults on fresh memory)\n", "fread", (now_sec() - t0) * 1e3,
         (double)n / 1e6);
  g_buf = buf;
  g_len = (size_t)n;
  g_out = (u8*)malloc((size_t)n);
  memset(g_out, 0, (size_t)n);  // pre-fault so the pass measures copying only
  for (int c = 0; c < 256; ++c) {
    g_cls[c] = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
  }

  run("sum (vector)", pass_sum);
  run("memchr lines", pass_memchr);
  run("scalar copy", pass_copy_scalar);
  run("mini tokenize", pass_mini_tokenize);
  run("lex", pass_lex);
  return 0;
}
