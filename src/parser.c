// parser.c: recursive-descent parser for luv, structured after Lua 4.0.1's
// lparser.
//
// Every decision is made from the current token alone. The places that look
// like they need more are handled by parsing a common prefix first and then
// deciding, the way Lua's namestat() parses a variable or call and then looks
// at what it got:
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

#include <stdio.h>

typedef struct Parser {
  LexState ls;
} Parser;

// What a parsed expression turned out to be, as far as syntax can tell. This
// is the acceptance-only stand-in for Lua's expdesc: enough to decide whether
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
  error_at(p, p->ls.t.offset, "expected %s, found %s", what, describe(p, buf, sizeof(buf)));
}

static void check(Parser* p, int kind) {
  if (T(p) != kind) {
    char what[16];
    snprintf(what, sizeof(what), "'%s'", tok_kind_name(kind));
    error_expected(p, what);
  }
  next(p);
}

static int optional(Parser* p, int kind) {
  if (T(p) != kind) {
    return 0;
  }
  next(p);
  return 1;
}

// Like check(), but if the closer is missing and the opener was on another
// line, says where the opener was (from Lua's check_match).
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
    error_at(p, p->ls.t.offset, "expected '%s', found %s", tok_kind_name(what),
             describe(p, buf, sizeof(buf)));
  }
  error_at(p, p->ls.t.offset, "expected '%s' (to close '%s' at line %u), found %s",
           tok_kind_name(what), tok_kind_name(who), who_line, describe(p, buf, sizeof(buf)));
}

static void check_name(Parser* p, const char* what) {
  if (T(p) != TK_NAME) {
    error_expected(p, what);
  }
  next(p);
}

static int is_builtin_type(int kind) {
  return kind >= TK_FIRST_TYPE && kind <= TK_LAST_TYPE;
}

// Tokens that can only start a type, never an expression statement. A NAME
// can start a type too (module.Type), but that case is handled by parsing the
// postfix chain first.
static int is_type_start(int kind) {
  return is_builtin_type(kind) || kind == TK_TYPENAME || kind == TK_STAR || kind == TK_LSQUARE ||
         kind == TK_LBRACE || kind == TK_TYPEOF;
}

// A simple statement ends at a NEWLINE, which it consumes, or at a DEDENT or
// end of file, which it leaves for the enclosing block.
static int block_follow(int kind) {
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
//         | builtin | TypeName | name '.' TypeName
//         | 'typeof' '(' expr ')'

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
      return;
    case TK_NAME:
      next(p);
      check(p, TK_DOT);
      if (T(p) != TK_TYPENAME) {
        error_expected(p, "type name after module name");
      }
      next(p);
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
// Precedence climbing as in Lua's subexpr(), with Python's precedence for
// `not` (below comparisons), and comparisons that don't chain.

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

// Left priority of each binary operator. All are left-associative, so the
// right operand is parsed with the same priority as the limit.
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

static int is_comparison(BinOpr op) {
  return op >= OPR_EQ && op <= OPR_NOTIN;
}

// subexpr -> (primary | unop subexpr) { binop subexpr }
// where binop is any binary operator with a priority higher than `limit`.
static void subexpr(Parser* p, int limit, Shape* sh) {
  switch (T(p)) {
    case TK_NOT:
      if (limit >= PRIORITY_NOT) {
        error_at(p, p->ls.t.offset, "'not' needs parentheses here");
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

// The { binop subexpr } loop of subexpr(), split out so that callers that
// have already parsed a primary (sizeof's type-or-expression) can continue.
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
      error_at(p, at, "comparisons can't be chained (use 'and')");
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

// for_targets -> name {',' name}
static void for_targets(Parser* p) {
  do {
    check_name(p, "loop variable name");
  } while (optional(p, TK_COMMA));
}

// '[' ']' | '[' expr comprehension ']' | '[' expr {',' expr} [','] ']'
//
// Syntactically this is LL(1): after the first element, `for` means a
// comprehension. (Generating code in a single pass will need the element
// re-parsed after the clauses bind the loop variables.)
static void list_literal(Parser* p) {
  u32 at = p->ls.t.offset;
  next(p);
  if (T(p) != TK_RSQUARE) {
    expr(p);
    if (T(p) == TK_FOR) {
      // comprehension -> 'for' targets 'in' expr { 'for' ... | 'if' expr }
      while (T(p) == TK_FOR || T(p) == TK_IF) {
        if (optional(p, TK_FOR)) {
          for_targets(p);
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
      error_at(p, p->ls.t.offset, "dict comprehensions aren't supported yet");
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

// sizeof/alignof '(' type-or-expr ')'. The first token decides:
// builtin types, '*', 'typeof', and '[' or '{' always start a type (so
// sizeof([2]int) is a type, and sizeof(([1, 2])) forces an expression). A
// name is parsed as a postfix chain, which is a type if it turns out to be
// TypeName or module.TypeName, and continues as an expression otherwise.
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

// Call arguments. `.field=` designators are only for struct literals
// (`Stuff(.a=1, true)`): ordinary functions don't have named parameters.
// No trailing comma.
static void call_args(Parser* p, int is_struct_literal) {
  u32 at = p->ls.t.offset;
  next(p);  // '('
  if (T(p) != TK_RPAREN) {
    do {
      if (T(p) == TK_RPAREN) {
        error_at(p, p->ls.t.offset, "trailing comma not allowed in calls");
      }
      if (T(p) == TK_DOT) {
        if (!is_struct_literal) {
          error_at(p, p->ls.t.offset, "'.name=' arguments are only for struct literals");
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

// postfix -> { '.' name | '.' TypeName | '[' index_or_slice ']' | '(' args ')' }
// (Lua's var_or_func_tail)
static void postfix(Parser* p, Shape* sh) {
  for (;;) {
    switch (T(p)) {
      case TK_DOT:
        next(p);
        if (T(p) == TK_NAME) {
          *sh = SH_LVALUE;
        } else if (T(p) == TK_TYPENAME) {
          if (*sh != SH_NAME) {
            error_at(p, p->ls.t.offset, "unexpected type name after '.'");
          }
          *sh = SH_TYPE;  // module.TypeName
        } else {
          error_expected(p, "field name");
        }
        next(p);
        break;
      case TK_LSQUARE:
        if (*sh == SH_TYPE) {
          error_at(p, p->ls.t.offset, "a type can't be indexed");
        }
        index_or_slice(p, sh);
        break;
      case TK_LPAREN: {
        int is_struct_literal = *sh == SH_TYPE;
        call_args(p, is_struct_literal);
        *sh = is_struct_literal ? SH_VALUE : SH_CALL;
        break;
      }
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
static void decl_rest(Parser* p) {
  check_name(p, "variable name");
  if (optional(p, TK_EQ)) {
    expr(p);
  }
}

static int is_augassign(int kind) {
  return kind >= TK_PLUSEQ && kind <= TK_SHREQ;
}

// A declaration, assignment, or call. Lua's namestat(): parse the postfix
// chain, then decide from its shape and the next token.
//
//   decl   -> type name ['=' expr]
//   assign -> name_or_lvalue ('=' | augop) expr
//   call   -> postfix chain ending in a call
static void decl_or_expr_stat(Parser* p, int toplevel) {
  int k = T(p);
  u32 at = p->ls.t.offset;
  if (k == TK_NAME || k == TK_TYPENAME) {
    Shape sh;
    primary(p, &sh);
    if (sh == SH_TYPE) {
      decl_rest(p);
      return;
    }
    if (T(p) == TK_EQ || is_augassign(T(p))) {
      if (sh != SH_NAME && sh != SH_LVALUE) {
        error_at(p, at, "can't assign to this expression");
      }
      if (toplevel && T(p) != TK_EQ) {
        error_at(p, p->ls.t.offset, "augmented assignment isn't allowed at top level");
      }
      next(p);
      expr(p);
      return;
    }
    if (sh == SH_CALL) {
      if (toplevel) {
        error_at(p, at, "calls aren't allowed at top level");
      }
      return;
    }
    error_at(p, at, "expression is not a statement");
  }
  if (is_type_start(k)) {
    type(p);
    decl_rest(p);
    return;
  }
  if (k == TK_INDENT) {
    error_at(p, at, "unexpected indent");
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
      error_at(p, p->ls.t.offset, "'%s' can't follow ':' on the same line", tok_kind_name(T(p)));
    default:
      decl_or_expr_stat(p, 0);
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

// funcsig -> [type] name '(' [param {',' param}] ')'
// param   -> type name | name    (bare name: a method's first parameter)
//
// The return type is optional, so after `def` a NAME is either the function
// name or the start of a module.Type return type; a following '.' decides.
// The same trick separates a method's `self` from a module.Type parameter.
static void funcsig(Parser* p, int is_method) {
  if (T(p) == TK_NAME) {
    next(p);
    if (optional(p, TK_DOT)) {  // module.Type return type
      if (T(p) != TK_TYPENAME) {
        error_expected(p, "type name after module name");
      }
      next(p);
      check_name(p, "function name");
    }
  } else {
    if (is_type_start(T(p))) {
      type(p);
    }
    check_name(p, "function name");
  }

  u32 at = p->ls.t.offset;
  check(p, TK_LPAREN);
  if (T(p) != TK_RPAREN) {
    int first = 1;
    do {
      if (T(p) == TK_NAME) {
        next(p);
        if (optional(p, TK_DOT)) {  // module.Type param
          if (T(p) != TK_TYPENAME) {
            error_expected(p, "type name after module name");
          }
          next(p);
          check_name(p, "parameter name");
        } else if (!(is_method && first)) {
          error_at(p, p->ls.t.offset, "parameter needs a type");
        }
      } else {
        type(p);
        check_name(p, "parameter name");
      }
      first = 0;
    } while (optional(p, TK_COMMA));
  }
  check_match(p, TK_RPAREN, TK_LPAREN, at);
}

// funcdef -> funcsig ':' suite   (after 'def')
static void funcdef(Parser* p, int is_method) {
  funcsig(p, is_method);
  check(p, TK_COLON);
  suite(p);
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
    case TK_FOR:
      next(p);
      for_targets(p);
      check(p, TK_IN);
      expr(p);
      check(p, TK_COLON);
      suite(p);
      return;
    case TK_WITH:
      next(p);
      expr(p);
      if (optional(p, TK_AS)) {
        check_name(p, "variable name");
      }
      check(p, TK_COLON);
      suite(p);
      return;
    case TK_DEF:
      next(p);
      funcdef(p, 0);
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

// toplevel_stat -> 'def' funcdef
//                | 'on' type ('def' funcdef | 'foreign' funcsig end)
//                | 'struct' TypeName ':' struct_body
//                | 'foreign' funcsig end
//                | decl_or_expr_stat end    (declarations and assignments)
static void toplevel_stat(Parser* p) {
  switch (T(p)) {
    case TK_DEF:
      next(p);
      funcdef(p, 0);
      return;
    case TK_ON:
      next(p);
      type(p);
      if (optional(p, TK_FOREIGN)) {
        funcsig(p, 1);
        end_of_stat(p);
      } else {
        check(p, TK_DEF);
        funcdef(p, 1);
      }
      return;
    case TK_STRUCT:
      next(p);
      if (T(p) != TK_TYPENAME) {
        error_expected(p, "struct name (a TypeName)");
      }
      next(p);
      check(p, TK_COLON);
      struct_body(p);
      return;
    case TK_FOREIGN:
      next(p);
      funcsig(p, 0);
      end_of_stat(p);
      return;
    case TK_IMPORT:
      error_at(p, p->ls.t.offset, "imports must come before other declarations");
    default:
      decl_or_expr_stat(p, 1);
      end_of_stat(p);
      return;
  }
}

// module -> {import} {toplevel_stat} EOS
// import -> 'import' STRING ['as' name] end
void parse_file(Ctx* ctx, const char* filename, const u8* buf, size_t len) {
  Parser parser;
  Parser* p = &parser;
  lex_init(&p->ls, ctx, filename, buf, len);
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
