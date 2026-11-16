// Recursive-descent parser for luv.
//
// Every decision is made from the current token alone. Where more seems to be
// needed, a common prefix is parsed first, and the decision is based on what
// it turned out to be:
//
//   - `Stuff s` (declaration) vs. `Stuff(1).f()` (expression), and
//     `mod.Type x` vs. `mod.func()`: parse the postfix chain, then look at its
//     shape and the next token.
//   - In a method's parameter list, `self` vs. `mod.Type x`: read the name,
//     then a following '.' means it was the start of a type.
//   - `[x for x in xs]` vs. `[x, y]`: parse the first element, then `for` or
//     `,` decides.
//
// Types are recognized lexically (TK_TYPENAME, builtin type keywords), so no
// decision here consults a symbol table.

#include "luvr.h"

typedef struct ExpDesc ExpDesc;
typedef struct Local Local;
typedef struct Parser Parser;
typedef struct Targets Targets;
typedef struct TypeId TypeId;

// Types are compared by index.
struct TypeId {
  u32 idx;
};

typedef enum FixedType {
  TY_UNKNOWN,        // Not inferred yet. Checks against it always pass.
  TY_UNTYPED_INT,    // An integer constant that hasn't been given a type yet.
  TY_UNTYPED_FLOAT,  // Similarly, a float constant.
  TY_VOID,           // The return type of a function without one.
  TY_BOOL,
  TY_STR,
  // Integer types are contiguous, from TY_FIRST_INT to TY_LAST_INT.
  TY_I32,
  TY_I64,
  TY_U64,
  TY_F32,
  TY_F64,
  TY_FIRST_INT = TY_I32,
  TY_LAST_INT = TY_U64,
} FixedType;

static const TypeId ty_unknown = {TY_UNKNOWN};
static const TypeId ty_untyped_int = {TY_UNTYPED_INT};
static const TypeId ty_untyped_float = {TY_UNTYPED_FLOAT};
static const TypeId ty_void = {TY_VOID};
static const TypeId ty_bool = {TY_BOOL};
static const TypeId ty_str = {TY_STR};
static const TypeId ty_i32 = {TY_I32};
static const TypeId ty_i64 = {TY_I64};
static const TypeId ty_u64 = {TY_U64};
static const TypeId ty_f32 = {TY_F32};
static const TypeId ty_f64 = {TY_F64};

// Locals that can be in scope at once, including those of enclosing functions.
#define MAX_LOCALS 1024

typedef enum LocalKind {
  LK_VAR,
  LK_PARAM,
  LK_CONST,
  // Named in a 'global' statement, so assignments in this function go to the
  // global rather than declaring a local.
  LK_GLOBAL,
} LocalKind;

// A parameter or local variable of the function being parsed, or of one that
// encloses it.
struct Local {
  NameId name;
  TypeId type;
  LocalKind kind;
  // The Name's binding before this was declared. It's restored when the
  // function ends.
  Local* shadowed;
};

typedef enum GlobalKind {
  GK_NONE,
  GK_VAR,
  GK_FUNC,
  GK_TYPE,
  GK_MODULE,
} GlobalKind;

// Minimum size of Module.globals. It must be a power of 2.
#define MIN_GLOBALS 64

struct Parser {
  LexState ls;
  Module* mod;
  Local locals[MAX_LOCALS];
  u32 num_locals;
  u32 func_base;   // first of the current function's entries in locals
  u32 func_depth;  // 0 at top level
  // Bit c - 'A' is set for each type parameter declared by the current
  // struct's list or method's receiver.
  u32 type_params;
  TypeId ret_type;  // of the current function
  bool in_foreign;  // in a foreign function's signature
};

// The syntactical shape of parsing an expression. Used to decide whether
// an expression can be assigned to, used as a statement, or was a type.
typedef enum Shape {
  SH_VALUE,           // anything else: literals, operators, ...
  SH_NAME,            // a bare name: assignable
  SH_LVALUE,          // a field or index: assignable
  SH_CALL,            // a function or method call: valid as a statement
  SH_TYPE,            // TypeName or module.TypeName
  SH_STRUCT_LITERAL,  // Stuff(...): '&' allocates it in the current arena
} Shape;

// What is known about an expression that has been parsed but not yet emitted.
struct ExpDesc {
  Shape shape;
  TypeId type;
};

static void expr(Parser* p);
static void subexpr(Parser* p, int limit, ExpDesc* e);
static void binop_rest(Parser* p, int limit, ExpDesc* e);
static void primary(Parser* p, ExpDesc* e);
static TypeId type(Parser* p);
static void stat(Parser* p);
static void suite(Parser* p);
static void parse_module(Parser* p);

// Token helpers

#define T(p) ((p)->ls.pos.t.kind)

static void next(Parser* p) {
  lex_next(&p->ls);
}

#define error_at(p, offset, ...) \
  ctx_error((p)->ls.ctx, (p)->ls.src.filename, (p)->ls.src.buf, (offset), __VA_ARGS__)

// Describes a type for error messages, as it would follow "not".
static const char* describe_type(TypeId t) {
  switch ((FixedType)t.idx) {
    case TY_UNKNOWN:
      break;
    case TY_UNTYPED_INT:
      return "an integer constant";
    case TY_UNTYPED_FLOAT:
      return "a float constant";
    case TY_VOID:
      return "void";
    case TY_BOOL:
      return "bool";
    case TY_STR:
      return "str";
    case TY_I32:
      return "i32";
    case TY_I64:
      return "int";
    case TY_U64:
      return "u64";
    case TY_F32:
      return "f32";
    case TY_F64:
      return "f64";
  }
  return "an unknown type";
}

// Describes the current token for error messages.
static const char* describe(Parser* p, char* buf, size_t size) {
  const Token* t = &p->ls.pos.t;
  switch (t->kind) {
    case TK_TYPEPARAM:
      snprintf(buf, size, "type parameter '%c'", lex_name(p->ls.ctx, t->v.name)->str[0]);
      return buf;
    case TK_NAME:
    case TK_TYPENAME: {
      const Name* n = lex_name(p->ls.ctx, t->v.name);
      snprintf(buf, size, "'%.*s'", (int)(n->len > 40 ? 40 : n->len), (const char*)n->str);
      return buf;
    }
    case TK_INTLIT:
    case TK_FLOATLIT:
      return "number";
    case TK_STRING:
    case TK_STRING_BEGIN:
      return "string";
    case TK_NEWLINE:
      return "end of line";
    case TK_INDENT:
      return "indent";
    case TK_DEDENT:
      return "dedent";
    case TK_EOS:
      return "end of file";
    default:
      snprintf(buf, size, "'%s'", tok_kind_name(t->kind));
      return buf;
  }
}

NORETURN static void error_expected(Parser* p, const char* what) {
  char buf[64];
  error_at(p, p->ls.pos.t.offset, "Expected %s, found %s.", what, describe(p, buf, sizeof(buf)));
}

static void check(Parser* p, TokenKind kind) {
  if (T(p) != kind) {
    char what[16];
    snprintf(what, sizeof(what), "'%s'", tok_kind_name(kind));
    error_expected(p, what);
  }
  next(p);
}

static bool optional(Parser* p, TokenKind kind) {
  if (T(p) != kind) {
    return false;
  }
  next(p);
  return true;
}

// Like check(), but if the closer is missing and the opener was on another
// line, the error includes the opener's line.
static void check_match(Parser* p, TokenKind what, TokenKind who, u32 who_offset) {
  if (T(p) == what) {
    next(p);
    return;
  }
  char buf[64];
  ctx_error_unclosed(&p->ls, what, who, who_offset, describe(p, buf, sizeof(buf)));
}

static void check_name(Parser* p, const char* what) {
  if (T(p) != TK_NAME) {
    error_expected(p, what);
  }
  next(p);
}

// Like check_name(), but returns the name, and its offset in *at.
static NameId expect_name(Parser* p, const char* what, u32* at) {
  if (T(p) != TK_NAME) {
    error_expected(p, what);
  }
  NameId name = p->ls.pos.t.v.name;
  *at = p->ls.pos.t.offset;
  next(p);
  return name;
}

// Global table

static GlobalKind module_global_kind(const Module* m, NameId name) {
  const u32* kind = namemap_find(&m->globals, name);
  return kind ? (GlobalKind)*kind : GK_NONE;
}

static GlobalKind global_kind(Parser* p, NameId name) {
  return module_global_kind(p->mod, name);
}

static void declare_global(Parser* p, NameId name, GlobalKind kind) {
  namemap_set(p->ls.ctx, &p->mod->globals, name, kind);
}

// Locals
//
// Scope is the whole function: a local is declared by its first assignment
// and is visible from there to the end of the function.

static Name* name_of(Parser* p, NameId name) {
  return &p->ls.ctx->names.v[name.idx];
}

static Local* lookup(Parser* p, NameId name) {
  return (Local*)name_of(p, name)->binding;
}

static Local* declare_local(Parser* p, NameId name, u32 at, LocalKind kind) {
  if (p->num_locals == MAX_LOCALS) {
    error_at(p, at, "Too many local variables.");
  }
  Local* l = &p->locals[p->num_locals++];
  l->name = name;
  l->type = ty_unknown;
  Name* n = name_of(p, name);
  l->kind = kind;
  l->shadowed = (Local*)n->binding;
  n->binding = l;
  return l;
}

// The current function's local for name, ignoring those of enclosing functions.
static Local* own_local(Parser* p, NameId name) {
  Local* l = lookup(p, name);
  return l && l >= &p->locals[p->func_base] ? l : NULL;
}

// Declares name in the current function with type t, unless it's already one
// of the function's locals.
static void declare_if_new(Parser* p, NameId name, u32 at, TypeId t) {
  if (p->func_depth == 0) {
    return;
  }
  if (!own_local(p, name)) {
    Local* l = declare_local(p, name, at, name_of(p, name)->is_const ? LK_CONST : LK_VAR);
    l->type = t;
  }
}

static void pop_locals(Parser* p, u32 base) {
  for (u32 i = p->num_locals; i-- > base;) {
    const Local* l = &p->locals[i];
    name_of(p, l->name)->binding = l->shadowed;
  }
  p->num_locals = base;
}

// Returns the enclosing function's base, to pass to func_close().
static u32 func_open(Parser* p) {
  u32 outer = p->func_base;
  p->func_base = p->num_locals;
  ++p->func_depth;
  return outer;
}

static void func_close(Parser* p, u32 outer) {
  pop_locals(p, p->func_base);
  p->func_base = outer;
  --p->func_depth;
}

static bool is_builtin_type(TokenKind kind) {
  return kind >= TK_FIRST_TYPE && kind <= TK_LAST_TYPE;
}

// Tokens that can only start a type, never an expression statement. A NAME
// can start a type too (module.Type), but that case is handled by parsing the
// postfix chain first.
static bool is_type_start(TokenKind kind) {
  return is_builtin_type(kind) || kind == TK_TYPENAME || kind == TK_TYPEPARAM || kind == TK_STAR ||
         kind == TK_LSQUARE || kind == TK_LBRACE || kind == TK_TYPEOF;
}

// A simple statement ends at a NEWLINE, which it consumes, or at a DEDENT or
// end of file, which it leaves for the enclosing block.
static bool block_follow(TokenKind kind) {
  return kind == TK_DEDENT || kind == TK_EOS;
}

static void end_of_stat(Parser* p) {
  if (T(p) == TK_NEWLINE) {
    next(p);
  } else if (!block_follow(T(p))) {
    error_expected(p, "end of line");
  }
}

// Types
//
//   type -> '*' type
//         | '[' [expr] ']' type        ([]T list, [N]T array)
//         | '{' type '}' type          ({K}V dict)
//         | builtin
//         | TypeName [type_args] | name '.' TypeName [type_args]
//         | 'typeof' '(' expr ')'

// type_args -> '{' type {',' type} '}'
static void type_args(Parser* p) {
  u32 at = p->ls.pos.t.offset;
  next(p);  // '{'
  for (;;) {
    type(p);
    if (!optional(p, TK_COMMA)) {
      break;
    }
  }
  check_match(p, TK_RBRACE, TK_LBRACE, at);
}

static void type_param(Parser* p) {
  if (T(p) != TK_TYPEPARAM) {
    error_expected(p, "type parameter (A-Z)");
  }
  p->type_params |= 1u << (name_of(p, p->ls.pos.t.v.name)->str[0] - 'A');
  next(p);
}

// type_params -> '{' TypeParam {',' TypeParam} '}'
static void type_params(Parser* p) {
  u32 at = p->ls.pos.t.offset;
  next(p);  // '{'
  for (;;) {
    type_param(p);
    if (!optional(p, TK_COMMA)) {
      break;
    }
  }
  check_match(p, TK_RBRACE, TK_LBRACE, at);
}

// Imported modules

// The module that name is bound to by an import, or NULL if it isn't an
// import or a local shadows it.
static const Module* imported_module(Parser* p, NameId name) {
  if (name_of(p, name)->binding) {
    return NULL;
  }
  const u32* idx = namemap_find(&p->mod->imports, name);
  return idx ? p->ls.ctx->modules.v[*idx] : NULL;
}

// Checks that the current token, which follows `mod.`, is a type or variable
// that m defines.
static void check_module_member(Parser* p, const Module* m, NameId mod) {
  const Name* mn = name_of(p, mod);
  const Name* n = name_of(p, p->ls.pos.t.v.name);
  GlobalKind kind = module_global_kind(m, p->ls.pos.t.v.name);
  if (T(p) == TK_TYPENAME && kind != GK_TYPE) {
    error_at(p, p->ls.pos.t.offset, "Type '%.*s' not found in imported module '%.*s'.", (int)n->len,
             (const char*)n->str, (int)mn->len, (const char*)mn->str);
  }
  if (T(p) == TK_NAME && kind != GK_VAR && kind != GK_FUNC) {
    error_at(p, p->ls.pos.t.offset, "Variable '%.*s' not found in imported module '%.*s'.",
             (int)n->len, (const char*)n->str, (int)mn->len, (const char*)mn->str);
  }
}

// The rest of a module.TypeName type, after the '.'. mod is the module's name,
// at offset at.
static void qualified_type_rest(Parser* p, NameId mod, u32 at) {
  const Module* m = imported_module(p, mod);
  if (!m) {
    const Name* n = name_of(p, mod);
    error_at(p, at, "Undefined module '%.*s'.", (int)n->len, (const char*)n->str);
  }
  if (T(p) != TK_TYPENAME) {
    error_expected(p, "type name after module name");
  }
  check_module_member(p, m, mod);
  next(p);
  if (T(p) == TK_LBRACE) {
    type_args(p);
  }
}

static void check_type_param(Parser* p) {
  const Name* n = name_of(p, p->ls.pos.t.v.name);
  u32 i = (u32)(n->str[0] - 'A');
  if (!(p->type_params & (1u << i))) {
    error_at(p, p->ls.pos.t.offset, "Undefined type parameter '%c'.", 'A' + i);
  }
}

static void check_type_name(Parser* p) {
  NameId name = p->ls.pos.t.v.name;
  if (global_kind(p, name) != GK_TYPE) {
    const Name* n = name_of(p, name);
    error_at(p, p->ls.pos.t.offset, "Undefined type '%.*s'.", (int)n->len, (const char*)n->str);
  }
}

static TypeId builtin_type(TokenKind kind) {
  switch (kind) {
    case TK_BOOL:
      return ty_bool;
    case TK_STR:
      return ty_str;
    case TK_I32:
      return ty_i32;
    case TK_I64:
    case TK_INT:
      return ty_i64;
    case TK_U64:
      return ty_u64;
    case TK_F32:
      return ty_f32;
    case TK_F64:
      return ty_f64;
    default:
      return ty_unknown;
  }
}

static TypeId type(Parser* p) {
  u32 at = p->ls.pos.t.offset;
  switch (T(p)) {
    case TK_STAR:
      next(p);
      type(p);
      return ty_unknown;
    case TK_LSQUARE:
      next(p);
      if (T(p) != TK_RSQUARE) {
        expr(p);
      }
      check_match(p, TK_RSQUARE, TK_LSQUARE, at);
      type(p);
      return ty_unknown;
    case TK_LBRACE:
      next(p);
      type(p);
      check_match(p, TK_RBRACE, TK_LBRACE, at);
      type(p);
      return ty_unknown;
    case TK_TYPEPARAM:
      check_type_param(p);
      next(p);
      return ty_unknown;
    case TK_TYPENAME:
      check_type_name(p);
      next(p);
      if (T(p) == TK_LBRACE) {
        type_args(p);
      }
      return ty_unknown;
    case TK_NAME: {
      NameId mod = p->ls.pos.t.v.name;
      next(p);
      check(p, TK_DOT);
      qualified_type_rest(p, mod, at);
      return ty_unknown;
    }
    case TK_TYPEOF:
      next(p);
      at = p->ls.pos.t.offset;
      check(p, TK_LPAREN);
      expr(p);
      check_match(p, TK_RPAREN, TK_LPAREN, at);
      return ty_unknown;
    default: {
      if (!is_builtin_type(T(p))) {
        error_expected(p, "type");
      }
      if (T(p) == TK_INT && p->in_foreign) {
        error_at(p, at, "Can't use int in a foreign declaration, use i32 or i64.");
      }
      TypeId t = builtin_type(T(p));
      next(p);
      return t;
    }
  }
}

// Expressions
//
// Precedence climbing. `not` has lower precedence than comparisons, and
// comparisons don't chain.

typedef enum BinOpr {
  OPR_OR,
  OPR_AND,
  OPR_EQ,
  OPR_NE,
  OPR_LT,
  OPR_LE,
  OPR_GT,
  OPR_GE,
  OPR_IN,
  OPR_NOTIN,
  OPR_BOR,
  OPR_BXOR,
  OPR_BAND,
  OPR_SHL,
  OPR_SHR,
  OPR_ADD,
  OPR_SUB,
  OPR_MUL,
  OPR_DIV,
  OPR_MOD,
  OPR_NONE,
} BinOpr;

// Priority of each binary operator. All are left-associative, so the right
// operand is parsed with the operator's priority as the limit.
static const u8 priority[] = {
    1,                          // or
    2,                          // and
    4,  4,  4,  4, 4, 4, 4, 4,  // comparisons, in, not in
    5,                          // |
    6,                          // ^
    7,                          // &
    8,  8,                      // << >>
    9,  9,                      // + -
    10, 10, 10,                 // * / %
};

enum {
  PRIORITY_NOT = 3,     // unary `not`: binds looser than comparisons
  PRIORITY_UNARY = 11,  // unary - ~ &: binds tighter than any binary operator
};

static BinOpr getbinopr(TokenKind kind) {
  switch (kind) {
    case TK_OR:
      return OPR_OR;
    case TK_AND:
      return OPR_AND;
    case TK_EQEQ:
      return OPR_EQ;
    case TK_NE:
      return OPR_NE;
    case TK_LT:
      return OPR_LT;
    case TK_LE:
      return OPR_LE;
    case TK_GT:
      return OPR_GT;
    case TK_GE:
      return OPR_GE;
    case TK_IN:
      return OPR_IN;
    case TK_NOT:
      return OPR_NOTIN;  // in operator position, `not` must be `not in`
    case TK_PIPE:
      return OPR_BOR;
    case TK_CARET:
      return OPR_BXOR;
    case TK_AMP:
      return OPR_BAND;
    case TK_SHL:
      return OPR_SHL;
    case TK_SHR:
      return OPR_SHR;
    case TK_PLUS:
      return OPR_ADD;
    case TK_MINUS:
      return OPR_SUB;
    case TK_STAR:
      return OPR_MUL;
    case TK_SLASH:
      return OPR_DIV;
    case TK_PERCENT:
      return OPR_MOD;
    default:
      return OPR_NONE;
  }
}

static bool is_comparison(BinOpr op) {
  return op >= OPR_EQ && op <= OPR_NOTIN;
}

// The type that a variable gets when it's declared by assigning a value of
// type t.
static TypeId default_type(TypeId t) {
  switch ((FixedType)t.idx) {
    case TY_UNTYPED_INT:
      return ty_i64;
    case TY_UNTYPED_FLOAT:
      return ty_f64;
    default:
      return t;
  }
}

static bool is_untyped(TypeId t) {
  return t.idx == TY_UNTYPED_INT || t.idx == TY_UNTYPED_FLOAT;
}

static bool is_integer(TypeId t) {
  return t.idx == TY_UNTYPED_INT || (t.idx >= TY_FIRST_INT && t.idx <= TY_LAST_INT);
}

static bool is_float(TypeId t) {
  return t.idx == TY_UNTYPED_FLOAT || t.idx == TY_F32 || t.idx == TY_F64;
}

// Whether a value of type `from` can be used, without a cast, where `to` is
// expected. An integer constant can become a float, but not the reverse, even
// when the float constant is integral.
static bool assignable(TypeId from, TypeId to) {
  return to.idx == TY_UNKNOWN || from.idx == TY_UNKNOWN || to.idx == from.idx ||
         (from.idx == TY_UNTYPED_INT && (is_integer(to) || is_float(to))) ||
         (from.idx == TY_UNTYPED_FLOAT && is_float(to));
}

// Checks that a value can be assigned to something of type `to`.
static void check_assign(Parser* p, TypeId to, const ExpDesc* e, u32 at) {
  if (!assignable(e->type, to)) {
    error_at(p, at, "Can't assign %s to %s.", describe_type(e->type), describe_type(to));
  }
}

// Error messages for arithmetic and bitwise operators, taking the left and
// right operand types in order, except for subtraction, which reads better
// with them swapped.
// clang-format off
static const char* const arith_error[] = {
    [OPR_BOR] = "Can't bitwise or %s with %s.",
    [OPR_BXOR] = "Can't bitwise xor %s with %s.",
    [OPR_BAND] = "Can't bitwise and %s with %s.",
    [OPR_SHL] = "Can't shift %s left by %s.",
    [OPR_SHR] = "Can't shift %s right by %s.",
    [OPR_ADD] = "Can't add %s and %s.",
    [OPR_SUB] = "Can't subtract %s from %s.",
    [OPR_MUL] = "Can't multiply %s by %s.",
    [OPR_DIV] = "Can't divide %s by %s.",
    [OPR_MOD] = "Can't take %s modulo %s.",
};
// clang-format on

// The type of `a op b` for an arithmetic or bitwise op. Operands are numbers
// of the same type, apart from untyped constants. Floats only have +, -, *
// and /. A shift count can be any integer type, and strings can be added.
static TypeId type_for_arith(Parser* p, BinOpr op, TypeId a, TypeId b, u32 at) {
  if (a.idx == TY_UNKNOWN || b.idx == TY_UNKNOWN) {
    return ty_unknown;
  }
  bool is_shift = op == OPR_SHL || op == OPR_SHR;
  bool is_concat = op == OPR_ADD && a.idx == TY_STR && b.idx == TY_STR;
  bool same_type = assignable(a, b) || assignable(b, a);
  bool ints = is_integer(a) && is_integer(b) && (is_shift || same_type);
  bool floats = op >= OPR_ADD && op <= OPR_DIV && same_type && (is_float(a) || is_float(b));
  if (!is_concat && !ints && !floats) {
    bool swap = op == OPR_SUB;
    error_at(p, at, arith_error[op], describe_type(swap ? b : a), describe_type(swap ? a : b));
  }
  if (is_shift || !is_untyped(a)) {
    return a;
  }
  // Two constants give a float constant if either is one.
  return is_untyped(b) && a.idx == TY_UNTYPED_FLOAT ? a : b;
}

// `what` is the start of the error message, naming what needed the bool.
static void expect_bool(Parser* p, const ExpDesc* e, u32 at, const char* what) {
  if (e->type.idx != TY_UNKNOWN && e->type.idx != TY_BOOL) {
    error_at(p, at, "%s expects bool, not %s.", what, describe_type(e->type));
  }
}

// subexpr -> (primary | unop subexpr) { binop subexpr }
// where binop is any binary operator with a priority higher than `limit`.
static void subexpr(Parser* p, int limit, ExpDesc* e) {
  switch (T(p)) {
    case TK_NOT: {
      if (limit >= PRIORITY_NOT) {
        error_at(p, p->ls.pos.t.offset, "'not' needs parentheses here.");
      }
      next(p);
      u32 at = p->ls.pos.t.offset;
      subexpr(p, PRIORITY_NOT, e);
      expect_bool(p, e, at, "'not'");
      e->shape = SH_VALUE;
      e->type = ty_bool;
      break;
    }
    case TK_MINUS:
    case TK_TILDE:
      next(p);
      subexpr(p, PRIORITY_UNARY, e);
      e->shape = SH_VALUE;
      e->type = ty_unknown;
      break;
    case TK_AMP: {
      next(p);
      u32 at = p->ls.pos.t.offset;
      // Only valid if the operand turns out to be a bare name.
      NameId name = p->ls.pos.t.v.name;
      subexpr(p, PRIORITY_UNARY, e);
      const Local* l = e->shape == SH_NAME ? lookup(p, name) : NULL;
      if ((e->shape != SH_NAME && e->shape != SH_LVALUE && e->shape != SH_STRUCT_LITERAL) ||
          (l && l->kind == LK_PARAM)) {
        error_at(p, at, "Can't take the address of non-lvalue.");
      }
      e->shape = SH_VALUE;
      e->type = ty_unknown;
      break;
    }
    default:
      primary(p, e);
      break;
  }
  binop_rest(p, limit, e);
}

// The { binop subexpr } loop of subexpr(). It's separate so that
// sizeof_operand() can continue an expression after already parsing a
// primary.
static void binop_rest(Parser* p, int limit, ExpDesc* e) {
  BinOpr op = getbinopr(T(p));
  while (op != OPR_NONE && priority[op] > limit) {
    u32 at = p->ls.pos.t.offset;
    next(p);
    if (op == OPR_NOTIN) {
      check(p, TK_IN);
    }
    const char* logical = op == OPR_AND ? "'and'" : op == OPR_OR ? "'or'" : NULL;
    if (logical) {
      expect_bool(p, e, at, logical);
    }
    u32 rhs_at = p->ls.pos.t.offset;
    ExpDesc rhs;
    subexpr(p, priority[op], &rhs);
    if (logical) {
      expect_bool(p, &rhs, rhs_at, logical);
    }
    BinOpr nextop = getbinopr(T(p));
    if (is_comparison(op) && is_comparison(nextop)) {
      error_at(p, at, "Comparisons can't be chained. Use 'and' to combine them.");
    }
    if (op >= OPR_EQ && op <= OPR_GE && !assignable(rhs.type, e->type) &&
        !assignable(e->type, rhs.type)) {
      error_at(p, at, "Can't compare %s and %s.", describe_type(e->type), describe_type(rhs.type));
    }
    e->shape = SH_VALUE;
    bool is_bool = is_comparison(op) || logical;
    bool is_arith = op >= OPR_BOR && op <= OPR_MOD;
    if (is_bool) {
      e->type = ty_bool;
    } else if (is_arith) {
      e->type = type_for_arith(p, op, e->type, rhs.type, at);
    } else {
      e->type = ty_unknown;
    }
    op = nextop;
  }
}

static void expr(Parser* p) {
  ExpDesc e;
  subexpr(p, -1, &e);
}

static void cond(Parser* p) {
  u32 at = p->ls.pos.t.offset;
  ExpDesc e;
  subexpr(p, -1, &e);
  expect_bool(p, &e, at, "Condition");
}

// "...\(e)...\(e)..."
static void interpolated_string(Parser* p) {
  next(p);  // STRING_BEGIN
  for (;;) {
    expr(p);
    if (T(p) == TK_STRING_MID) {
      next(p);
    } else if (T(p) == TK_STRING_END) {
      next(p);
      return;
    } else {
      error_expected(p, "')' to end the string interpolation");
    }
  }
}

// Globals
//
// Locals are looked up first, so they shadow globals.

// Reports an error at `at` if name is already a global.
static void check_not_global(Parser* p, NameId name, u32 at) {
  if (global_kind(p, name) != GK_NONE) {
    const Name* n = name_of(p, name);
    error_at(p, at,
             n->is_const ? "Cannot assign a new value to a constant."
                         : "Cannot re-initialize an existing global.");
  }
}

// Declares a function or type, which can only be defined once.
//
// TODO: This is measurable on dumbbench, which has 1.25M top-level functions.
// Part of it is code layout in funcsig() (moving the error report out of line
// recovers most of that), and part is page faults from growing
// Module.globals.
static void declare_definition(Parser* p, NameId name, u32 at, GlobalKind kind) {
  if (global_kind(p, name) != GK_NONE) {
    const Name* n = name_of(p, name);
    error_at(p, at, "Duplicate top-level definition of '%.*s'.", (int)n->len, (const char*)n->str);
  }
  declare_global(p, name, kind);
}

#define MAX_TARGETS 8

// Loop variables, which are bound after the expression that's iterated.
struct Targets {
  u32 n;
  NameId name[MAX_TARGETS];
  u32 at[MAX_TARGETS];
};

// for_targets -> name {',' name}
static void for_targets(Parser* p, Targets* t) {
  t->n = 0;
  for (;;) {
    if (T(p) != TK_NAME) {
      error_expected(p, "loop variable name");
    }
    if (t->n == MAX_TARGETS) {
      error_at(p, p->ls.pos.t.offset, "Too many loop variables.");
    }
    t->name[t->n] = p->ls.pos.t.v.name;
    t->at[t->n] = p->ls.pos.t.offset;
    ++t->n;
    next(p);
    if (!optional(p, TK_COMMA)) {
      break;
    }
  }
}

// Lexes past the first element of a list literal, without parsing it. Stops at
// a 'for' or ',' in the list itself, or at its ']'. Returns whether it stopped
// at a 'for'.
static bool skim_to_for(Parser* p, u32 depth) {
  for (;;) {
    TokenKind k = T(p);
    if (p->ls.pos.bracket_depth < depth || k == TK_EOS) {
      return false;
    }
    if (p->ls.pos.bracket_depth == depth && (k == TK_FOR || k == TK_COMMA)) {
      return k == TK_FOR;
    }
    next(p);
  }
}

// comprehension -> 'for' for_targets 'in' expr { 'for' ... | 'if' expr }
//
// The element comes before the 'for' clauses that bind its names. The clauses
// are parsed first, and then the lexer is rewound to parse the element with
// the loop variables bound. Expressions can't declare new names, so there's
// no parser state to be saved.
static void comprehension(Parser* p, const LexPos* elem, u32 at) {
#if BUILD_DEBUG
  LexPos at_for;
  lex_mark(&p->ls, &at_for);
#endif
  u32 base = p->num_locals;
  while (T(p) == TK_FOR || T(p) == TK_IF) {
    if (optional(p, TK_FOR)) {
      Targets t;
      for_targets(p, &t);
      check(p, TK_IN);
      expr(p);
      // Loop variables are always new, so they shadow any outer name.
      for (u32 i = 0; i < t.n; ++i) {
        LocalKind kind = name_of(p, t.name[i])->is_const ? LK_CONST : LK_VAR;
        declare_local(p, t.name[i], t.at[i], kind);
      }
    } else {
      next(p);
      expr(p);
    }
  }
  LexPos end;
  lex_mark(&p->ls, &end);
  lex_rewind(&p->ls, elem);
  expr(p);
  if (T(p) != TK_FOR) {
    check_match(p, TK_RSQUARE, TK_LSQUARE, at);
  }
  // Both lexed the same text from elem, so a difference means some lexer state
  // isn't restored by lex_rewind().
  ASSERT(lex_same_pos_for_testing(&p->ls, &at_for, &p->ls.pos));
  lex_rewind(&p->ls, &end);
  pop_locals(p, base);
  check_match(p, TK_RSQUARE, TK_LSQUARE, at);
}

// '[' ']' | '[' expr comprehension ']' | '[' expr {',' expr} [','] ']'
static void list_literal(Parser* p) {
  u32 at = p->ls.pos.t.offset;
  u32 depth = p->ls.pos.bracket_depth;
  next(p);
  if (T(p) != TK_RSQUARE) {
    LexPos elem;
    lex_mark(&p->ls, &elem);
    if (skim_to_for(p, depth)) {
      comprehension(p, &elem, at);
      return;
    }
    lex_rewind(&p->ls, &elem);
    expr(p);
    while (optional(p, TK_COMMA)) {
      if (T(p) == TK_RSQUARE) {
        break;  // trailing comma
      }
      expr(p);
    }
  }
  check_match(p, TK_RSQUARE, TK_LSQUARE, at);
}

// '{' '}' | '{' expr ':' expr {',' expr ':' expr} [','] '}'
static void dict_literal(Parser* p) {
  u32 at = p->ls.pos.t.offset;
  next(p);
  if (T(p) != TK_RBRACE) {
    expr(p);
    check(p, TK_COLON);
    expr(p);
    if (T(p) == TK_FOR) {
      error_at(p, p->ls.pos.t.offset, "Dict comprehensions aren't supported yet.");
    }
    while (optional(p, TK_COMMA)) {
      if (T(p) == TK_RBRACE) {
        break;  // trailing comma
      }
      expr(p);
      check(p, TK_COLON);
      expr(p);
    }
  }
  check_match(p, TK_RBRACE, TK_LBRACE, at);
}

// The operand of sizeof or alignof is either a type or an expression.
// Builtin types, '*', 'typeof', '[' and '{' always start a type, so
// sizeof([2]int) is a type, and an expression needs extra parentheses, as in
// sizeof(([1, 2])). A name or TypeName is parsed as a postfix chain, which is
// a type if it's TypeName or module.TypeName, and is otherwise continued as an
// expression.
static void sizeof_operand(Parser* p) {
  TokenKind k = T(p);
  if (is_builtin_type(k) || k == TK_STAR || k == TK_LSQUARE || k == TK_LBRACE || k == TK_TYPEOF) {
    type(p);
  } else if (k == TK_NAME || k == TK_TYPENAME || k == TK_TYPEPARAM) {
    ExpDesc e;
    primary(p, &e);
    if (e.shape != SH_TYPE) {
      binop_rest(p, -1, &e);
    }
  } else {
    expr(p);
  }
}

// `.field=` designators are only allowed in struct literals
// (`Stuff(.a=1, true)`) because functions don't have named parameters.
static void call_args(Parser* p, bool is_struct_literal) {
  u32 at = p->ls.pos.t.offset;
  next(p);  // '('
  if (T(p) != TK_RPAREN) {
    for (;;) {
      if (T(p) == TK_RPAREN) {
        error_at(p, p->ls.pos.t.offset, "Trailing commas aren't allowed in calls.");
      }
      if (T(p) == TK_DOT) {
        if (!is_struct_literal) {
          error_at(p, p->ls.pos.t.offset,
                   "'.name=' arguments are only allowed in struct literals.");
        }
        next(p);
        check_name(p, "field name");
        check(p, TK_EQ);
      }
      expr(p);
      if (!optional(p, TK_COMMA)) {
        break;
      }
    }
  }
  check_match(p, TK_RPAREN, TK_LPAREN, at);
}

// '[' expr ']' (index) or '[' [expr] ':' [expr] ']' (slice)
static void index_or_slice(Parser* p, ExpDesc* e) {
  u32 at = p->ls.pos.t.offset;
  next(p);  // '['
  e->shape = SH_LVALUE;
  if (T(p) != TK_COLON) {
    expr(p);
  }
  if (optional(p, TK_COLON)) {
    e->shape = SH_VALUE;
    if (T(p) != TK_RSQUARE) {
      expr(p);
    }
  }
  check_match(p, TK_RSQUARE, TK_LSQUARE, at);
}

// postfix -> { '.' name | '.' TypeName | '[' index_or_slice ']' | '(' args ')'
//            | type_args }
//
// A '{' can't otherwise follow a name or type, because a dict literal can only
// be at the start of an expression. So after a type it opens type arguments
// (Pair{int, str}). Only structs have type parameters, so after a name it's an
// error.
static void postfix(Parser* p, ExpDesc* e) {
  for (;;) {
    switch (T(p)) {
      case TK_DOT:
        e->type = ty_unknown;
        next(p);
        if (T(p) == TK_NAME) {
          e->shape = SH_LVALUE;
        } else if (T(p) == TK_TYPENAME) {
          // module.TypeName is handled by primary().
          error_at(p, p->ls.pos.t.offset, "Unexpected type name after '.'.");
        } else {
          error_expected(p, "field name");
        }
        next(p);
        break;
      case TK_LSQUARE:
        if (e->shape == SH_TYPE) {
          error_at(p, p->ls.pos.t.offset, "A type can't be indexed.");
        }
        e->type = ty_unknown;
        index_or_slice(p, e);
        break;
      case TK_LPAREN: {
        bool is_struct_literal = e->shape == SH_TYPE;
        call_args(p, is_struct_literal);
        e->shape = is_struct_literal ? SH_STRUCT_LITERAL : SH_CALL;
        e->type = ty_unknown;
        break;
      }
      case TK_LBRACE:
        if (e->shape == SH_NAME || e->shape == SH_LVALUE) {
          error_at(p, p->ls.pos.t.offset, "Type arguments can only follow a type name.");
        }
        if (e->shape != SH_TYPE) {
          return;
        }
        type_args(p);
        if (T(p) == TK_LBRACE) {
          error_at(p, p->ls.pos.t.offset, "Type arguments were given twice.");
        }
        break;
      default:
        return;
    }
  }
}

// primary -> literal | string | name | TypeName | '(' expr ')'
//          | list | dict | sizeof | alignof | offsetof | cast
//          followed by postfix
static TypeId literal_type(Parser* p) {
  switch ((NumSuffix)(p->ls.pos.t.flags & TF_SUFFIX_MASK)) {
    case SUF_NONE:
      return T(p) == TK_INTLIT ? ty_untyped_int : ty_untyped_float;
    case SUF_I32:
      return ty_i32;
    case SUF_I64:
      return ty_i64;
    case SUF_U64:
      return ty_u64;
    case SUF_F32:
      return ty_f32;
    case SUF_F64:
      return ty_f64;
    default:
      return ty_unknown;
  }
}

static void primary(Parser* p, ExpDesc* e) {
  u32 at = p->ls.pos.t.offset;
  e->shape = SH_VALUE;
  e->type = ty_unknown;
  switch (T(p)) {
    case TK_INTLIT:
    case TK_FLOATLIT:
      e->type = literal_type(p);
      next(p);
      break;
    case TK_STRING:
      e->type = ty_str;
      next(p);
      break;
    case TK_TRUE:
    case TK_FALSE:
      e->type = ty_bool;
      next(p);
      break;
    case TK_NULL:
      next(p);
      break;
    case TK_STRING_BEGIN:
      interpolated_string(p);
      e->type = ty_str;
      break;
    case TK_NAME: {
      NameId name = p->ls.pos.t.v.name;
      const Name* n = name_of(p, name);
      GlobalKind kind = n->binding ? GK_NONE : global_kind(p, name);
      const Module* m = kind == GK_MODULE ? imported_module(p, name) : NULL;
      if (m) {
        next(p);
        if (T(p) != TK_DOT) {
          error_at(p, p->ls.pos.t.offset, "Expecting '.' after module name.");
        }
        next(p);
        if (T(p) != TK_NAME && T(p) != TK_TYPENAME) {
          error_expected(p, "name after module name");
        }
        check_module_member(p, m, name);
        e->shape = T(p) == TK_TYPENAME ? SH_TYPE : SH_LVALUE;
        next(p);
        break;
      }
      bool defined = n->binding || n->is_builtin || kind != GK_NONE;
      next(p);
      // A name followed by '=' is an assignment target, which declares the name
      // if it's new.
      if (!defined && T(p) != TK_EQ) {
        error_at(p, at, "Undefined reference to '%.*s'.", (int)n->len, (const char*)n->str);
      }
      if (n->binding) {
        e->type = ((const Local*)n->binding)->type;
      }
      e->shape = SH_NAME;
      break;
    }
    case TK_TYPEPARAM:
      check_type_param(p);
      next(p);
      e->shape = SH_TYPE;
      break;
    case TK_TYPENAME:
      check_type_name(p);
      next(p);
      e->shape = SH_TYPE;
      break;
    case TK_LPAREN:
      next(p);
      subexpr(p, -1, e);
      check_match(p, TK_RPAREN, TK_LPAREN, at);
      e->shape = SH_VALUE;
      break;
    case TK_LSQUARE:
      list_literal(p);
      break;
    case TK_LBRACE:
      dict_literal(p);
      break;
    case TK_SIZEOF:
    case TK_ALIGNOF:
      next(p);
      at = p->ls.pos.t.offset;
      check(p, TK_LPAREN);
      sizeof_operand(p);
      check_match(p, TK_RPAREN, TK_LPAREN, at);
      e->type = ty_i64;
      break;
    case TK_OFFSETOF:
      next(p);
      at = p->ls.pos.t.offset;
      check(p, TK_LPAREN);
      type(p);
      check(p, TK_COMMA);
      check_name(p, "field name");
      check_match(p, TK_RPAREN, TK_LPAREN, at);
      e->type = ty_i64;
      break;
    case TK_CAST: {  // cast T(expr)
      next(p);
      TypeId t = type(p);
      at = p->ls.pos.t.offset;
      check(p, TK_LPAREN);
      expr(p);
      check_match(p, TK_RPAREN, TK_LPAREN, at);
      e->type = t;
      break;
    }
    default:
      error_expected(p, "expression");
  }
  postfix(p, e);
}

// Statements

// The rest of a declaration once its type is parsed: name ['=' expr].
static void decl_rest(Parser* p, bool toplevel, TypeId t) {
  u32 at;
  NameId name = expect_name(p, "variable name", &at);
  if (toplevel) {
    check_not_global(p, name, T(p) == TK_EQ ? p->ls.pos.t.offset : at);
  } else if (own_local(p, name)) {
    const Name* n = name_of(p, name);
    error_at(p, at, "'%.*s' is already declared.", (int)n->len, (const char*)n->str);
  }
  if (optional(p, TK_EQ)) {
    u32 value_at = p->ls.pos.t.offset;
    ExpDesc v;
    subexpr(p, -1, &v);
    check_assign(p, t, &v, value_at);
  }
  if (toplevel) {
    declare_global(p, name, GK_VAR);
  } else {
    declare_if_new(p, name, at, t);
  }
}

static bool is_augassign(TokenKind kind) {
  return kind >= TK_PLUSEQ && kind <= TK_SHREQ;
}

// Indexed by an augmented assignment token's offset from TK_PLUSEQ.
static const BinOpr augassign_opr[] = {
    OPR_ADD,   // +=
    OPR_SUB,   // -=
    OPR_MUL,   // *=
    OPR_DIV,   // /=
    OPR_MOD,   // %=
    OPR_BAND,  // &=
    OPR_BOR,   // |=
    OPR_BXOR,  // ^=
    OPR_SHL,   // <<=
    OPR_SHR,   // >>=
};
static_assert(COUNTOF(augassign_opr) == TK_SHREQ - TK_PLUSEQ + 1,
              "augassign_opr doesn't match tokens");

// A declaration, assignment, or call. The postfix chain is parsed first, and
// then its shape and the next token decide which it is.
//
//   decl   -> type name ['=' expr]
//   assign -> name_or_lvalue ('=' | augop) expr
//   call   -> postfix chain ending in a call
static void decl_or_expr_stat(Parser* p, bool toplevel) {
  TokenKind k = T(p);
  u32 at = p->ls.pos.t.offset;
  if (k == TK_NAME || k == TK_TYPENAME || k == TK_TYPEPARAM) {
    NameId name = p->ls.pos.t.v.name;
    ExpDesc e;
    primary(p, &e);
    if (e.shape == SH_TYPE) {
      decl_rest(p, toplevel, ty_unknown);
      return;
    }
    if (T(p) == TK_EQ || is_augassign(T(p))) {
      if (e.shape != SH_NAME && e.shape != SH_LVALUE) {
        error_at(p, at, "Can't assign to this expression.");
      }
      if (toplevel && T(p) != TK_EQ) {
        error_at(p, p->ls.pos.t.offset, "Augmented assignment isn't allowed at top level.");
      }
      if (e.shape == SH_NAME) {
        if (toplevel) {
          check_not_global(p, name, p->ls.pos.t.offset);
        }
        Local* l = lookup(p, name);
        // An enclosing function's 'global' doesn't apply to this function, so
        // this assignment declares a new local.
        if (l && l < &p->locals[p->func_base] && l->kind == LK_GLOBAL) {
          l = NULL;
        }
        if (l && l < &p->locals[p->func_base]) {
          error_at(p, p->ls.pos.t.offset, "Upvalues are immutable.");
        }
        if (l && l->kind == LK_PARAM) {
          error_at(p, p->ls.pos.t.offset, "Function parameters are immutable.");
        }
        if (l && (l->kind == LK_CONST || (l->kind == LK_GLOBAL && name_of(p, name)->is_const))) {
          error_at(p, p->ls.pos.t.offset, "Cannot assign a new value to a constant.");
        }
      }
      bool is_plain = T(p) == TK_EQ;
      bool declares = e.shape == SH_NAME && is_plain;
      TokenKind op_kind = T(p);
      u32 op_at = p->ls.pos.t.offset;
      next(p);
      u32 value_at = p->ls.pos.t.offset;
      ExpDesc v;
      subexpr(p, -1, &v);
      if (is_plain) {
        check_assign(p, e.type, &v, value_at);
      } else {
        // The result is always the target's type, so it doesn't need to be
        // checked again.
        type_for_arith(p, augassign_opr[op_kind - TK_PLUSEQ], e.type, v.type, op_at);
      }
      // After the expression, so that it can't see the new local.
      if (declares && toplevel) {
        declare_global(p, name, GK_VAR);
      } else if (declares) {
        declare_if_new(p, name, at, default_type(v.type));
      }
      return;
    }
    if (e.shape == SH_CALL) {
      if (toplevel) {
        error_at(p, at, "Calls aren't allowed at top level.");
      }
      return;
    }
    error_at(p, at, "Expression can't be used as a statement.");
  }
  if (is_type_start(k)) {
    TypeId t = type(p);
    decl_rest(p, toplevel, t);
    return;
  }
  if (k == TK_INDENT) {
    error_at(p, at, "Unexpected indent.");
  }
  error_expected(p, "statement");
}

// simple_stat -> 'return' [expr] | 'break' | 'continue' | 'pass'
//              | 'print' expr | 'check' expr
//              | ('global' | 'nonlocal') name {',' name}
//              | decl_or_expr_stat
static void simple_stat(Parser* p) {
  switch (T(p)) {
    case TK_RETURN: {
      u32 return_at = p->ls.pos.t.offset;
      next(p);
      if (T(p) == TK_NEWLINE || block_follow(T(p))) {
        if (p->ret_type.idx != TY_VOID && p->ret_type.idx != TY_UNKNOWN) {
          error_at(p, return_at, "Missing return value in a function returning %s.",
                   describe_type(p->ret_type));
        }
      } else {
        u32 at = p->ls.pos.t.offset;
        ExpDesc e;
        subexpr(p, -1, &e);
        if (p->ret_type.idx == TY_VOID) {
          error_at(p, at, "Can't return a value from a function with no return type.");
        }
        if (!assignable(e.type, p->ret_type)) {
          error_at(p, at, "Can't return %s from a function returning %s.", describe_type(e.type),
                   describe_type(p->ret_type));
        }
      }
      return;
    }
    case TK_BREAK:
    case TK_CONTINUE:
    case TK_PASS:
      next(p);
      return;
    case TK_PRINT:
      next(p);
      expr(p);
      return;
    case TK_CHECK: {
      next(p);
      u32 at = p->ls.pos.t.offset;
      ExpDesc e;
      subexpr(p, -1, &e);
      expect_bool(p, &e, at, "'check'");
      return;
    }
    case TK_GLOBAL:
      next(p);
      for (;;) {
        u32 at;
        NameId name = expect_name(p, "variable name", &at);
        if (global_kind(p, name) == GK_NONE) {
          const Name* n = name_of(p, name);
          error_at(p, at, "Undefined global '%.*s'.", (int)n->len, (const char*)n->str);
        }
        declare_local(p, name, at, LK_GLOBAL);
        if (!optional(p, TK_COMMA)) {
          break;
        }
      }
      return;
    case TK_NONLOCAL:
      next(p);
      for (;;) {
        check_name(p, "variable name");
        if (!optional(p, TK_COMMA)) {
          break;
        }
      }
      return;
    case TK_IF:
    case TK_FOR:
    case TK_WITH:
    case TK_DEF:
      error_at(p, p->ls.pos.t.offset, "'%s' can't follow ':' on the same line.",
               tok_kind_name(T(p)));
    default:
      decl_or_expr_stat(p, /*toplevel=*/false);
      return;
  }
}

// block -> INDENT stat {stat} DEDENT
static void block(Parser* p) {
  next(p);  // INDENT
  for (;;) {
    TokenKind k = T(p);
    stat(p);
    if (block_follow(T(p))) {
      break;
    }
    if (k == TK_RETURN) {
      error_expected(p, "end of block");
    }
  }
  check(p, TK_DEDENT);
}

// The body after a ':'. Either an indented block, or a single simple
// statement on the same line (`if x: return 1`).
static void suite(Parser* p) {
  if (T(p) == TK_INDENT) {
    block(p);
  } else if (T(p) == TK_NEWLINE || block_follow(T(p))) {
    error_expected(p, "an indented block");
  } else {
    simple_stat(p);
    end_of_stat(p);
  }
}

// funcsig -> [type] name '(' [param {',' param}] ')'
// param   -> type name | name    (bare name: a method's first parameter)
//
// The return type is optional, so after `def` a NAME is either the function
// name or the start of a module.Type return type. A following '.' means it's
// a type. A method's `self` and a module.Type parameter are distinguished the
// same way.
//
// A top-level function's name is a global, and a nested function's name is a
// local of the enclosing function. The new function's scope is opened after
// the name, and the return value is for func_close().
static u32 funcsig(Parser* p, bool is_method) {
  NameId name = p->ls.pos.t.v.name;
  u32 at = p->ls.pos.t.offset;
  TypeId ret = ty_void;
  if (T(p) == TK_NAME) {
    next(p);
    if (optional(p, TK_DOT)) {  // module.Type return type
      qualified_type_rest(p, name, at);
      ret = ty_unknown;
      name = expect_name(p, "function name", &at);
    }
  } else {
    if (is_type_start(T(p))) {
      ret = type(p);
    }
    name = expect_name(p, "function name", &at);
  }
  if (!is_method) {
    if (p->func_depth == 0) {
      declare_definition(p, name, at, GK_FUNC);
    } else {
      declare_if_new(p, name, at, ty_unknown);
    }
  }
  if (T(p) == TK_LBRACE) {
    error_at(p, p->ls.pos.t.offset, "Functions can't have type parameters, only structs.");
  }
  u32 outer = func_open(p);
  p->ret_type = ret;

  u32 lparen = p->ls.pos.t.offset;
  check(p, TK_LPAREN);
  if (T(p) != TK_RPAREN) {
    bool first = true;
    for (;;) {
      name = p->ls.pos.t.v.name;
      at = p->ls.pos.t.offset;
      if (T(p) == TK_NAME) {
        next(p);
        if (optional(p, TK_DOT)) {  // module.Type param
          qualified_type_rest(p, name, at);
          name = expect_name(p, "parameter name", &at);
        } else if (!(is_method && first)) {
          error_at(p, p->ls.pos.t.offset, "Parameter needs a type.");
        }
      } else {
        type(p);
        name = expect_name(p, "parameter name", &at);
      }
      declare_local(p, name, at, LK_PARAM);
      first = false;
      if (!optional(p, TK_COMMA)) {
        break;
      }
    }
  }
  check_match(p, TK_RPAREN, TK_LPAREN, lparen);
  return outer;
}

// foreign_decl -> funcsig end   (after 'foreign')
static void foreign_decl(Parser* p, bool is_method) {
  p->in_foreign = true;
  func_close(p, funcsig(p, is_method));
  p->in_foreign = false;
  end_of_stat(p);
}

// funcdef -> funcsig ':' suite   (after 'def')
static void funcdef(Parser* p, bool is_method) {
  TypeId outer_ret = p->ret_type;
  u32 outer = funcsig(p, is_method);
  check(p, TK_COLON);
  suite(p);
  func_close(p, outer);
  p->ret_type = outer_ret;
}

// stat -> 'if' expr ':' suite {'elif' expr ':' suite} ['else' ':' suite]
//       | 'for' for_targets 'in' expr ':' suite
//       | 'with' expr ['as' name] ':' suite
//       | 'def' funcdef
//       | simple_stat end
static void stat(Parser* p) {
  switch (T(p)) {
    case TK_IF:
      next(p);
      cond(p);
      check(p, TK_COLON);
      suite(p);
      while (optional(p, TK_ELIF)) {
        cond(p);
        check(p, TK_COLON);
        suite(p);
      }
      if (optional(p, TK_ELSE)) {
        check(p, TK_COLON);
        suite(p);
      }
      return;
    case TK_FOR: {
      Targets t;
      next(p);
      for_targets(p, &t);
      check(p, TK_IN);
      expr(p);
      for (u32 i = 0; i < t.n; ++i) {
        declare_if_new(p, t.name[i], t.at[i], ty_unknown);
      }
      check(p, TK_COLON);
      suite(p);
      return;
    }
    case TK_WITH:
      next(p);
      expr(p);
      if (optional(p, TK_AS)) {
        u32 at;
        NameId name = expect_name(p, "variable name", &at);
        declare_if_new(p, name, at, ty_unknown);
      }
      check(p, TK_COLON);
      suite(p);
      return;
    case TK_DEF:
      next(p);
      funcdef(p, /*is_method=*/false);
      return;
    default:
      simple_stat(p);
      end_of_stat(p);
      return;
  }
}

// Top level

// struct_body -> INDENT field {field} DEDENT
// field       -> type name ['=' expr] end | 'pass' end
static void struct_body(Parser* p) {
  if (T(p) != TK_INDENT) {
    error_expected(p, "an indented block of fields");
  }
  next(p);
  for (;;) {
    if (!optional(p, TK_PASS)) {
      type(p);
      check_name(p, "field name");
      if (optional(p, TK_EQ)) {
        expr(p);
      }
    }
    end_of_stat(p);
    if (block_follow(T(p))) {
      break;
    }
  }
  check(p, TK_DEDENT);
}

// receiver -> TypeName [type_params] | builtin
//           | '[' ']' TypeName | '{' TypeName '}' TypeName
//
// Methods can only be defined in the receiver type's own module, so the
// receiver is never module.Type. There's no specialization, so the type
// arguments of a generic receiver are always new type parameters. There are
// no methods on pointers, because '.' auto-dereferences, and so p.f() would be
// ambiguous.
static void receiver_param(Parser* p) {
  if (is_type_start(T(p)) && T(p) != TK_TYPEPARAM) {
    error_at(p, p->ls.pos.t.offset, "Methods can't be defined for specific type arguments.");
  }
  type_param(p);
}

static void receiver(Parser* p) {
  u32 at = p->ls.pos.t.offset;
  switch (T(p)) {
    case TK_TYPENAME:
      check_type_name(p);
      next(p);
      if (T(p) == TK_LBRACE) {
        u32 brace = p->ls.pos.t.offset;
        next(p);
        for (;;) {
          receiver_param(p);
          if (!optional(p, TK_COMMA)) {
            break;
          }
        }
        check_match(p, TK_RBRACE, TK_LBRACE, brace);
      }
      return;
    case TK_LSQUARE:
      next(p);
      if (T(p) != TK_RSQUARE) {
        error_at(p, at, "Methods can't be defined on fixed-size arrays.");
      }
      next(p);
      receiver_param(p);
      return;
    case TK_LBRACE:
      next(p);
      receiver_param(p);
      check_match(p, TK_RBRACE, TK_LBRACE, at);
      receiver_param(p);
      return;
    case TK_STAR:
      error_at(p, at, "Methods can't be defined on pointer types.");
    case TK_NAME:
      error_at(p, at, "Methods can only be defined in the type's own module.");
    default:
      if (!is_builtin_type(T(p))) {
        error_expected(p, "type to define methods on");
      }
      next(p);
      return;
  }
}

// toplevel_stat -> 'def' funcdef
//                | 'on' receiver ('def' funcdef | 'foreign' funcsig end)
//                | 'struct' TypeName [type_params] ':' struct_body
//                | 'foreign' funcsig end
//                | decl_or_expr_stat end    (declarations and assignments)
static void toplevel_stat(Parser* p) {
  p->type_params = 0;
  switch (T(p)) {
    case TK_DEF:
      next(p);
      funcdef(p, /*is_method=*/false);
      return;
    case TK_ON:
      next(p);
      receiver(p);
      if (optional(p, TK_FOREIGN)) {
        foreign_decl(p, /*is_method=*/true);
      } else {
        check(p, TK_DEF);
        funcdef(p, /*is_method=*/true);
      }
      return;
    case TK_STRUCT:
      next(p);
      if (T(p) != TK_TYPENAME) {
        error_expected(p, "struct name (a TypeName)");
      }
      declare_definition(p, p->ls.pos.t.v.name, p->ls.pos.t.offset, GK_TYPE);
      next(p);
      if (T(p) == TK_LBRACE) {
        type_params(p);
      }
      check(p, TK_COLON);
      struct_body(p);
      return;
    case TK_FOREIGN:
      next(p);
      foreign_decl(p, /*is_method=*/false);
      return;
    case TK_IMPORT:
      error_at(p, p->ls.pos.t.offset, "Imports must come before other declarations.");
    default:
      decl_or_expr_stat(p, /*toplevel=*/true);
      end_of_stat(p);
      return;
  }
}

// Modules

// Returns len zero bytes followed by LEX_PADDING more, for a name that will be
// interned, because lex_intern() reads past the end.
static u8* name_buf_new(Ctx* ctx, u32 len) {
  u8* buf = (u8*)calloc(len + LEX_PADDING, 1);
  if (!buf) {
    ctx_error(ctx, NULL, NULL, 0, "Out of memory.");
  }
  return buf;
}

// Adds a module to ctx, which takes ownership of name_buf. Its source isn't
// set yet.
static Module* module_new(Ctx* ctx, u8* name_buf, NameId name) {
  Module* m = (Module*)calloc(1, sizeof(Module));
  if (!m) {
    free(name_buf);
    ctx_error(ctx, NULL, NULL, 0, "Out of memory.");
  }
  *vec_push(ctx, &ctx->modules) = m;
  m->name = name;
  m->name_buf = name_buf;
  m->loading = true;
  if (!namemap_init(&m->imports, 16)) {
    ctx_error(ctx, NULL, NULL, 0, "Out of memory.");
  }
  namemap_set(ctx, &ctx->module_ids, name, ctx->modules.num - 1);
  return m;
}

// Returns len bytes for a filename, plus one for the terminating NUL.
static char* filename_new(Ctx* ctx, size_t len) {
  char* filename = (char*)malloc(len + 1);
  if (!filename) {
    ctx_error(ctx, NULL, NULL, 0, "Out of memory.");
  }
  return filename;
}

// Reads m's source from filename, which m takes ownership of. Returns false if
// it can't be read.
static bool module_read(Ctx* ctx, Module* m, char* filename) {
  m->src.filename = filename;
  size_t size;
  u8* buf = ctx->load_file ? ctx->load_file(filename, &size) : NULL;
  if (!buf) {
    return false;
  }
  m->src.buf = buf;
  m->src.end = buf + size;
  // Most of the cost of growing a large table is page faults, so it starts
  // with room for a global every 64 bytes of source. A large calloc() gets
  // pages that the OS has already zeroed, so the pages that are never used
  // aren't touched.
  u32 cap = MIN_GLOBALS;
  while (cap < size / 32) {
    cap *= 2;
  }
  if (!namemap_init(&m->globals, cap)) {
    ctx_error(ctx, filename, NULL, 0, "Out of memory.");
  }
  return true;
}

// Parses m, and then returns p to where it was. Imports are before anything
// else in a file, so p has no other state that could be seen or changed by
// parsing m.
static void parse_import(Parser* p, Module* m) {
  ASSERT(p->num_locals == 0 && p->func_depth == 0 && p->type_params == 0 && p->ls.interp_sp == 0);
  Module* mod = p->mod;
  FileBuf src = p->ls.src;
  LexPos pos = p->ls.pos;
  p->mod = m;
  lex_init(&p->ls, p->ls.ctx, &m->src);
  parse_module(p);
  m->loading = false;
  p->mod = mod;
  p->ls.src = src;
  p->ls.pos = pos;
}

// Reports an import of ctx->modules.v[idx], which is still loading.
NORETURN static void error_import_cycle(Parser* p, u32 idx, u32 at) {
  // The modules that are loading are the chain of imports that led here, and
  // they're in Ctx.modules in the same order.
  Ctx* ctx = p->ls.ctx;
  char chain[1000];
  size_t len = 0;
  for (u32 i = idx; i < ctx->modules.num && len < sizeof(chain); ++i) {
    const Module* m = ctx->modules.v[i];
    if (m->loading) {
      const Name* n = lex_name(ctx, m->name);
      len += (size_t)snprintf(chain + len, sizeof(chain) - len, "%.*s -> ", (int)n->len,
                              (const char*)n->str);
    }
  }
  const Name* n = lex_name(ctx, ctx->modules.v[idx]->name);
  error_at(p, at, "Import cycle: %s%.*s.", chain, (int)n->len, (const char*)n->str);
}

// Returns the index in Ctx.modules of the module that path[0..len) names,
// relative to the current module's directory, and loads and parses it if it's
// new. at is the path's offset, for errors.
static u32 import_module(Parser* p, const u8* path, u32 len, u32 at) {
  Ctx* ctx = p->ls.ctx;
  const char* err = path_import_is_invalid(path, len);
  if (err) {
    error_at(p, at, "%s", err);
  }
  const Name* from = lex_name(ctx, p->mod->name);
  u32 dir = path_dir_len(from->str, from->len);
  u32 name_len = dir + len;
  u8* name_buf = name_buf_new(ctx, name_len);
  memcpy(name_buf, from->str, dir);
  memcpy(name_buf + dir, path, len);
  u32 joined_len = name_len;
  name_len = path_normalize(name_buf, name_len);
  memset(name_buf + name_len, 0, joined_len - name_len);
  NameId name = lex_intern(ctx, name_buf, name_len);
  const u32* idx = namemap_find(&ctx->module_ids, name);
  if (idx) {
    // The module's name was already interned when it was added, so the Name
    // doesn't point into name_buf.
    free(name_buf);
    if (ctx->modules.v[*idx]->loading) {
      error_import_cycle(p, *idx, at);
    }
    return *idx;
  }
  Module* m = module_new(ctx, name_buf, name);
  // Modules that m imports are added after it.
  u32 m_idx = ctx->modules.num - 1;
  // Files are found relative to the root module's directory.
  const char* root = ctx->modules.v[0]->src.filename;
  u32 root_dir = path_dir_len((const u8*)root, (u32)strlen(root));
  char* filename = filename_new(ctx, root_dir + name_len + strlen(".luv"));
  memcpy(filename, root, root_dir);
  memcpy(filename + root_dir, name_buf, name_len);
  memcpy(filename + root_dir + name_len, ".luv", sizeof(".luv"));
  if (!module_read(ctx, m, filename)) {
    error_at(p, at, "Couldn't open import, looking for '%s'.", filename);
  }
  parse_import(p, m);
  return m_idx;
}

// module -> {import} {toplevel_stat} EOS
// import -> 'import' STRING ['as' name] end
static void parse_module(Parser* p) {
  next(p);
  while (optional(p, TK_IMPORT)) {
    // Escaped strings are decoded into Ctx.strbytes, which isn't padded for
    // lex_intern(), and a module name has no reason to contain escapes. This
    // also means that a path can't contain '\', so it always uses '/'.
    if (T(p) != TK_STRING || (p->ls.pos.t.flags & TF_ESCAPED)) {
      error_expected(p, "quoted module name");
    }
    u32 len;
    const u8* s = lex_str(&p->ls, &p->ls.pos.t, &len);
    u32 idx = import_module(p, s, len, p->ls.pos.t.offset);
    // The module is bound to the last component of its path.
    u32 base = path_dir_len(s, len);
    NameId name = lex_intern(p->ls.ctx, s + base, len - base);
    next(p);
    if (optional(p, TK_AS)) {
      u32 at;
      name = expect_name(p, "module alias", &at);
    }
    declare_global(p, name, GK_MODULE);
    namemap_set(p->ls.ctx, &p->mod->imports, name, idx);
    end_of_stat(p);
  }
  while (T(p) != TK_EOS) {
    toplevel_stat(p);
  }
}

// Undoes the file's bindings and frees the Parser.
static void parse_end(Parser* p) {
  pop_locals(p, 0);
  free(p);
}

void parse_file(Ctx* ctx, const char* path) {
  ASSERT(ctx->modules.num == 0);
  // The Parser isn't a local, so that it's still valid after a longjmp.
  Parser* p = (Parser*)malloc(sizeof(Parser));
  if (!p) {
    ctx_error(ctx, path, NULL, 0, "Out of memory.");
  }
  p->num_locals = 0;
  p->func_base = 0;
  p->func_depth = 0;
  p->ret_type = ty_unknown;
  // Names are shared by all files parsed with ctx, so this file's bindings
  // are undone at the end, including after an error.
  jmp_buf* outer = ctx->on_error;
  jmp_buf jb;
  ctx->on_error = &jb;
  if (setjmp(jb)) {
    parse_end(p);
    ctx->on_error = outer;
    longjmp(*outer, 1);
  }
  // The root module's canonical name is its file name, without the directory
  // or .luv.
  u32 len;
  const char* name = path_stem(path, &len);
  u8* name_buf = name_buf_new(ctx, len);
  memcpy(name_buf, name, len);
  p->mod = module_new(ctx, name_buf, lex_intern(ctx, name_buf, len));
  size_t path_len = strlen(path);
  char* filename = filename_new(ctx, path_len);
  memcpy(filename, path, path_len + 1);
  if (!module_read(ctx, p->mod, filename)) {
    ctx_error(ctx, filename, NULL, 0, "Couldn't open '%s'.", filename);
  }
  lex_init(&p->ls, ctx, &p->mod->src);
  parse_module(p);
  p->mod->loading = false;
  parse_end(p);
  ctx->on_error = outer;
}
