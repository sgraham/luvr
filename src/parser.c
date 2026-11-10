// Recursive-descent parser for luv.
//
// Every decision is made from the current token alone. Where more seems to be
// needed, a common prefix is parsed first, and the decision is based on what
// it turned out to be:
//
//   - `Stuff s` (declaration) vs. `Stuff(1).f()` (expression), and
//     `mod.Type x` vs. `mod.func()`: parse the postfix chain, then look at its
//     Shape and the next token.
//   - In a method's parameter list, `self` vs. `mod.Type x`: read the name,
//     then a following '.' means it was the start of a type.
//   - `[x for x in xs]` vs. `[x, y]`: parse the first element, then `for` or
//     `,` decides.
//
// Types are recognized lexically (TK_TYPENAME, builtin type keywords), so no
// decision here consults a symbol table.

#include "parser.h"

// Locals that can be in scope at once, including those of enclosing functions.
#define MAX_LOCALS 1024

typedef enum LocalKind {
  LK_VAR,
  LK_PARAM,
  LK_CONST,
} LocalKind;

// A parameter or local variable of the function being parsed, or of one that
// encloses it.
typedef struct Local Local;
struct Local {
  NameId name;
  u8 kind;  // LocalKind
  // The Name's binding before this was declared. It's restored when the
  // function ends.
  Local* shadowed;
};

typedef enum GlobalKind {
  GK_NONE,
  GK_VAR,
} GlobalKind;

typedef struct Parser {
  LexState ls;
  Local locals[MAX_LOCALS];
  u32 num_locals;
  u32 func_base;   // first of the current function's entries in locals
  u32 func_depth;  // 0 at top level
  // Names declared as globals by this file, so that Name.global can be
  // cleared at the end.
  NameId* globals;
  u32 num_globals, cap_globals;
  // Counts names that were found as locals. Until there's code generation,
  // this is the only use of a lookup's result.
  u32 num_resolved;
} Parser;

// The syntactical shape of parsing an expression. Used to decide whether
// an expression can be assigned to, used as a statement, or was a type.
typedef enum Shape {
  SH_VALUE,   // anything else: literals, operators, struct literals, ...
  SH_NAME,    // a bare name: assignable
  SH_LVALUE,  // a field or index: assignable
  SH_CALL,    // a function or method call: valid as a statement
  SH_TYPE,    // TypeName or module.TypeName
} Shape;

static void expr(Parser* p);
static void subexpr(Parser* p, int limit, Shape* sh);
static void binop_rest(Parser* p, int limit, Shape* sh);
static void primary(Parser* p, Shape* sh);
static void type(Parser* p);
static void stat(Parser* p);
static void suite(Parser* p);

// Token helpers

#define T(p) ((p)->ls.t.kind)

static void next(Parser* p) {
  lex_next(&p->ls);
}

#define error_at(p, offset, ...) \
  ctx_error((p)->ls.ctx, (p)->ls.filename, (p)->ls.buf, (offset), __VA_ARGS__)

// Describes the current token for error messages.
static const char* describe(Parser* p, char* buf, size_t size) {
  const Token* t = &p->ls.t;
  switch (t->kind) {
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
  error_at(p, p->ls.t.offset, "Expected %s, found %s.", what, describe(p, buf, sizeof(buf)));
}

static void check(Parser* p, int kind) {
  if (T(p) != kind) {
    char what[16];
    snprintf(what, sizeof(what), "'%s'", tok_kind_name(kind));
    error_expected(p, what);
  }
  next(p);
}

static bool optional(Parser* p, int kind) {
  if (T(p) != kind) {
    return false;
  }
  next(p);
  return true;
}

// Like check(), but if the closer is missing and the opener was on another
// line, the error includes the opener's line.
static void check_match(Parser* p, int what, int who, u32 who_offset) {
  if (T(p) == what) {
    next(p);
    return;
  }
  u32 who_line, cur_line, col;
  src_location(p->ls.buf, who_offset, &who_line, &col);
  src_location(p->ls.buf, p->ls.t.offset, &cur_line, &col);
  char buf[64];
  if (who_line == cur_line) {
    error_at(p, p->ls.t.offset, "Expected '%s', found %s.", tok_kind_name(what),
             describe(p, buf, sizeof(buf)));
  }
  error_at(p, p->ls.t.offset, "Expected '%s' (to close '%s' at line %u), found %s.",
           tok_kind_name(what), tok_kind_name(who), who_line, describe(p, buf, sizeof(buf)));
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
  NameId name = p->ls.t.v.name;
  *at = p->ls.t.offset;
  next(p);
  return name;
}

// Locals
//
// Scope is the whole function: a local is declared by its first assignment
// and is visible from there to the end of the function. Only locals are
// resolved so far. Other names aren't looked up anywhere yet.

static Name* name_of(Parser* p, NameId name) {
  return &p->ls.ctx->names[name.idx];
}

static Local* lookup(Parser* p, NameId name) {
  return (Local*)name_of(p, name)->binding;
}

static void declare_local(Parser* p, NameId name, u32 at, bool is_param) {
  if (p->num_locals == MAX_LOCALS) {
    error_at(p, at, "Too many local variables.");
  }
  Local* l = &p->locals[p->num_locals++];
  l->name = name;
  Name* n = name_of(p, name);
  l->kind = is_param ? LK_PARAM : n->is_const ? LK_CONST : LK_VAR;
  l->shadowed = (Local*)n->binding;
  n->binding = l;
}

// Returns the current function's local for name, ignoring those of enclosing
// functions.
static Local* own_local(Parser* p, NameId name) {
  Local* l = lookup(p, name);
  return l && l >= &p->locals[p->func_base] ? l : NULL;
}

// Declares name in the current function, unless it's already one of the
// function's locals.
static void declare_if_new(Parser* p, NameId name, u32 at) {
  if (p->func_depth == 0) {
    return;
  }
  if (!own_local(p, name)) {
    declare_local(p, name, at, false);
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

static bool is_builtin_type(int kind) {
  return kind >= TK_FIRST_TYPE && kind <= TK_LAST_TYPE;
}

// Tokens that can only start a type, never an expression statement. A NAME
// can start a type too (module.Type), but that case is handled by parsing the
// postfix chain first.
static bool is_type_start(int kind) {
  return is_builtin_type(kind) || kind == TK_TYPENAME || kind == TK_STAR || kind == TK_LSQUARE ||
         kind == TK_LBRACE || kind == TK_TYPEOF;
}

// A simple statement ends at a NEWLINE, which it consumes, or at a DEDENT or
// end of file, which it leaves for the enclosing block.
static bool block_follow(int kind) {
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
  u32 at = p->ls.t.offset;
  next(p);  // '{'
  do {
    type(p);
  } while (optional(p, TK_COMMA));
  check_match(p, TK_RBRACE, TK_LBRACE, at);
}

static void type_param(Parser* p) {
  if (T(p) != TK_TYPENAME) {
    error_expected(p, "type parameter name");
  }
  next(p);
}

// type_params -> '{' TypeName {',' TypeName} '}'
static void type_params(Parser* p) {
  u32 at = p->ls.t.offset;
  next(p);  // '{'
  do {
    type_param(p);
  } while (optional(p, TK_COMMA));
  check_match(p, TK_RBRACE, TK_LBRACE, at);
}

// The rest of a module.TypeName type, after the '.'.
static void qualified_type_rest(Parser* p) {
  if (T(p) != TK_TYPENAME) {
    error_expected(p, "type name after module name");
  }
  next(p);
  if (T(p) == TK_LBRACE) {
    type_args(p);
  }
}

static void type(Parser* p) {
  u32 at = p->ls.t.offset;
  switch (T(p)) {
    case TK_STAR:
      next(p);
      type(p);
      return;
    case TK_LSQUARE:
      next(p);
      if (T(p) != TK_RSQUARE) {
        expr(p);
      }
      check_match(p, TK_RSQUARE, TK_LSQUARE, at);
      type(p);
      return;
    case TK_LBRACE:
      next(p);
      type(p);
      check_match(p, TK_RBRACE, TK_LBRACE, at);
      type(p);
      return;
    case TK_TYPENAME:
      next(p);
      if (T(p) == TK_LBRACE) {
        type_args(p);
      }
      return;
    case TK_NAME:
      next(p);
      check(p, TK_DOT);
      qualified_type_rest(p);
      return;
    case TK_TYPEOF:
      next(p);
      at = p->ls.t.offset;
      check(p, TK_LPAREN);
      expr(p);
      check_match(p, TK_RPAREN, TK_LPAREN, at);
      return;
    default:
      if (!is_builtin_type(T(p))) {
        error_expected(p, "type");
      }
      next(p);
      return;
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

static BinOpr getbinopr(int kind) {
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

// subexpr -> (primary | unop subexpr) { binop subexpr }
// where binop is any binary operator with a priority higher than `limit`.
static void subexpr(Parser* p, int limit, Shape* sh) {
  switch (T(p)) {
    case TK_NOT:
      if (limit >= PRIORITY_NOT) {
        error_at(p, p->ls.t.offset, "'not' needs parentheses here.");
      }
      next(p);
      subexpr(p, PRIORITY_NOT, sh);
      *sh = SH_VALUE;
      break;
    case TK_MINUS:
    case TK_TILDE:
    case TK_AMP:
      next(p);
      subexpr(p, PRIORITY_UNARY, sh);
      *sh = SH_VALUE;
      break;
    default:
      primary(p, sh);
      break;
  }
  binop_rest(p, limit, sh);
}

// The { binop subexpr } loop of subexpr(). It's separate so that
// sizeof_operand() can continue an expression after already parsing a
// primary.
static void binop_rest(Parser* p, int limit, Shape* sh) {
  BinOpr op = getbinopr(T(p));
  while (op != OPR_NONE && priority[op] > limit) {
    u32 at = p->ls.t.offset;
    next(p);
    if (op == OPR_NOTIN) {
      check(p, TK_IN);
    }
    Shape rhs;
    subexpr(p, priority[op], &rhs);
    BinOpr nextop = getbinopr(T(p));
    if (is_comparison(op) && is_comparison(nextop)) {
      error_at(p, at, "Comparisons can't be chained. Use 'and' to combine them.");
    }
    *sh = SH_VALUE;
    op = nextop;
  }
}

static void expr(Parser* p) {
  Shape sh;
  subexpr(p, -1, &sh);
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
// A global is marked on its Name rather than having a binding, because the
// number of globals isn't bounded. Locals are looked up first, so they shadow
// globals.

static void declare_global(Parser* p, NameId name, GlobalKind kind) {
  if (p->num_globals == p->cap_globals) {
    p->cap_globals = p->cap_globals ? p->cap_globals * 2 : 64;
    p->globals = (NameId*)ctx_grow(p->ls.ctx, p->globals, p->cap_globals * sizeof(NameId));
  }
  p->globals[p->num_globals++] = name;
  name_of(p, name)->global = (u8)kind;
}

// Reports an error at `at` if name is already a global.
static void check_not_global(Parser* p, NameId name, u32 at) {
  const Name* n = name_of(p, name);
  if (n->global) {
    error_at(p, at,
             n->is_const ? "Cannot assign a new value to a constant."
                         : "Cannot re-initialize an existing global.");
  }
}

static void pop_globals(Parser* p) {
  for (u32 i = 0; i < p->num_globals; ++i) {
    name_of(p, p->globals[i])->global = GK_NONE;
  }
  p->num_globals = 0;
}

#define MAX_TARGETS 8

// Loop variables, which are bound after the expression that's iterated.
typedef struct Targets {
  u32 n;
  NameId name[MAX_TARGETS];
  u32 at[MAX_TARGETS];
} Targets;

// for_targets -> name {',' name}
static void for_targets(Parser* p, Targets* t) {
  t->n = 0;
  do {
    if (T(p) != TK_NAME) {
      error_expected(p, "loop variable name");
    }
    if (t->n == MAX_TARGETS) {
      error_at(p, p->ls.t.offset, "Too many loop variables.");
    }
    t->name[t->n] = p->ls.t.v.name;
    t->at[t->n] = p->ls.t.offset;
    ++t->n;
    next(p);
  } while (optional(p, TK_COMMA));
}

// '[' ']' | '[' expr comprehension ']' | '[' expr {',' expr} [','] ']'
//
// This is LL(1) because a `for` after the first element means it's a
// comprehension. Single pass code generation will need the element to be
// re-parsed after the loop variables are bound.
static void list_literal(Parser* p) {
  u32 at = p->ls.t.offset;
  next(p);
  if (T(p) != TK_RSQUARE) {
    expr(p);
    if (T(p) == TK_FOR) {
      // comprehension -> 'for' targets 'in' expr { 'for' ... | 'if' expr }
      while (T(p) == TK_FOR || T(p) == TK_IF) {
        if (optional(p, TK_FOR)) {
          // The element is parsed before the loop variables, so they aren't
          // bound yet.
          Targets t;
          for_targets(p, &t);
          check(p, TK_IN);
          expr(p);
        } else {
          next(p);
          expr(p);
        }
      }
    } else {
      while (optional(p, TK_COMMA)) {
        if (T(p) == TK_RSQUARE) {
          break;  // trailing comma
        }
        expr(p);
      }
    }
  }
  check_match(p, TK_RSQUARE, TK_LSQUARE, at);
}

// '{' '}' | '{' expr ':' expr {',' expr ':' expr} [','] '}'
static void dict_literal(Parser* p) {
  u32 at = p->ls.t.offset;
  next(p);
  if (T(p) != TK_RBRACE) {
    expr(p);
    check(p, TK_COLON);
    expr(p);
    if (T(p) == TK_FOR) {
      error_at(p, p->ls.t.offset, "Dict comprehensions aren't supported yet.");
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
  int k = T(p);
  if (is_builtin_type(k) || k == TK_STAR || k == TK_LSQUARE || k == TK_LBRACE || k == TK_TYPEOF) {
    type(p);
  } else if (k == TK_NAME || k == TK_TYPENAME) {
    Shape sh;
    primary(p, &sh);
    if (sh != SH_TYPE) {
      binop_rest(p, -1, &sh);
    }
  } else {
    expr(p);
  }
}

// `.field=` designators are only allowed in struct literals
// (`Stuff(.a=1, true)`) because functions don't have named parameters.
static void call_args(Parser* p, bool is_struct_literal) {
  u32 at = p->ls.t.offset;
  next(p);  // '('
  if (T(p) != TK_RPAREN) {
    do {
      if (T(p) == TK_RPAREN) {
        error_at(p, p->ls.t.offset, "Trailing commas aren't allowed in calls.");
      }
      if (T(p) == TK_DOT) {
        if (!is_struct_literal) {
          error_at(p, p->ls.t.offset, "'.name=' arguments are only allowed in struct literals.");
        }
        next(p);
        check_name(p, "field name");
        check(p, TK_EQ);
      }
      expr(p);
    } while (optional(p, TK_COMMA));
  }
  check_match(p, TK_RPAREN, TK_LPAREN, at);
}

// '[' expr ']' (index) or '[' [expr] ':' [expr] ']' (slice)
static void index_or_slice(Parser* p, Shape* sh) {
  u32 at = p->ls.t.offset;
  next(p);  // '['
  *sh = SH_LVALUE;
  if (T(p) != TK_COLON) {
    expr(p);
  }
  if (optional(p, TK_COLON)) {
    *sh = SH_VALUE;
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
// be at the start of an expression. So it always opens type arguments, both
// for a generic type (Pair{int, str}) and for explicitly instantiating a
// generic function (max{u64}).
static void postfix(Parser* p, Shape* sh) {
  for (;;) {
    switch (T(p)) {
      case TK_DOT:
        next(p);
        if (T(p) == TK_NAME) {
          *sh = SH_LVALUE;
        } else if (T(p) == TK_TYPENAME) {
          if (*sh != SH_NAME) {
            error_at(p, p->ls.t.offset, "Unexpected type name after '.'.");
          }
          *sh = SH_TYPE;  // module.TypeName
        } else {
          error_expected(p, "field name");
        }
        next(p);
        break;
      case TK_LSQUARE:
        if (*sh == SH_TYPE) {
          error_at(p, p->ls.t.offset, "A type can't be indexed.");
        }
        index_or_slice(p, sh);
        break;
      case TK_LPAREN: {
        bool is_struct_literal = *sh == SH_TYPE;
        call_args(p, is_struct_literal);
        *sh = is_struct_literal ? SH_VALUE : SH_CALL;
        break;
      }
      case TK_LBRACE:
        if (*sh != SH_TYPE && *sh != SH_NAME && *sh != SH_LVALUE) {
          return;
        }
        type_args(p);
        if (*sh != SH_TYPE) {
          *sh = SH_VALUE;
        } else if (T(p) == TK_LBRACE) {
          error_at(p, p->ls.t.offset, "Type arguments were given twice.");
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
static void primary(Parser* p, Shape* sh) {
  u32 at = p->ls.t.offset;
  *sh = SH_VALUE;
  switch (T(p)) {
    case TK_INTLIT:
    case TK_FLOATLIT:
    case TK_STRING:
    case TK_TRUE:
    case TK_FALSE:
    case TK_NULL:
      next(p);
      break;
    case TK_STRING_BEGIN:
      interpolated_string(p);
      break;
    case TK_NAME:
      p->num_resolved += lookup(p, p->ls.t.v.name) != NULL;
      next(p);
      *sh = SH_NAME;
      break;
    case TK_TYPENAME:
      next(p);
      *sh = SH_TYPE;
      break;
    case TK_LPAREN:
      next(p);
      expr(p);
      check_match(p, TK_RPAREN, TK_LPAREN, at);
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
      at = p->ls.t.offset;
      check(p, TK_LPAREN);
      sizeof_operand(p);
      check_match(p, TK_RPAREN, TK_LPAREN, at);
      break;
    case TK_OFFSETOF:
      next(p);
      at = p->ls.t.offset;
      check(p, TK_LPAREN);
      type(p);
      check(p, TK_COMMA);
      check_name(p, "field name");
      check_match(p, TK_RPAREN, TK_LPAREN, at);
      break;
    case TK_CAST:  // cast T(expr)
      next(p);
      type(p);
      at = p->ls.t.offset;
      check(p, TK_LPAREN);
      expr(p);
      check_match(p, TK_RPAREN, TK_LPAREN, at);
      break;
    default:
      error_expected(p, "expression");
  }
  postfix(p, sh);
}

// Statements

// The rest of a declaration once its type is parsed: name ['=' expr].
static void decl_rest(Parser* p, bool toplevel) {
  u32 at;
  NameId name = expect_name(p, "variable name", &at);
  if (toplevel) {
    check_not_global(p, name, T(p) == TK_EQ ? p->ls.t.offset : at);
  }
  if (optional(p, TK_EQ)) {
    expr(p);
  }
  if (toplevel) {
    declare_global(p, name, GK_VAR);
  } else {
    declare_if_new(p, name, at);
  }
}

static bool is_augassign(int kind) {
  return kind >= TK_PLUSEQ && kind <= TK_SHREQ;
}

// A declaration, assignment, or call. The postfix chain is parsed first, and
// then its Shape and the next token decide which it is.
//
//   decl   -> type name ['=' expr]
//   assign -> name_or_lvalue ('=' | augop) expr
//   call   -> postfix chain ending in a call
static void decl_or_expr_stat(Parser* p, bool toplevel) {
  int k = T(p);
  u32 at = p->ls.t.offset;
  if (k == TK_NAME || k == TK_TYPENAME) {
    NameId name = p->ls.t.v.name;
    Shape sh;
    primary(p, &sh);
    if (sh == SH_TYPE) {
      decl_rest(p, toplevel);
      return;
    }
    if (T(p) == TK_EQ || is_augassign(T(p))) {
      if (sh != SH_NAME && sh != SH_LVALUE) {
        error_at(p, at, "Can't assign to this expression.");
      }
      if (toplevel && T(p) != TK_EQ) {
        error_at(p, p->ls.t.offset, "Augmented assignment isn't allowed at top level.");
      }
      if (sh == SH_NAME) {
        if (toplevel) {
          check_not_global(p, name, p->ls.t.offset);
        }
        Local* l = lookup(p, name);
        if (l && l < &p->locals[p->func_base]) {
          error_at(p, p->ls.t.offset, "Upvalues are immutable.");
        }
        if (l && l->kind == LK_PARAM) {
          error_at(p, p->ls.t.offset, "Function parameters are immutable.");
        }
        if (l && l->kind == LK_CONST) {
          error_at(p, p->ls.t.offset, "Cannot assign a new value to a constant.");
        }
      }
      bool declares = sh == SH_NAME && T(p) == TK_EQ;
      next(p);
      expr(p);
      // After the expression, so that it can't see the new local.
      if (declares && toplevel) {
        declare_global(p, name, GK_VAR);
      } else if (declares) {
        declare_if_new(p, name, at);
      }
      return;
    }
    if (sh == SH_CALL) {
      if (toplevel) {
        error_at(p, at, "Calls aren't allowed at top level.");
      }
      return;
    }
    error_at(p, at, "Expression can't be used as a statement.");
  }
  if (is_type_start(k)) {
    type(p);
    decl_rest(p, toplevel);
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
    case TK_RETURN:
      next(p);
      if (T(p) != TK_NEWLINE && !block_follow(T(p))) {
        expr(p);
      }
      return;
    case TK_BREAK:
    case TK_CONTINUE:
    case TK_PASS:
      next(p);
      return;
    case TK_PRINT:
    case TK_CHECK:
      next(p);
      expr(p);
      return;
    case TK_GLOBAL:
    case TK_NONLOCAL:
      next(p);
      do {
        check_name(p, "variable name");
      } while (optional(p, TK_COMMA));
      return;
    case TK_IF:
    case TK_FOR:
    case TK_WITH:
    case TK_DEF:
      error_at(p, p->ls.t.offset, "'%s' can't follow ':' on the same line.", tok_kind_name(T(p)));
    default:
      decl_or_expr_stat(p, false);
      return;
  }
}

// block -> INDENT stat {stat} DEDENT
static void block(Parser* p) {
  next(p);  // INDENT
  do {
    stat(p);
  } while (!block_follow(T(p)));
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

// funcsig -> [type] name [type_params] '(' [param {',' param}] ')'
// param   -> type name | name    (bare name: a method's first parameter)
//
// The return type is optional, so after `def` a NAME is either the function
// name or the start of a module.Type return type. A following '.' means it's
// a type. A method's `self` and a module.Type parameter are distinguished the
// same way.
//
// A nested function's name is a local of the enclosing function. The new
// function's scope is opened after the name, and the return value is for
// func_close().
static u32 funcsig(Parser* p, bool is_method, bool is_foreign) {
  NameId name = p->ls.t.v.name;
  u32 at = p->ls.t.offset;
  if (T(p) == TK_NAME) {
    next(p);
    if (optional(p, TK_DOT)) {  // module.Type return type
      qualified_type_rest(p);
      name = expect_name(p, "function name", &at);
    }
  } else {
    if (is_type_start(T(p))) {
      type(p);
    }
    name = expect_name(p, "function name", &at);
  }
  if (!is_method) {
    declare_if_new(p, name, at);
  }
  u32 outer = func_open(p);
  if (T(p) == TK_LBRACE) {
    if (is_foreign) {
      error_at(p, p->ls.t.offset, "Foreign functions can't be generic.");
    }
    type_params(p);
  }

  u32 lparen = p->ls.t.offset;
  check(p, TK_LPAREN);
  if (T(p) != TK_RPAREN) {
    bool first = true;
    do {
      name = p->ls.t.v.name;
      at = p->ls.t.offset;
      if (T(p) == TK_NAME) {
        next(p);
        if (optional(p, TK_DOT)) {  // module.Type param
          qualified_type_rest(p);
          name = expect_name(p, "parameter name", &at);
        } else if (!(is_method && first)) {
          error_at(p, p->ls.t.offset, "Parameter needs a type.");
        }
      } else {
        type(p);
        name = expect_name(p, "parameter name", &at);
      }
      declare_local(p, name, at, true);
      first = false;
    } while (optional(p, TK_COMMA));
  }
  check_match(p, TK_RPAREN, TK_LPAREN, lparen);
  return outer;
}

// funcdef -> funcsig ':' suite   (after 'def')
static void funcdef(Parser* p, bool is_method) {
  u32 outer = funcsig(p, is_method, false);
  check(p, TK_COLON);
  suite(p);
  func_close(p, outer);
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
      expr(p);
      check(p, TK_COLON);
      suite(p);
      while (optional(p, TK_ELIF)) {
        expr(p);
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
        declare_if_new(p, t.name[i], t.at[i]);
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
        declare_if_new(p, name, at);
      }
      check(p, TK_COLON);
      suite(p);
      return;
    case TK_DEF:
      next(p);
      funcdef(p, false);
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
  do {
    if (!optional(p, TK_PASS)) {
      type(p);
      check_name(p, "field name");
      if (optional(p, TK_EQ)) {
        expr(p);
      }
    }
    end_of_stat(p);
  } while (!block_follow(T(p)));
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
  if (is_type_start(T(p)) && T(p) != TK_TYPENAME) {
    error_at(p, p->ls.t.offset, "Methods can't be defined for specific type arguments.");
  }
  type_param(p);
}

static void receiver(Parser* p) {
  u32 at = p->ls.t.offset;
  switch (T(p)) {
    case TK_TYPENAME:
      next(p);
      if (T(p) == TK_LBRACE) {
        u32 brace = p->ls.t.offset;
        next(p);
        do {
          receiver_param(p);
        } while (optional(p, TK_COMMA));
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
  switch (T(p)) {
    case TK_DEF:
      next(p);
      funcdef(p, false);
      return;
    case TK_ON:
      next(p);
      receiver(p);
      if (optional(p, TK_FOREIGN)) {
        func_close(p, funcsig(p, true, true));
        end_of_stat(p);
      } else {
        check(p, TK_DEF);
        funcdef(p, true);
      }
      return;
    case TK_STRUCT:
      next(p);
      if (T(p) != TK_TYPENAME) {
        error_expected(p, "struct name (a TypeName)");
      }
      next(p);
      if (T(p) == TK_LBRACE) {
        type_params(p);
      }
      check(p, TK_COLON);
      struct_body(p);
      return;
    case TK_FOREIGN:
      next(p);
      func_close(p, funcsig(p, false, true));
      end_of_stat(p);
      return;
    case TK_IMPORT:
      error_at(p, p->ls.t.offset, "Imports must come before other declarations.");
    default:
      decl_or_expr_stat(p, true);
      end_of_stat(p);
      return;
  }
}

// module -> {import} {toplevel_stat} EOS
// import -> 'import' STRING ['as' name] end
static void parse_module(Parser* p) {
  next(p);
  while (optional(p, TK_IMPORT)) {
    if (T(p) != TK_STRING) {
      error_expected(p, "quoted module name");
    }
    next(p);
    if (optional(p, TK_AS)) {
      check_name(p, "module alias");
    }
    end_of_stat(p);
  }
  while (T(p) != TK_EOS) {
    toplevel_stat(p);
  }
}

// Undoes the file's bindings and frees the Parser.
static void parse_end(Parser* p) {
  pop_locals(p, 0);
  pop_globals(p);
  free(p->globals);
  free(p);
}

void parse_file(Ctx* ctx, const char* filename, const u8* buf, size_t len) {
  // The Parser isn't a local, so that it's still valid after a longjmp.
  // TODO: Freeing on error needs its own setjmp here. Ctx already grows its
  // tables with ctx_grow(), so it'd be better to settle on one owner for
  // allocations that error paths can clean up.
  Parser* p = (Parser*)malloc(sizeof(Parser));
  if (!p) {
    ctx_error(ctx, filename, NULL, 0, "Out of memory.");
  }
  p->num_locals = 0;
  p->func_base = 0;
  p->func_depth = 0;
  p->num_resolved = 0;
  p->globals = NULL;
  p->num_globals = p->cap_globals = 0;
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
  lex_init(&p->ls, ctx, filename, buf, len);
  parse_module(p);
  parse_end(p);
  ctx->on_error = outer;
}
