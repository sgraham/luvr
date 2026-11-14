#pragma once

typedef struct Ctx Ctx;
typedef struct FileBuf FileBuf;
typedef struct LexPos LexPos;
typedef struct LexState LexState;
typedef struct Module Module;
typedef struct Name Name;
typedef struct NameId NameId;
typedef struct NameMap NameMap;
typedef struct NameMapSlot NameMapSlot;
typedef struct StrRef StrRef;
typedef struct Token Token;

// Platform

#if defined(__clang__)

#  define COMPILER_CLANG 1

#  if defined(_WIN32)
#    define OS_WINDOWS 1
#  elif defined(__gnu_linux__) || defined(__linux__)
#    define OS_LINUX 1
#  elif defined(__APPLE__) && defined(__MACH__)
#    define OS_MAC 1
#  else
#    error port
#  endif

#  if defined(__amd64__) || defined(__amd64) || defined(__x86_64__) || defined(__x86_64)
#    define ARCH_X64 1
#  elif defined(__aarch64__)
#    define ARCH_ARM64 1
#  else
#    error port
#  endif

#elif defined(_MSC_VER)

#  define COMPILER_MSVC 1

#  if defined(_WIN32)
#    define OS_WINDOWS 1
#  else
#    error port
#  endif

#  if defined(_M_AMD64)
#    define ARCH_X64 1
#  elif defined(_M_ARM64)
#    define ARCH_ARM64 1
#  else
#    error port
#  endif

#else

#  error Compiler not supported.

#endif

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#  error little-endian only
#endif

#if !defined(BUILD_DEBUG)
#  define BUILD_DEBUG 1
#endif

#if !defined(ARCH_X64)
#  define ARCH_X64 0
#endif
#if !defined(ARCH_ARM64)
#  define ARCH_ARM64 0
#endif
#if !defined(COMPILER_MSVC)
#  define COMPILER_MSVC 0
#endif
#if !defined(COMPILER_CLANG)
#  define COMPILER_CLANG 0
#endif
#if !defined(OS_WINDOWS)
#  define OS_WINDOWS 0
#endif
#if !defined(OS_LINUX)
#  define OS_LINUX 0
#endif
#if !defined(OS_MAC)
#  define OS_MAC 0
#endif

// Base

#include <assert.h>
#include <inttypes.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if COMPILER_MSVC
#  define FORCE_INLINE __forceinline
#elif COMPILER_CLANG || COMPILER_GCC
#  define FORCE_INLINE __attribute__((always_inline))
#endif

#if COMPILER_MSVC
#  define NORETURN __declspec(noreturn)
#elif COMPILER_CLANG || COMPILER_GCC
#  define NORETURN __attribute__((noreturn))
#else
#  error port
#endif

#if COMPILER_MSVC
#  define TRAP() __debugbreak()
#elif COMPILER_CLANG || COMPILER_GCC
#  define TRAP() __builtin_trap()
#else
#  error port
#endif

#define CHECK(x)                                                            \
  do {                                                                      \
    if (!(x)) {                                                             \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); \
      TRAP();                                                               \
    }                                                                       \
  } while (0)

#if BUILD_DEBUG
#  define ASSERT(x) CHECK(x)
#else
#  define ASSERT(x)
#endif

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

// A growable array of T. The elements are v[0..num).
#define VEC(T) \
  struct {     \
    T* v;      \
    u32 num;   \
    u32 cap;   \
  }

// Makes room for n more elements in vec.
#define vec_reserve(ctx, vec, n)                                                             \
  ((size_t)(vec)->num + (n) > (vec)->cap                                                     \
       ? (void)((vec)->v = vec_grow_((ctx), (vec)->v, &(vec)->cap, (size_t)(vec)->num + (n), \
                                     sizeof(*(vec)->v)))                                     \
       : (void)0)

// Appends an uninitialized element to vec, and returns a pointer to it.
#define vec_push(ctx, vec) (vec_reserve((ctx), (vec), 1), &(vec)->v[(vec)->num++])

// Grows *cap to at least need, and returns v reallocated to match. Running out
// of memory, or needing more than 2^32 elements, is an error.
void* vec_grow_(Ctx* ctx, void* v, u32* cap, size_t need, size_t elem_size);

// Index of an interned Name in Ctx.names. It's a struct so that it can't be
// mixed up with offsets or other indices.
struct NameId {
  u32 idx;
};

// A hash table from NameId to u32, using open addressing and linear probing.
// A NameId is an index into Ctx.names, allocated in the order that names are
// first interned, so ids are compact, and names that are declared together are
// usually close together. Using the id itself as the hash keeps them in nearby
// slots, and on a table with many entries that's much faster than spreading
// them out with a hash. An array indexed directly by the id would avoid
// probing, but it would have to cover every name in the Ctx, and grow as more
// names are interned.
struct NameMap {
  NameMapSlot* v;
  u32 num;
  // A power of 2. The table is kept at most half full, so a probe always
  // reaches an empty slot.
  u32 cap;
};

struct NameMapSlot {
  u32 key;  // NameId.idx + 1, so that a zeroed slot is empty
  u32 val;
};

// cap must be a power of 2. Returns false if allocation fails.
bool namemap_init(NameMap* m, u32 cap);
void namemap_free(NameMap* m);

// Sets name's value, adding it if it's new.
void namemap_set(Ctx* ctx, NameMap* m, NameId name, u32 val);

// Returns the value for name, or NULL if it isn't in m.
static inline u32* namemap_find(const NameMap* m, NameId name) {
  u32 mask = m->cap - 1;
  u32 i = name.idx & mask;
  for (;;) {
    NameMapSlot* s = &m->v[i];
    if (s->key == name.idx + 1) {
      return &s->val;
    }
    if (s->key == 0) {
      return NULL;
    }
    i = (i + 1) & mask;
  }
}

// Token kinds:
// - X(name, display) for non-keywords
// - KW(name, spelling) for keywords
// - TY(name, spelling) for builtin type keywords
//
// Note: builtin types are [TK_FIRST_TYPE..TK_LAST_TYPE] (so have to be
// contiguous) for comparisons in the parser. Keywords and builtin types are
// last, starting at TK_FIRST_KEYWORD.
#define LEX_TOKENS(X, KW, TY)       \
  X(EOS, "<eof>")                   \
  X(NEWLINE, "<newline>")           \
  X(INDENT, "<indent>")             \
  X(DEDENT, "<dedent>")             \
  X(NAME, "<name>")                 \
  X(TYPENAME, "<typename>")         \
  X(TYPEPARAM, "<typeparam>")       \
  X(INTLIT, "<int>")                \
  X(FLOATLIT, "<float>")            \
  X(STRING, "<string>")             \
  X(STRING_BEGIN, "<string-begin>") \
  X(STRING_MID, "<string-mid>")     \
  X(STRING_END, "<string-end>")     \
  X(LPAREN, "(")                    \
  X(RPAREN, ")")                    \
  X(LSQUARE, "[")                   \
  X(RSQUARE, "]")                   \
  X(LBRACE, "{")                    \
  X(RBRACE, "}")                    \
  X(COMMA, ",")                     \
  X(COLON, ":")                     \
  X(DOT, ".")                       \
  X(PLUS, "+")                      \
  X(MINUS, "-")                     \
  X(STAR, "*")                      \
  X(SLASH, "/")                     \
  X(PERCENT, "%")                   \
  X(AMP, "&")                       \
  X(PIPE, "|")                      \
  X(CARET, "^")                     \
  X(TILDE, "~")                     \
  X(LT, "<")                        \
  X(GT, ">")                        \
  X(LE, "<=")                       \
  X(GE, ">=")                       \
  X(EQEQ, "==")                     \
  X(NE, "!=")                       \
  X(SHL, "<<")                      \
  X(SHR, ">>")                      \
  X(EQ, "=")                        \
  X(PLUSEQ, "+=")                   \
  X(MINUSEQ, "-=")                  \
  X(STAREQ, "*=")                   \
  X(SLASHEQ, "/=")                  \
  X(PERCENTEQ, "%=")                \
  X(AMPEQ, "&=")                    \
  X(PIPEEQ, "|=")                   \
  X(CARETEQ, "^=")                  \
  X(SHLEQ, "<<=")                   \
  X(SHREQ, ">>=")                   \
  KW(ALIGNOF, "alignof")            \
  KW(AND, "and")                    \
  KW(AS, "as")                      \
  KW(BREAK, "break")                \
  KW(CAST, "cast")                  \
  KW(CHECK, "check")                \
  KW(CONTINUE, "continue")          \
  KW(DEF, "def")                    \
  KW(ELIF, "elif")                  \
  KW(ELSE, "else")                  \
  KW(FALSE, "false")                \
  KW(FOR, "for")                    \
  KW(FOREIGN, "foreign")            \
  KW(GLOBAL, "global")              \
  KW(IF, "if")                      \
  KW(IMPORT, "import")              \
  KW(IN, "in")                      \
  KW(NONLOCAL, "nonlocal")          \
  KW(NOT, "not")                    \
  KW(NULL, "null")                  \
  KW(OFFSETOF, "offsetof")          \
  KW(ON, "on")                      \
  KW(OR, "or")                      \
  KW(PASS, "pass")                  \
  KW(PRINT, "print")                \
  KW(RETURN, "return")              \
  KW(SIZEOF, "sizeof")              \
  KW(STRUCT, "struct")              \
  KW(TRUE, "true")                  \
  KW(TYPEOF, "typeof")              \
  KW(WITH, "with")                  \
  TY(BOOL, "bool")                  \
  TY(BYTE, "byte")                  \
  TY(CODEPT, "codept")              \
  TY(C_CHAR, "c_char")              \
  TY(C_OPAQUE, "c_opaque")          \
  TY(DOUBLE, "double")              \
  TY(F16, "f16")                    \
  TY(F32, "f32")                    \
  TY(F64, "f64")                    \
  TY(FLOAT, "float")                \
  TY(I8, "i8")                      \
  TY(I16, "i16")                    \
  TY(I32, "i32")                    \
  TY(I64, "i64")                    \
  TY(INT, "int")                    \
  TY(OPAQUE, "opaque")              \
  TY(SIZE_T, "size_t")              \
  TY(STR, "str")                    \
  TY(U8, "u8")                      \
  TY(U16, "u16")                    \
  TY(U32, "u32")                    \
  TY(U64, "u64")                    \
  TY(UINT, "uint")

// clang-format off
typedef enum TokenKind {
#define LEX_ENUM_(name, str) TK_##name,
  LEX_TOKENS(LEX_ENUM_, LEX_ENUM_, LEX_ENUM_)
#undef LEX_ENUM_
  TK_NUM_KINDS,
  TK_FIRST_KEYWORD = TK_ALIGNOF,
  TK_FIRST_TYPE = TK_BOOL,
  TK_LAST_TYPE = TK_UINT,
} TokenKind;
// clang-format on

// API

// Require input buffers to be padded with >= LEX_PADDING 0 bytes so that the
// lexer can read without bounds checking.
#define LEX_PADDING 16

// Compiler context, which owns all allocations. There's one per compilation,
// and it can't be shared between threads.
struct Ctx {
  jmp_buf* on_error;  // must be set (via setjmp) before lexing

  // Filled in before the longjmp to on_error.
  char err_msg[256];
  const char* err_file;
  const u8* err_buf;
  u32 err_offset;

  // Intern table, using open addressing and linear probing. Each slot is the
  // upper 32 bits of the hash, and the name index + 1 in the lower 32 bits, so
  // that 0 is empty.
  u64* slots;
  u32 slot_mask;
  VEC(Name) names;

  // Decoded copies of string literals that contained escapes.
  VEC(StrRef) strs;
  VEC(u8) strbytes;

  // Every module that's been loaded, and the index of each by canonical name.
  VEC(Module*) modules;
  NameMap module_ids;
};

bool ctx_init(Ctx* ctx);
void ctx_free(Ctx* ctx);

// Line and column are 1-based. They're found by scanning from the start of the
// buffer, so this is only suitable for the error path.
void ctx_error_location(const Ctx* ctx, u32* line, u32* col);

// Records the error in ctx and longjmps to ctx->on_error.
NORETURN void ctx_error(Ctx* ctx,
                        const char* file,
                        const u8* buf,
                        u32 offset,
                        const char* fmt,
                        ...);

// Reports that the token kind `what` was expected instead of the current
// token, which is described by `found`. If the `who` that it would close is on
// an earlier line, the message says which.
NORETURN void ctx_error_unclosed(const LexState* ls,
                                 TokenKind what,
                                 TokenKind who,
                                 u32 who_offset,
                                 const char* found);

// Recursive-descent parser. The only lookahead is the lexer's current token.
// There's no backtracking and no token buffer. It only accepts or rejects.
// There's no code generation yet.
//
// Returns normally if the file is syntactically valid. On the first syntax
// error, the message and location are stored in ctx (see
// ctx_error_location()) and it longjmps to ctx->on_error.
//
// src is the root module of a program. Canonical module names are relative to
// its directory, so a Ctx can only be used to parse one root. Its filename
// has to use '/' to separate directories, as import paths do.
void parse_file(Ctx* ctx, const FileBuf* src);

// Lexer
//
// The lexer is pull-style. The parser calls lex_next() to get a token into
// ls->pos.t. There is no token buffer and no "peek". The grammar was finessed (and
// occasionally slightly hacked) so that the parser never needs to see more
// than a single token.

// Maximum nesting of \( ) string interpolation.
#define LEX_MAX_INTERP 16

// Literal suffixes (e.g. 123i32, 1.5f64). Stored in the bottom nibble of
// Token.flags (TF_SUFFIX_MASK).
typedef enum NumSuffix {
  SUF_NONE,
  SUF_I8,
  SUF_I16,
  SUF_I32,
  SUF_I64,
  SUF_U8,
  SUF_U16,
  SUF_U32,
  SUF_U64,
  SUF_F32,
  SUF_F64,
} NumSuffix;

enum {
  TF_SUFFIX_MASK = 0x0f,

  // Set on STRING* tokens that contained escapes. The de-escaped data is in
  // Ctx.strs at Token.v.str. Otherwise, the content is a view of Token.v.len
  // bytes of the source buffer, starting at offset+1.
  TF_ESCAPED = 0x10,
};

struct Token {
  u8 kind;   // TokenKind
  u8 flags;  // TF_*
  u16 pad_;

  // Normally, this is the byte offset of the token start. This is the only
  // position information saved, errors have to rederive line/col when
  // necessary. For STRING*, offset is the delimiter (either a '"' or the ')'
  // that closed the previous \() interpolation). So, the content of the string
  // always starts at offset+1.
  u32 offset;

  union {
    u64 i;        // kind=INTLIT (always unsigned, sign comes from unary minus)
    double f;     // kind=FLOATLIT
    NameId name;  // kind=NAME/TYPENAME/TYPEPARAM/keywords
    u32 len;      // kind=STRING* and flags !TF_ESCAPED: byte length of content slice
    u32 str;      // kind=STRING* and flags TF_ESCAPED: index into Ctx.strs
  } v;
};

// Token is written in place into LexState.t. There is only ever a single token,
// so the size only currently matters to keep it in L1 as much as possible.
static_assert(sizeof(Token) == 16, "unexpected Token size");

// Interned identifier or keyword.
struct Name {
  const u8* str;  // points into a source buffer (or static, for keywords)
  u32 len;
  u8 kind;          // TK_NAME, TK_TYPENAME, TK_TYPEPARAM, or a keyword kind
  bool is_const;    // kind=TK_NAME: an ALL_CAPS constant
  bool is_builtin;  // kind=TK_NAME: a builtin function such as len
  u64 hash;
  void* binding;  // parser: current symbol for this name, or NULL
};

// Decoded copy of a string literal that contained escapes.
struct StrRef {
  u32 off;  // into Ctx.strbytes
  u32 len;
};

// A source file. buf is followed by LEX_PADDING zero bytes, and must outlive
// the Ctx, because interned names point into it.
struct FileBuf {
  const char* filename;  // for errors
  const u8* buf;
  const u8* end;  // buf + len
};

// A source file, and what's kept from parsing it for the rest of the
// compilation. Modules are owned by the Ctx.
struct Module {
  // The path relative to the root module's directory, without .luv.
  NameId name;
  // A padded copy of the canonical name, which the Name may point into.
  u8* name_buf;
  FileBuf src;
  NameMap globals;  // NameId -> GlobalKind
};

// The part of LexState that changes as the lexer advances, other than the
// interpolation stack. A checkpoint is a copy of it.
struct LexPos {
  const u8* p;  // cursor
  Token t;
  u32 indent;  // current block level, in units of 4 spaces
  u32 pending_dedents;
  u32 bracket_depth;  // > 0: newlines are whitespace
};

struct LexState {
  Ctx* ctx;
  FileBuf src;
  // bracket_depth at each open \(. These aren't in LexPos, so a checkpoint has
  // to be restored at the same depth of interpolation that it was made at.
  u8 interp_sp;
  u32 interp_stack[LEX_MAX_INTERP];
  LexPos pos;
};

// It's an error for the first line with content to be indented. The first
// token isn't read until lex_next() is called.
void lex_init(LexState* ls, Ctx* ctx, const FileBuf* src);

void lex_next(LexState* ls);

// The tables in Ctx that the lexer adds to aren't restored by lex_rewind().
// Re-lexing a name gives the same NameId, but re-lexing an escaped string adds
// a duplicate to Ctx.strs.
void lex_mark(const LexState* ls, LexPos* mark);
void lex_rewind(LexState* ls, const LexPos* mark);

// Whether a and b are at the same place with the same current token. Escaped
// strings are compared by content, because re-lexing one gives it a new index.
bool lex_same_pos_for_testing(const LexState* ls, const LexPos* a, const LexPos* b);

const char* tok_kind_name(TokenKind kind);

// Allocates the intern table and interns the keywords. Returns false if
// allocation fails.
bool lex_intern_init(Ctx* ctx);

// Interns s, which must be followed by at least 7 readable bytes (as source
// text is, because of LEX_PADDING).
NameId lex_intern(Ctx* ctx, const u8* s, u32 len);

static inline const Name* lex_name(const Ctx* ctx, NameId id) {
  return &ctx->names.v[id.idx];
}

// Content of a STRING* token (not NUL-terminated).
const u8* lex_str(const LexState* ls, const Token* t, u32* len);
