#pragma once

typedef struct Ctx Ctx;
typedef struct LexState LexState;
typedef struct Name Name;
typedef struct NameId NameId;
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
#  define ASSERT(x) (void)(x)
#endif

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

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
  Name* names;
  u32 num_names, cap_names;

  // Decoded copies of string literals that contained escapes.
  StrRef* strs;
  u32 num_strs, cap_strs;
  u8* strbytes;
  u32 num_strbytes, cap_strbytes;
};

bool ctx_init(Ctx* ctx);
void ctx_free(Ctx* ctx);

// Line and column are 1-based. They're found by scanning from the start of the
// buffer, so this is only suitable for the error path.
void ctx_error_location(const Ctx* ctx, u32* line, u32* col);

// Recursive-descent parser. The only lookahead is the lexer's current token.
// There's no backtracking and no token buffer. It only accepts or rejects.
// There's no code generation yet.
//
// Returns normally if the file is syntactically valid. On the first syntax
// error, the message and location are stored in ctx (see
// ctx_error_location()) and it longjmps to ctx->on_error.
//
// As with lex_init(), buf must have LEX_PADDING zero bytes after len and
// must outlive ctx.
void parse_file(Ctx* ctx, const char* filename, const u8* buf, size_t len);

// Errors and allocation

// realloc() that reports running out of memory as an error.
void* ctx_grow(Ctx* ctx, void* p, size_t bytes);

// Records the error in ctx and longjmps to ctx->on_error.
NORETURN void ctx_error(Ctx* ctx,
                        const char* file,
                        const u8* buf,
                        u32 offset,
                        const char* fmt,
                        ...);

// Lexer
//
// The lexer is pull-style. The parser calls lex_next() to get a token into
// ls->t. There is no token buffer and no "peek". The grammar was finessed (and
// occasionally slightly hacked) so that the parser never needs to see more
// than a single token.

// Maximum nesting of \( ) string interpolation.
#define LEX_MAX_INTERP 16

// Token kinds:
// - X(name, display) for non-keywords
// - KW(name, spelling) for keywords
// - TY(name, spelling) for builtin type keywords
//
// Note: builtin types are [TK_FIRST_TYPE..TK_LAST_TYPE] (so have to be
// contiguous) for comparisons in the parser.
#define LEX_TOKENS(X, KW, TY)       \
  X(EOS, "<eof>")                   \
  X(NEWLINE, "<newline>")           \
  X(INDENT, "<indent>")             \
  X(DEDENT, "<dedent>")             \
  X(NAME, "<name>")                 \
  X(TYPENAME, "<typename>")         \
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
  TK_FIRST_TYPE = TK_BOOL,
  TK_LAST_TYPE = TK_UINT,
} TokenKind;
// clang-format on

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

// Index of an interned Name in Ctx.names. It's a struct so that it can't be
// mixed up with offsets or other indices.
struct NameId {
  u32 idx;
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
    NameId name;  // kind=NAME/TYPENAME/keywords
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
  u8 kind;        // TK_NAME, TK_TYPENAME, or a keyword kind
  bool is_const;  // kind=TK_NAME: an ALL_CAPS constant
  u8 global;      // parser: kind of global in the file being parsed, or 0
  u64 hash;
  void* binding;  // parser: current symbol for this name, or NULL
};

// Decoded copy of a string literal that contained escapes.
struct StrRef {
  u32 off;  // into Ctx.strbytes
  u32 len;
};

struct LexState {
  Ctx* ctx;
  const char* filename;
  const u8* buf;  // start of source, followed by LEX_PADDING zero bytes
  const u8* end;  // buf + len
  const u8* p;    // cursor
  u16 indent;     // current block level, in units of 4 spaces
  u16 pending_dedents;
  u16 bracket_depth;  // > 0: newlines are whitespace
  u8 interp_sp;
  u16 interp_stack[LEX_MAX_INTERP];  // bracket_depth at each open \(
  Token t;
};

// buf must have LEX_PADDING zero bytes after len, and must outlive ctx because
// interned names point into it. It's an error for the first line with content
// to be indented. The first token isn't read until lex_next() is called.
void lex_init(LexState* ls, Ctx* ctx, const char* filename, const u8* buf, size_t len);

void lex_next(LexState* ls);

const char* tok_kind_name(TokenKind kind);

// Reports that the token kind `what` was expected instead of the current
// token, which is described by `found`. If the `who` that it would close is on
// an earlier line, the message says which.
NORETURN void lex_error_unclosed(LexState* ls,
                                 TokenKind what,
                                 TokenKind who,
                                 u32 who_offset,
                                 const char* found);

static inline const Name* lex_name(const Ctx* ctx, NameId id) {
  return &ctx->names[id.idx];
}

// Content of a STRING* token (not NUL-terminated).
const u8* lex_str(const LexState* ls, const Token* t, u32* len);
