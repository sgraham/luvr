// The compiler driver.
//   luv parse [-q] file...
//   luv il file
// parse checks each file, and prints the first error in each failing file.
// -q suppresses the summary. il prints the IL for each function in file.
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

typedef void (*ParseFn)(Ctx* ctx, const char* path);

static int usage(void) {
  fprintf(stderr,
          "usage: luv parse [-q] file...\n"  //
          "       luv il file\n");
  return 2;
}

// Each file is a separate program, so it has its own Ctx. Prints the error
// and returns false if the file fails. Otherwise adds the size of the
// program's modules to nbytes.
static bool run_file(ParseFn fn,
                     FuncDoneFn on_func,
                     AggDoneFn on_agg,
                     const char* path,
                     size_t* nbytes,
                     double* secs) {
  Ctx ctx;
  if (!ctx_init(&ctx)) {
    fprintf(stderr, "out of memory\n");
    exit(1);
  }
  ctx.load_file = load_file;
  ctx.on_func = on_func;
  ctx.on_agg = on_agg;
  jmp_buf jb;
  ctx.on_error = &jb;
  double t0 = now_sec();
  if (setjmp(jb)) {
    u32 line, col;
    ctx_error_location(&ctx, &line, &col);
    const char* file = ctx.err_file ? ctx.err_file : path;
    if (line) {
      // The buffer is padded with 0s, so the scan for the end of the line
      // stops even when the error is on the last line.
      const u8* bol = ctx.err_buf + ctx.err_offset - (col - 1);
      const u8* eol = bol;
      while (*eol != '\n' && *eol != '\r' && *eol != 0) {
        ++eol;
      }
      int prefix = fprintf(stderr, "%s:%u:%u:", file, line, col);
      fprintf(stderr, "%.*s\n", (int)(eol - bol), (const char*)bol);
      fprintf(stderr, "%*s^ error: %s\n", prefix + (int)col - 1, "", ctx.err_msg);
    } else {
      fprintf(stderr, "%s: error: %s\n", file, ctx.err_msg);
    }
    ctx_free(&ctx);
    return false;
  }
  fn(&ctx, path);
  *secs += now_sec() - t0;
  for (u32 j = 0; j < ctx.modules.num; ++j) {
    const FileBuf* src = &ctx.modules.v[j]->src;
    *nbytes += (size_t)(src->end - src->buf);
  }
  ctx_free(&ctx);
  return true;
}

static int cmd_parse(int argc, char** argv) {
  bool quiet = false;
  int first = 0;
  if (argc > 0 && strcmp(argv[0], "-q") == 0) {
    quiet = true;
    first = 1;
  }
  if (first >= argc) {
    return usage();
  }
  int failures = 0;
  size_t nbytes = 0;
  double secs = 0;
  for (int i = first; i < argc; ++i) {
    failures += !run_file(parse_file, NULL, NULL, argv[i], &nbytes, &secs);
  }
  if (!quiet) {
    int nfiles = argc - first;
    fprintf(stderr, "%d/%d files ok, %.1f MB parsed in %.3f s (%.0f MB/s)\n", nfiles - failures,
            nfiles, (double)nbytes / 1e6, secs, secs > 0 ? (double)nbytes / 1e6 / secs : 0.0);
  }
  return failures ? 1 : 0;
}

static void print_func(Ctx* ctx, const IlFunc* f, const u8* src) {
  // Data is printed before the first function that can refer to it.
  if (ctx->data_printed < ctx->data.num) {
    for (; ctx->data_printed < ctx->data.num; ++ctx->data_printed) {
      il_dump_data(ctx, ctx->data_printed, stdout);
    }
    fputc('\n', stdout);
  }
  il_dump(f, src, stdout);
  fputc('\n', stdout);
}

static void print_agg(Ctx* ctx, AggId agg) {
  il_dump_agg(ctx, agg.idx, stdout);
  fputc('\n', stdout);
}

static int cmd_il(int argc, char** argv) {
  if (argc != 1) {
    return usage();
  }
  size_t nbytes = 0;
  double secs = 0;
  return run_file(parse_file_emit, print_func, print_agg, argv[0], &nbytes, &secs) ? 0 : 1;
}

int main(int argc, char** argv) {
  if (argc > 1 && strcmp(argv[1], "parse") == 0) {
    return cmd_parse(argc - 2, argv + 2);
  }
  if (argc > 1 && strcmp(argv[1], "il") == 0) {
    return cmd_il(argc - 2, argv + 2);
  }
  return usage();
}
