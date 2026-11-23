# D4 ★ Telling types from values (the C-declaration problem)

The user requirement is C-style `Type name` declarations. In C this is the
classic ambiguity: `T * x;`, `T(x);`, and the lexer hack. luv60 avoids most of
it by **classifying identifiers lexically** (`categorizer.in.c:8-10`):

- `varname` = starts lowercase → `IDENT_VAR`
- `typename` = CamelCase / single capital → `IDENT_TYPE`. A single capital
  is always a generic type parameter, never a struct name ([D21](D21-generics.md)).
- `constname` = `ALL_CAPS` → `IDENT_CONST`
- builtin types (`int`, `i32`, `str`, `bool`, ...) are keywords.

Options:

- **(a) Keep the naming convention.** `TK_TYPENAME` is decided in the lexer
  for free: track "saw uppercase" and "saw lowercase" bits while scanning the
  name. With that, every type-starting token is recognizable from one token:
  `TK_TYPENAME`, builtin-type keywords, `*`, `[`, `{` (and `def` if function
  types arrive).
- **(b) Symbol-table feedback (the C lexer hack).** Needs declare-before-use
  and couples the lexer to scopes. It breaks out-of-order top-level
  declarations and imports.
- **(c) Keyword-introduced declarations** (`var x int`, `let`). Trivially
  LL(1), but drops the C-style requirement.
- **(d) Postfix annotations** (`x: int`). LL(1) via `NAME ':'`, but again not
  C-style.

**Recommendation: (a).** Two refinements:

1. **Drop `IDENT_CONST` as a token kind.** Constness is semantic and no
   parse decision depends on it. `[BLAH]int` takes an expression in brackets
   anyway. A token kind with no parse use is one more case for every switch.
2. **Drop `IDENT_IMPORT` lexing.** luv60 looks up every lowercase identifier
   in the import dict in the lexer (`token.c:21`). That's a hash probe per
   identifier for a decision the parser can make when it reaches `NAME '.'`
   (see [D5](D05-decl-vs-expr.md)). Removing it makes the lexer context-free.

The convention is the price of LL(1) C-style declarations. It's worth stating
in the language docs as a rule, not a style suggestion.
