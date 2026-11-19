// Tests of functions that are easier to check directly than through .luv
// files.
//   unittest
// Prints each failure. Exit status is 1 if any failed.

#include "luvr.h"

static int failures;

static void test_path_dir_len(void) {
  static const struct {
    const char* path;
    u32 want;
  } cases[] = {
      {"", 0}, {"x", 0}, {"a/x", 2}, {"a/b/x", 4}, {"../x", 3}, {"a/", 2},
  };
  for (size_t i = 0; i < COUNTOF(cases); ++i) {
    const char* path = cases[i].path;
    u32 got = path_dir_len((const u8*)path, (u32)strlen(path));
    if (got != cases[i].want) {
      printf("path_dir_len(\"%s\") = %u, want %u\n", path, got, cases[i].want);
      ++failures;
    }
  }
}

static void test_path_stem(void) {
  static const struct {
    const char* path;
    const char* want;
  } cases[] = {
      {"x.luv", "x"},   {"test/x.luv", "x"},    {"a/b/x.luv", "x"},  {"x", "x"},
      {".luv", ".luv"}, {"x.luv.luv", "x.luv"}, {"test/x.c", "x.c"},
  };
  for (size_t i = 0; i < COUNTOF(cases); ++i) {
    u32 len;
    const char* got = path_stem(cases[i].path, &len);
    if (len != strlen(cases[i].want) || memcmp(got, cases[i].want, len) != 0) {
      printf("path_stem(\"%s\") = \"%.*s\", want \"%s\"\n", cases[i].path, (int)len, got,
             cases[i].want);
      ++failures;
    }
  }
}

static void test_path_import_is_invalid(void) {
  const char* empty = "An import path can't have an empty component.";
  const char* dot = "An import path can't have a '.' component.";
  static const struct {
    const char* path;
    bool empty;
    bool dot;
  } cases[] = {
      {"x", false, false},  {"sub/x", false, false}, {"../x", false, false}, {"..", false, false},
      {".x", false, false}, {"x.", false, false},    {"...", false, false},  {"", true, false},
      {"/x", true, false},  {"x/", true, false},     {"a//b", true, false},  {".", false, true},
      {"./x", false, true}, {"a/./b", false, true},  {"a/.", false, true},
  };
  for (size_t i = 0; i < COUNTOF(cases); ++i) {
    const char* path = cases[i].path;
    const char* got = path_import_is_invalid((const u8*)path, (u32)strlen(path));
    const char* want = cases[i].empty ? empty : cases[i].dot ? dot : NULL;
    if (got && want ? strcmp(got, want) != 0 : got != want) {
      printf("path_import_is_invalid(\"%s\") = %s, want %s\n", path, got ? got : "NULL",
             want ? want : "NULL");
      ++failures;
    }
  }
}

static void test_path_normalize(void) {
  static const struct {
    const char* path;
    const char* want;
  } cases[] = {
      {"x", "x"},
      {"sub/x", "sub/x"},
      {"sub/../x", "x"},
      {"a/b/..", "a"},
      {"sub/..", ""},
      {"../x", "../x"},
      {"../../x", "../../x"},
      {"a/../../x", "../x"},
      {"sub/../../nopes", "../nopes"},
      {"../a/../b", "../b"},
      {"a/b/../../..", ".."},
  };
  for (size_t i = 0; i < COUNTOF(cases); ++i) {
    u8 buf[64];
    u32 len = (u32)strlen(cases[i].path);
    memcpy(buf, cases[i].path, len);
    u32 got = path_normalize(buf, len);
    if (got != strlen(cases[i].want) || memcmp(buf, cases[i].want, got) != 0) {
      printf("path_normalize(\"%s\") = \"%.*s\", want \"%s\"\n", cases[i].path, (int)got, buf,
             cases[i].want);
      ++failures;
    }
  }
}

static void expect_il_dump(const IlFunc* f, const char* src, const char* want, const char* what) {
  FILE* tmp = tmpfile();
  CHECK(tmp);
  for (u32 i = 0; i < f->ctx->data.num; ++i) {
    il_dump_data(f->ctx, i, tmp);
  }
  il_dump(f, (const u8*)src, tmp);
  rewind(tmp);
  char got[1024];
  size_t len = fread(got, 1, sizeof(got) - 1, tmp);
  got[len] = 0;
  fclose(tmp);
  if (strcmp(got, want) != 0) {
    printf("%s: il_dump gave\n%swant\n%s", what, got, want);
    ++failures;
  }
}

static void il_init_test(IlFunc* f, Ctx* ctx, IlType ret) {
  il_init(f, ctx);
  il_reset(f, lex_intern_safe(ctx, "f"), ret);
}

static void test_il_const_int(void) {
  Ctx ctx;
  CHECK(ctx_init(&ctx));
  IlFunc f;
  il_init(&f, &ctx);
  Ref one = il_const_int(&f, IT_I64, 1);
  if (il_const_int(&f, IT_I64, 1) != one || il_const_int(&f, IT_I32, 1) == one) {
    printf("il_const_int: constants aren't interned by value and type\n");
    ++failures;
  }
  if (il_const_int(&f, IT_I32, 0xffffffff) != il_const_int(&f, IT_I32, (u64)-1)) {
    printf("il_const_int: i32 0xffffffff and -1 aren't the same constant\n");
    ++failures;
  }
  // Enough to grow the table a few times.
  Ref refs[1000];
  for (u32 i = 0; i < COUNTOF(refs); ++i) {
    refs[i] = il_const_int(&f, IT_I64, i * 1000);
  }
  for (u32 i = 0; i < COUNTOF(refs); ++i) {
    if (il_const_int(&f, IT_I64, i * 1000) != refs[i]) {
      printf("il_const_int: %u isn't the same constant after growing\n", i * 1000);
      ++failures;
      break;
    }
  }
  il_free(&f);
  ctx_free(&ctx);
}

static void test_il_dump(void) {
  Ctx ctx;
  CHECK(ctx_init(&ctx));
  IlFunc f;
  il_init_test(&f, &ctx, IT_I64);
  Ref sum = il_emit(&f, OP_ADD, IT_I64, il_const_int(&f, IT_I64, 1), il_const_int(&f, IT_I64, 2));
  Ref diff = il_emit(&f, OP_SUB, IT_I64, sum, il_const_int(&f, IT_I64, (u64)-3));
  il_emit(&f, OP_RET, IT_I64, diff, 0);
  expect_il_dump(&f, "",
                 "func i64 @f\n"
                 "  %0 = ADD i64 1, 2\n"
                 "  %1 = SUB i64 %0, -3\n"
                 "  RET i64 %1\n",
                 "test_il_dump");
  il_reset(&f, f.name, IT_VOID);
  il_emit(&f, OP_RET, IT_VOID, 0, 0);
  expect_il_dump(&f, "", "func @f\n  RET\n", "test_il_dump after il_reset");
  il_free(&f);
  ctx_free(&ctx);
}

static void test_il_blocks(void) {
  Ctx ctx;
  CHECK(ctx_init(&ctx));
  IlFunc f;
  il_init_test(&f, &ctx, IT_VOID);
  il_block_start(&f, il_block_declare(&f));
  u32 end = il_block_declare(&f);
  il_branch(&f, il_const_int(&f, IT_I32, 1), end);
  il_jump(&f, end);
  il_block_start(&f, end);
  il_emit(&f, OP_RET, IT_VOID, 0, 0);
  expect_il_dump(&f, "",
                 "func @f\n"
                 ":0\n"
                 "  BR 1, :1\n"
                 ":2\n"
                 "  JMP :1\n"
                 ":1\n"
                 "  RET\n",
                 "test_il_blocks");
  il_free(&f);
  ctx_free(&ctx);
}

static void test_il_slots(void) {
  Ctx ctx;
  CHECK(ctx_init(&ctx));
  IlFunc f;
  il_init_test(&f, &ctx, IT_I64);
  u32 sx = il_slot_add(&f, IT_I64, lex_intern_safe(&ctx, "x"));
  il_slot_add(&f, IT_I32, (NameId){IL_SLOT_UNNAMED});
  const char* src = "def int main():\n    int x = 5\n    pass\n    return x\n";
  il_block_start(&f, il_block_declare(&f));
  il_source_pos(&f, (u32)(strstr(src, "int x") - src));
  Ref addr = il_emit(&f, OP_SLOT, IT_I64, sx, 0);
  il_emit(&f, OP_STORE, IT_I64, il_const_int(&f, IT_I64, 5), addr);
  il_source_pos(&f, (u32)(strstr(src, "pass") - src));
  il_source_pos(&f, (u32)(strstr(src, "return") - src));
  addr = il_emit(&f, OP_SLOT, IT_I64, sx, 0);
  Ref v = il_emit(&f, OP_LOAD, IT_I64, addr, 0);
  il_emit(&f, OP_RET, IT_I64, v, 0);
  expect_il_dump(&f, src,
                 "func i64 @f\n"
                 "  slot $0 i64 x\n"
                 "  slot $1 i32\n"
                 ":0\n"
                 "  ;     int x = 5\n"
                 "  %1 = SLOT $0\n"
                 "  STORE i64 5, %1\n"
                 "  ;     pass\n"
                 "  ;     return x\n"
                 "  %3 = SLOT $0\n"
                 "  %4 = LOAD i64 %3\n"
                 "  RET i64 %4\n",
                 "test_il_slots");
  il_free(&f);
  ctx_free(&ctx);
}

static void test_il_call(void) {
  Ctx ctx;
  CHECK(ctx_init(&ctx));
  IlFunc f;
  il_init_test(&f, &ctx, IT_I64);
  u32 sym = ctx_sym_add(&ctx, lex_intern_safe(&ctx, "g"));
  Ref callee = il_const_sym(&f, sym);
  if (il_const_sym(&f, sym) != callee || il_const_int(&f, IT_I64, sym) == callee) {
    printf("il_const_sym: symbols aren't interned apart from integers\n");
    ++failures;
  }
  il_block_start(&f, il_block_declare(&f));
  Ref one_arg[] = {il_const_int(&f, IT_I64, 5)};
  il_call(&f, IT_VOID, callee, one_arg, 1);
  Ref three_args[] = {il_const_int(&f, IT_I64, 1), il_const_int(&f, IT_I64, 2),
                      il_const_int(&f, IT_I64, 3)};
  Ref r = il_call(&f, IT_I64, callee, three_args, 3);
  il_emit(&f, OP_RET, IT_I64, r, 0);
  expect_il_dump(&f, "",
                 "func i64 @f\n"
                 ":0\n"
                 "  CALL @g, 5\n"
                 "  %3 = CALL i64 @g, 1, 2, 3\n"
                 "  RET i64 %3\n",
                 "test_il_call");
  il_free(&f);
  ctx_free(&ctx);
}

static void test_il_data(void) {
  Ctx ctx;
  CHECK(ctx_init(&ctx));
  IlFunc f;
  il_init_test(&f, &ctx, IT_VOID);
  u32 sym = ctx_sym_add(&ctx, lex_intern_safe(&ctx, "g"));
  u32 hi = il_data_add(&ctx, (const u8*)"hi", 2);
  u32 escaped = il_data_add(&ctx, (const u8*)"\"\\\n\t\x00\x7f\xff~ ", 9);
  il_data_add(&ctx, NULL, 0);
  il_block_start(&f, il_block_declare(&f));
  Ref args[] = {il_const_data(&f, escaped), il_const_int(&f, IT_I64, 9), il_const_data(&f, hi),
                il_const_int(&f, IT_I64, 2)};
  il_call(&f, IT_VOID, il_const_sym(&f, sym), args, 4);
  il_emit(&f, OP_RET, IT_VOID, 0, 0);
  expect_il_dump(&f, "",
                 "data @.dat0 \"hi\"\n"
                 "data @.dat1 \"\\\"\\\\\\n\\t\\x00\\x7f\\xff~ \"\n"
                 "data @.dat2 \"\"\n"
                 "func @f\n"
                 ":0\n"
                 "  CALL @g, @.dat1, 9, @.dat0, 2\n"
                 "  RET\n",
                 "test_il_data");
  il_free(&f);
  ctx_free(&ctx);
}

int main(void) {
  test_path_dir_len();
  test_path_stem();
  test_path_import_is_invalid();
  test_path_normalize();
  test_il_const_int();
  test_il_dump();
  test_il_blocks();
  test_il_slots();
  test_il_call();
  test_il_data();
  return failures ? 1 : 0;
}
