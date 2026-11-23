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
  for (u32 i = 0; i < f->ctx->aggs.num; ++i) {
    il_dump_agg(f->ctx, i, tmp);
  }
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
  SlotId sx = il_slot_add(&f, IT_I64, lex_intern_safe(&ctx, "x"));
  il_slot_add(&f, IT_I32, (NameId){IL_SLOT_UNNAMED});
  const char* src = "def int main():\n    int x = 5\n    pass\n    return x\n";
  il_block_start(&f, il_block_declare(&f));
  il_source_pos(&f, (u32)(strstr(src, "int x") - src));
  Ref addr = il_slot_addr(&f, sx);
  il_emit(&f, OP_STORE, IT_I64, il_const_int(&f, IT_I64, 5), addr);
  il_source_pos(&f, (u32)(strstr(src, "pass") - src));
  il_source_pos(&f, (u32)(strstr(src, "return") - src));
  addr = il_slot_addr(&f, sx);
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
  Ref param = il_emit(&f, OP_PARAM, IT_I32, 1, 0);
  Ref one_arg[] = {il_const_int(&f, IT_I64, 5)};
  il_call(&f, IT_VOID, callee, one_arg, 1);
  Ref three_args[] = {il_const_int(&f, IT_I64, 1), param, il_const_int(&f, IT_I64, 3)};
  Ref r = il_call(&f, IT_I64, callee, three_args, 3);
  il_emit(&f, OP_RET, IT_I64, r, 0);
  expect_il_dump(&f, "",
                 "func i64 @f\n"
                 ":0\n"
                 "  %1 = PARAM i32 1\n"
                 "  CALL @g, 5\n"
                 "  %4 = CALL i64 @g, 1, %1, 3\n"
                 "  RET i64 %4\n",
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

static void test_il_agg_add(void) {
  // The checked cases in the design notes, and a few around the limits.
  static const struct {
    const char* what;
    u32 size;
    u32 align;
    AggLeaf leaves[6];
    u32 num_leaves;
    u8 want_abi;
  } cases[] = {
      {"str", 16, 8, {{0, IT_I64}, {8, IT_I64}}, 2, 0},
      {"f32, f32", 8, 4, {{0, IT_F32}, {4, IT_F32}}, 2, AGG_SSE0 | AGG_HFA_F32},
      {"f32, f32, f32",
       12,
       4,
       {{0, IT_F32}, {4, IT_F32}, {8, IT_F32}},
       3,
       AGG_SSE0 | AGG_SSE1 | AGG_HFA_F32},
      {"f64, f64", 16, 8, {{0, IT_F64}, {8, IT_F64}}, 2, AGG_SSE0 | AGG_SSE1 | AGG_HFA_F64},
      {"f64 x 4", 32, 8, {{0, IT_F64}, {8, IT_F64}, {16, IT_F64}, {24, IT_F64}}, 4, AGG_HFA_F64},
      {"[4]f32",
       16,
       4,
       {{0, IT_F32}, {4, IT_F32}, {8, IT_F32}, {12, IT_F32}},
       4,
       AGG_SSE0 | AGG_SSE1 | AGG_HFA_F32},
      {"f32, i32", 8, 4, {{0, IT_F32}, {4, IT_I32}}, 2, 0},
      {"f64, i64", 16, 8, {{0, IT_F64}, {8, IT_I64}}, 2, AGG_SSE0},
      {"f32, f32, f64", 16, 8, {{0, IT_F32}, {4, IT_F32}, {8, IT_F64}}, 3, AGG_SSE0 | AGG_SSE1},
      {"f64, f32", 16, 8, {{0, IT_F64}, {8, IT_F32}}, 2, AGG_SSE0 | AGG_SSE1},
      {"i32, i32, i32", 12, 4, {{0, IT_I32}, {4, IT_I32}, {8, IT_I32}}, 3, 0},
      {"u8, u8, u8", 3, 1, {{0, IT_U8}, {1, IT_U8}, {2, IT_U8}}, 3, 0},
      {"i16, u8", 4, 2, {{0, IT_I16}, {2, IT_U8}}, 2, 0},
      {"i64 x 3", 24, 8, {{0, IT_I64}, {8, IT_I64}, {16, IT_I64}}, 3, 0},
      {"f32 x 5", 20, 4, {{0, IT_F32}, {4, IT_F32}, {8, IT_F32}, {12, IT_F32}, {16, IT_F32}}, 5, 0},
      {"i32, f32, f64", 16, 8, {{0, IT_I32}, {4, IT_F32}, {8, IT_F64}}, 3, AGG_SSE1},
      {"f64 x 4 (too large to read)", 40, 8, {{0, IT_F64}}, 1, 0},
  };
  Ctx ctx;
  CHECK(ctx_init(&ctx));
  for (size_t i = 0; i < COUNTOF(cases); ++i) {
    AggId agg =
        il_agg_add(&ctx, cases[i].size, cases[i].align, cases[i].leaves, cases[i].num_leaves);
    const AggInfo* got = il_agg(&ctx, agg);
    if (agg.idx != i || got->size != cases[i].size || 1u << got->align_log2 != cases[i].align ||
        got->abi != cases[i].want_abi) {
      printf("il_agg_add(%s) = #%u {size %u, align %u, abi %#x}, want #%zu {%u, %u, %#x}\n",
             cases[i].what, agg.idx, got->size, 1u << got->align_log2, got->abi, i, cases[i].size,
             cases[i].align, cases[i].want_abi);
      ++failures;
    }
  }
  ctx_free(&ctx);
}

static void test_il_agg_slot(void) {
  Ctx ctx;
  CHECK(ctx_init(&ctx));
  IlFunc f;
  il_init_test(&f, &ctx, IT_VOID);
  AggLeaf leaves[] = {{0, IT_I64}, {8, IT_U8}};
  AggId agg = il_agg_add(&ctx, 16, 8, leaves, 2);
  AggLeaf float_leaves[] = {{0, IT_F64}, {8, IT_F64}};
  il_agg_add(&ctx, 16, 8, float_leaves, 2);
  SlotId slot = il_slot_add_agg(&f, agg, lex_intern_safe(&ctx, "s"));
  SlotId temp = il_slot_add_agg(&f, agg, (NameId){IL_SLOT_UNNAMED});
  il_block_start(&f, il_block_declare(&f));
  Ref addr = il_slot_addr(&f, slot);
  il_emit(&f, OP_ZERO, IT_VOID, addr, agg.idx);
  Ref temp_addr = il_slot_addr(&f, temp);
  il_copy(&f, temp_addr, addr, agg);
  il_emit(&f, OP_RET, IT_VOID, 0, 0);
  expect_il_dump(&f, "",
                 "agg #0 16 8\n"
                 "agg #1 16 8 sse0 sse1 hfa_f64\n"
                 "func @f\n"
                 "  slot $0 #0 s\n"
                 "  slot $1 #0\n"
                 ":0\n"
                 "  %1 = SLOT $0\n"
                 "  ZERO %1, #0\n"
                 "  %3 = SLOT $1\n"
                 "  COPY %3, %1, #0\n"
                 "  RET\n",
                 "test_il_agg_slot");
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
  test_il_agg_add();
  test_il_agg_slot();
  return failures ? 1 : 0;
}
