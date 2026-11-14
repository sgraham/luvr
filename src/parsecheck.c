// Syntax-checks files with parser.c.
//   parsecheck [-q] file...
// Prints the first error in each failing file; -q suppresses the summary.
// Exit status is 1 if any file failed.

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

int main(int argc, char** argv) {
  bool quiet = false;
  int first = 1;
  if (argc > 1 && strcmp(argv[1], "-q") == 0) {
    quiet = true;
    first = 2;
  }
  if (first >= argc) {
    fprintf(stderr, "usage: parsecheck [-q] file...\n");
    return 2;
  }
  int failures = 0;
  size_t nbytes = 0;
  double secs = 0;
  for (int i = first; i < argc; ++i) {
    size_t len;
    u8* buf = load_file(argv[i], &len);
    if (!buf) {
      fprintf(stderr, "%s: cannot read\n", argv[i]);
      ++failures;
      continue;
    }
    // Each file is a separate program, so it has its own Ctx.
    Ctx ctx;
    if (!ctx_init(&ctx)) {
      fprintf(stderr, "out of memory\n");
      return 1;
    }
    jmp_buf jb;
    ctx.on_error = &jb;
    double t0 = now_sec();
    if (setjmp(jb)) {
      u32 line, col;
      ctx_error_location(&ctx, &line, &col);
      fprintf(stderr, "%s:%u:%u: error: %s\n", ctx.err_file ? ctx.err_file : argv[i], line, col,
              ctx.err_msg);
      ++failures;
      // Interned names point into buf, so it's freed after ctx.
      ctx_free(&ctx);
      free(buf);
      continue;
    }
    FileBuf src = {argv[i], buf, buf + len};
    parse_file(&ctx, &src);
    secs += now_sec() - t0;
    nbytes += len;
    ctx_free(&ctx);
    free(buf);
  }
  if (!quiet) {
    int nfiles = argc - first;
    fprintf(stderr, "%d/%d files ok, %.1f MB parsed in %.3f s (%.0f MB/s)\n", nfiles - failures,
            nfiles, (double)nbytes / 1e6, secs, secs > 0 ? (double)nbytes / 1e6 / secs : 0.0);
  }
  return failures ? 1 : 0;
}
