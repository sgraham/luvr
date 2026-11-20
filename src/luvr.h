#pragma once

typedef struct Ctx Ctx;
typedef struct FileBuf FileBuf;
typedef struct HashSlot HashSlot;
typedef struct HashTable HashTable;
typedef struct IlData IlData;
typedef struct IlFunc IlFunc;
typedef struct IlSlot IlSlot;
typedef struct IlSourcePos IlSourcePos;
typedef struct LexPos LexPos;
typedef struct LexState LexState;
typedef struct Module Module;
typedef struct Name Name;
typedef struct NameId NameId;
typedef struct NameMap NameMap;
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
#include <float.h>
#include <inttypes.h>
#include <math.h>
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
#  define NOINLINE __declspec(noinline)
#elif COMPILER_CLANG || COMPILER_GCC
#  define NOINLINE __attribute__((noinline))
#endif

#if COMPILER_MSVC
#  define NORETURN __declspec(noreturn)
#elif COMPILER_CLANG || COMPILER_GCC
#  define NORETURN __attribute__((noreturn))
#else
#  error port
#endif

#if COMPILER_MSVC
#  define FALLTHROUGH
#elif COMPILER_CLANG || COMPILER_GCC
#  define FALLTHROUGH __attribute__((fallthrough))
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

#define COUNTOF(a) (sizeof(a) / sizeof((a)[0]))

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

// A hash table using open addressing and linear probing. A slot's position
// is its tag & mask, so growing only needs the slots. The tag can be the
// whole key, or part of a hash with the key stored elsewhere, in which case
// a slot with a matching tag still has to be compared with the key.
struct HashTable {
  HashSlot* v;
  u32 num;
  // One less than a power of 2. The table is kept at most half full, so a
  // probe always reaches an empty slot.
  u32 mask;
};

struct HashSlot {
  u32 tag;  // 0 is empty
  u32 val;
};

// cap must be a power of 2. Returns false if allocation fails.
bool hashtable_init(HashTable* t, u32 cap);
void hashtable_free(HashTable* t);

// Empties t, keeping its allocation.
void hashtable_clear(HashTable* t);

// A tag from a 64-bit hash. 0 is empty, so it's mapped to 1.
static inline u32 hashtable_tag(u64 h) {
  u32 tag = (u32)(h >> 32);
  return tag ? tag : 1;
}

static inline HashSlot* hashtable_probe_(const HashTable* t, u32 tag, u32 pos) {
  for (;;) {
    HashSlot* s = &t->v[pos];
    if (s->tag == tag || s->tag == 0) {
      return s;
    }
    pos = (pos + 1) & t->mask;
  }
}

// Returns the first slot with tag, or the empty slot where it would be
// inserted.
static inline HashSlot* hashtable_first(const HashTable* t, u32 tag) {
  return hashtable_probe_(t, tag, tag & t->mask);
}

// Returns the next slot after s with tag, or the empty slot where it would be
// inserted.
static inline HashSlot* hashtable_next(const HashTable* t, const HashSlot* s, u32 tag) {
  return hashtable_probe_(t, tag, (u32)(s - t->v + 1) & t->mask);
}

// Fills the empty slot s, which was returned by hashtable_first() or
// hashtable_next() for tag. s can't be used afterwards, because the table
// might have grown.
void hashtable_insert(Ctx* ctx, HashTable* t, HashSlot* s, u32 tag, u32 val);

// Index of an interned Name in Ctx.names. It's a struct so that it can't be
// mixed up with offsets or other indices.
struct NameId {
  u32 idx;
};

// A hash table from NameId to u32. The tag is NameId.idx + 1, so a slot with a
// matching tag is the name. A NameId is an index into Ctx.names, allocated in
// the order that names are first interned, so ids are compact, and names that
// are declared together are usually close together. Using the id itself as the
// tag keeps them in nearby slots, and on a table with many entries that's much
// faster than spreading them out with a hash. An array indexed directly by the
// id would avoid probing, but it would have to cover every name in the Ctx,
// and grow as more names are interned.
struct NameMap {
  HashTable table;
};

// cap must be a power of 2. Returns false if allocation fails.
static inline bool namemap_init(NameMap* m, u32 cap) {
  return hashtable_init(&m->table, cap);
}

static inline void namemap_free(NameMap* m) {
  hashtable_free(&m->table);
}

// Sets name's value, adding it if it's new.
void namemap_set(Ctx* ctx, NameMap* m, NameId name, u32 val);

// Returns the value for name, or NULL if it isn't in m.
static inline u32* namemap_find(const NameMap* m, NameId name) {
  HashSlot* s = hashtable_first(&m->table, name.idx + 1);
  return s->tag != 0 ? &s->val : NULL;
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
  TY(CODEPT, "codept")              \
  TY(F32, "f32")                    \
  TY(F64, "f64")                    \
  TY(I8, "i8")                      \
  TY(I16, "i16")                    \
  TY(I32, "i32")                    \
  TY(I64, "i64")                    \
  TY(INT, "int")                    \
  TY(STR, "str")                    \
  TY(U8, "u8")                      \
  TY(U16, "u16")                    \
  TY(U32, "u32")                    \
  TY(U64, "u64")

// clang-format off
typedef enum TokenKind {
#define LEX_ENUM_(name, str) TK_##name,
  LEX_TOKENS(LEX_ENUM_, LEX_ENUM_, LEX_ENUM_)
#undef LEX_ENUM_
  TK_NUM_KINDS,
  TK_FIRST_KEYWORD = TK_ALIGNOF,
  TK_FIRST_TYPE = TK_BOOL,
  TK_LAST_TYPE = TK_U64,
} TokenKind;
// clang-format on

// API

// Require input buffers to be padded with >= LEX_PADDING 0 bytes so that the
// lexer can read without bounds checking.
#define LEX_PADDING 16

// Reads the file at path into a malloc()ed buffer followed by LEX_PADDING zero
// bytes, which the Ctx frees. Returns NULL if the file can't be read.
typedef u8* (*LoadFileFn)(const char* path, size_t* len);

// Called with each function's IL. f is reused for the next function after it
// returns. src is the source of the module that f is in.
typedef void (*FuncDoneFn)(Ctx* ctx, const IlFunc* f, const u8* src);

// The runtime's functions, which are the first symbols in Ctx.syms, in this
// order.
#define RUNTIME_SYMS(X) \
  X(luv_print_i64)      \
  X(luv_print_u64)      \
  X(luv_print_bool)     \
  X(luv_print_str)      \
  X(luv_print_end)

// clang-format off
typedef enum RuntimeSym {
#define RUNTIME_SYM_ENUM_(name) SYM_##name,
  RUNTIME_SYMS(RUNTIME_SYM_ENUM_)
#undef RUNTIME_SYM_ENUM_
  SYM_NUM_RUNTIME,
} RuntimeSym;
// clang-format on

// Compiler context, which owns all allocations. There's one per compilation,
// and it can't be shared between threads.
struct Ctx {
  jmp_buf* on_error;  // must be set (via setjmp) before lexing

  // Filled in before the longjmp to on_error.
  char err_msg[256];
  const char* err_file;
  const u8* err_buf;
  u32 err_offset;

  // Intern table. A slot's tag is from the hash of the name, and its val is
  // the index in names.
  HashTable name_table;
  VEC(Name) names;

  // Decoded copies of string literals that contained escapes.
  VEC(StrRef) strs;
  VEC(u8) strbytes;

  // Every module that's been loaded, and the index of each by canonical name.
  VEC(Module*) modules;
  NameMap module_ids;

  // Reads the root module and imported files. It can't be NULL.
  LoadFileFn load_file;

  // Gets the IL from parse_file_emit(). It's not called if it's NULL.
  FuncDoneFn on_func;

  // Functions that IL constants can refer to, by index.
  VEC(NameId) syms;

  // Read-only bytes, such as string literals, that IL constants can refer to
  // by index.
  VEC(IlData) data;
  VEC(u8) databytes;
  // The number of entries of data that have been printed as IL text.
  u32 data_printed;
};

bool ctx_init(Ctx* ctx);
void ctx_free(Ctx* ctx);

// Returns the new symbol's index.
u32 ctx_sym_add(Ctx* ctx, NameId name);

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
//
// Returns normally if the file is syntactically valid. On the first syntax
// error, the message and location are stored in ctx (see
// ctx_error_location()) and it longjmps to ctx->on_error.
//
// path is the root module of a program, which is read with ctx->load_file,
// as imports are. Canonical module names are relative to its directory, so a
// Ctx can only be used to parse one root. It has to use '/' to separate
// directories, as import paths do.
void parse_file(Ctx* ctx, const char* path);

// The same as parse_file(), and also emits IL for each function.
void parse_file_emit(Ctx* ctx, const char* path);

// Paths

// Returns the length of the directory part of path[0..len), including its
// final '/', or 0 if there isn't one.
u32 path_dir_len(const u8* path, u32 len);

// Returns the file name in path, and its length without .luv in *len.
const char* path_stem(const char* path, u32* len);

// Returns an error message if path[0..len) isn't allowed as an import path, or
// NULL if it is.
const char* path_import_is_invalid(const u8* path, u32 len);

// Removes each ".." in path[0..len) along with the component before it, in
// place, and returns the new length. A ".." with nothing before it to remove is
// kept, so a path above the root is still a consistent name. path can't have
// empty or '.' components.
u32 path_normalize(u8* path, u32 len);

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
  void* binding;    // parser: current symbol for this name, or NULL
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

// The value for a name in Module.globals. The parser packs the kind of global
// into it, and for constants, where the value is.
typedef u32 GlobalInfo;

// A source file, and what's kept from parsing it for the rest of the
// compilation. Modules are owned by the Ctx.
struct Module {
  // The path relative to the root module's directory, without .luv.
  NameId name;
  // A padded copy of the canonical name, which the Name may point into.
  u8* name_buf;
  FileBuf src;  // filename and buf are freed with the Ctx
  // Until it's been parsed. Importing a module that's still loading is an
  // import cycle.
  bool loading;
  NameMap globals;  // NameId -> GlobalInfo
  // The name that each import is bound to -> index in Ctx.modules.
  NameMap imports;
};

// The part of LexState that changes as the lexer advances, other than the
// interpolation stack. A checkpoint is a copy of it.
struct LexPos {
  const u8* p;  // cursor
  Token t;
  u32 indent;  // current block level, in units of 4 spaces
  u32 pending_dedents;
  u32 bracket_depth;  // > 0: newlines are whitespace
  u8 interp_sp;       // number of open \( in interp_stack
};

struct LexState {
  Ctx* ctx;
  FileBuf src;
  // bracket_depth at each open \(. Only the depth is in LexPos. Entries below
  // a mark's top are still open when it's restored, and lex_rewind() restores
  // the top one.
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
// text is, because of LEX_PADDING). The Name points to s, so s has to live as
// long as ctx.
NameId lex_intern_padded(Ctx* ctx, const u8* s, u32 len);

// Interns a NUL-terminated s of at most 64 bytes that isn't padded, such as
// a string literal. s has to live as long as ctx.
NameId lex_intern_safe(Ctx* ctx, const char* s);

static inline const Name* lex_name(const Ctx* ctx, NameId id) {
  return &ctx->names.v[id.idx];
}

// Content of a STRING* token (not NUL-terminated).
const u8* lex_str(const LexState* ls, const Token* t, u32* len);

// IL
//
// The IL for one function at a time. Instructions are appended to a flat
// array, and operands refer to other instructions and to constants by index.

// op:8 | ty:8 | a:24 | b:24
typedef u64 Ins;

// A reference to an instruction or a constant, in the low 24 bits. If
// REF_INS is set, the other 23 bits are an index in IlFunc.ins. Otherwise
// they're an index in IlFunc.consts + 1, so that 0 means no operand. For
// example:
//   0x000000  no operand
//   0x000001  the first constant, IlFunc.consts.v[0]
//   0x000002  the second constant, IlFunc.consts.v[1]
//   0x800000  the first instruction, IlFunc.ins.v[0], dumped as %0
//   0x800005  IlFunc.ins.v[5], dumped as %5
typedef u32 Ref;
#define REF_INS (1u << 23)

// What an instruction's a or b field holds. A Ref of 0 is no operand, so a
// REF operand can be left out, as in RET with no value.
typedef enum IlOperand {
  OPND_NONE,
  OPND_REF,
  OPND_BLOCK,  // an index in IlFunc.block_starts
  OPND_SLOT,   // an index in IlFunc.slots
  OPND_COUNT,  // the number of arguments in the EXTs that follow a CALL
} IlOperand;

// X(name, a, b, has_result), where a and b are IlOperands without OPND_.
#define IL_OPS(X)              \
  X(ADD, REF, REF, true)       \
  X(SUB, REF, REF, true)       \
  X(MUL, REF, REF, true)       \
  X(XOR, REF, REF, true)       \
  X(EQ, REF, REF, true)        \
  X(NE, REF, REF, true)        \
  X(LT, REF, REF, true)        \
  X(LE, REF, REF, true)        \
  X(GT, REF, REF, true)        \
  X(GE, REF, REF, true)        \
  X(ULT, REF, REF, true)       \
  X(ULE, REF, REF, true)       \
  X(UGT, REF, REF, true)       \
  X(UGE, REF, REF, true)       \
  X(SEXT32, REF, NONE, true)   \
  X(ZEXT32, REF, NONE, true)   \
  X(NEG, REF, NONE, true)      \
  X(SLOT, SLOT, NONE, true)    \
  X(LOAD, REF, NONE, true)     \
  X(STORE, REF, REF, false)    \
  X(BLOCK, BLOCK, NONE, false) \
  X(JMP, BLOCK, NONE, false)   \
  X(BR, REF, BLOCK, false)     \
  X(RET, REF, NONE, false)     \
  X(CALL, REF, COUNT, true)    \
  X(EXT, REF, REF, false)

// clang-format off
typedef enum IlOp {
#define IL_ENUM_(name, a, b, has_result) OP_##name,
  IL_OPS(IL_ENUM_)
#undef IL_ENUM_
  OP_NUM_OPS,
} IlOp;
// clang-format on

static_assert(OP_NUM_OPS <= 256, "IlOp doesn't fit in Ins");

// Narrower integers than i32 are only the type of memory accesses and
// extensions. Signedness is part of the operation, not the type.
typedef enum IlType {
  IT_VOID,
  IT_I8,
  IT_U8,
  IT_I16,
  IT_U16,
  IT_I32,
  IT_I64,
  IT_F32,
  IT_F64,
  // Only the type of a constant, whose value is a symbol index. It's used as
  // an i64 address.
  IT_SYM,
  // Only the type of a constant, whose value is an index in Ctx.data. It's
  // used as an i64 address.
  IT_DATA,
  IT_NUM_TYPES,
} IlType;

static_assert(IT_NUM_TYPES <= 256, "IlType doesn't fit in Ins");

static inline Ins ins_make(IlOp op, IlType ty, Ref a, Ref b) {
  ASSERT(a < (1u << 24) && b < (1u << 24));
  return (u64)op | (u64)ty << 8 | (u64)a << 16 | (u64)b << 40;
}
static inline IlOp ins_op(Ins i) {
  return (IlOp)(i & 0xff);
}
static inline IlType ins_type(Ins i) {
  return (IlType)(i >> 8 & 0xff);
}
static inline Ref ins_a(Ins i) {
  return (Ref)(i >> 16 & 0xffffff);
}
static inline Ref ins_b(Ins i) {
  return (Ref)(i >> 40);
}

// A piece of the function's frame, which holds a local.
struct IlSlot {
  NameId name;  // name.idx is IL_SLOT_UNNAMED for a compiler temporary
  IlType type;
};

#define IL_SLOT_UNNAMED UINT32_MAX

// A statement's instructions are from its ins up to the next entry's. A
// statement that emits nothing has an entry with the same ins as the next.
struct IlSourcePos {
  u32 ins;     // index in IlFunc.ins of the statement's first instruction
  u32 offset;  // of the statement in the source
};

struct IlFunc {
  Ctx* ctx;
  NameId name;
  IlType ret;
  VEC(Ins) ins;  // ins.v[i] is the instruction at REF_INS | i
  // The constant at i + 1 is consts.v[i], and its type is const_types.v[i].
  VEC(u64) consts;
  VEC(u8) const_types;  // IlType
  // Interns constants, so that equal constants have the same Ref. A hash
  // table slot's tag is from the hash of the value and type, and its val is
  // the index in consts.
  HashTable const_map;
  // The index in ins of each block's BLOCK, or IL_BLOCK_UNSTARTED if it
  // hasn't been started yet.
  VEC(u32) block_starts;
  VEC(IlSlot) slots;
  // An entry for each statement, in the order they start.
  VEC(IlSourcePos) source_pos;
};

#define IL_BLOCK_UNSTARTED UINT32_MAX

// A block number that hasn't been declared yet.
#define IL_BLOCK_NONE UINT32_MAX

struct IlData {
  u32 off;  // into Ctx.databytes
  u32 len;
};

void il_init(IlFunc* f, Ctx* ctx);
void il_free(IlFunc* f);

// Empties f for the next function, keeping its allocations.
void il_reset(IlFunc* f, NameId name, IlType ret);

Ref il_emit(IlFunc* f, IlOp op, IlType ty, Ref a, Ref b);

// ty is IT_I32 or IT_I64. An i32 constant is stored sign-extended, so that
// both its signed and unsigned spellings are the same constant.
Ref il_const_int(IlFunc* f, IlType ty, u64 v);

Ref il_const_sym(IlFunc* f, u32 sym);

// data is an index in Ctx.data.
Ref il_const_data(IlFunc* f, u32 data);

// Returns the index in Ctx.data of a copy of the n bytes at s.
u32 il_data_add(Ctx* ctx, const u8* s, u32 n);

// Emits a CALL followed by an EXT for each two of the n args. ty is IT_VOID
// if the call has no result.
Ref il_call(IlFunc* f, IlType ty, Ref callee, const Ref* args, u32 n);

// Returns a new block, which can be jumped to before it's started.
u32 il_block_declare(IlFunc* f);

// Starts block b at the end of the instructions.
void il_block_start(IlFunc* f, u32 b);

void il_jump(IlFunc* f, u32 b);

// Goes to block b if cond is 0, and otherwise continues into a new block,
// because a branch is always the last instruction of its block.
void il_branch(IlFunc* f, Ref cond, u32 b);

// Returns a new slot holding a value of type ty. SLOT gives its address,
// which LOAD and STORE use.
u32 il_slot_add(IlFunc* f, IlType ty, NameId name);

// Records that a statement at offset in the source starts at the next
// instruction.
void il_source_pos(IlFunc* f, u32 offset);

// Writes the text form of f's instructions. src is the source that the
// offsets in f->source_pos are in.
void il_dump(const IlFunc* f, const u8* src, FILE* out);

// Writes the text form of Ctx.data[i].
void il_dump_data(const Ctx* ctx, u32 i, FILE* out);
