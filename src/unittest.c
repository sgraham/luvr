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

static void expect_il_dump(const IlFunc* f, const char* want, const char* what) {
  FILE* tmp = tmpfile();
  CHECK(tmp);
  il_dump(f, tmp);
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
  il_init(&f, &ctx);
  Ref sum = il_emit(&f, OP_ADD, IT_I64, il_const_int(&f, IT_I64, 1), il_const_int(&f, IT_I64, 2));
  Ref diff = il_emit(&f, OP_SUB, IT_I64, sum, il_const_int(&f, IT_I64, (u64)-3));
  il_emit(&f, OP_RET, IT_I64, diff, 0);
  expect_il_dump(&f,
                 "  %0 = ADD i64 1, 2\n"
                 "  %1 = SUB i64 %0, -3\n"
                 "  RET i64 %1\n",
                 "test_il_dump");
  il_reset(&f);
  il_emit(&f, OP_RET, IT_VOID, 0, 0);
  expect_il_dump(&f, "  RET\n", "test_il_dump after il_reset");
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
  return failures ? 1 : 0;
}
