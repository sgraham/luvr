# D21 Generics

Scope: template-style generics only. Structs take type parameters, and
methods use their receiver's (see the 2026-11-13 revision). No constraints, no value parameters (`Array{int, 4}`), no
specialization. Each distinct instantiation is monomorphized once, on first
use. The body is parsed once at the definition without emitting anything, so
syntax errors are reported at the definition. Each instantiation then rewinds
the lexer to the body and re-parses it with the type parameters bound, using
the same rewind as D10.

**When instantiation happens (2026-11-12, not implemented).** Re-parsing a
body where it's first used, in the middle of another function, would see
that function's locals through `Name.binding`, and its `func_depth`,
`type_params` and return type state. Saving and restoring all of that
means a full parser snapshot, and any field later added to `Parser` could
break it. Instead, instantiations are queued (the body's offset and the
type arguments) and done after the current top-level item ends. The parser
state is then nearly empty (no locals, `func_depth` 0, `type_params` 0), so
an instantiation needs a lexer checkpoint, the type parameters bound to the
arguments, and a flag so the function isn't declared a second time. The
call site already knows the return type from the signature, so the call
can be emitted before the body. Instantiations found while instantiating
are added to the queue. A `static_assert` on `sizeof(Parser)` next to this
code would force a decision about each new parser field. The parser doesn't
need any of this until there's type checking or codegen.

Open: this isn't enough for a generic defined in another module. The
imported module is finished before the importer starts, so its globals
are no longer bound (`Name.global` is per file and cleared in
`parse_end()`). Re-parsing its body in the importer would resolve names
against the importer's globals: a private helper of the defining module
would be undefined, and a name both modules define would bind to the wrong
one. The body needs the defining module's global scope. Either the
definition parse records what each non-local name resolved to and the
instantiation uses that, or globals become per-module tables that can be
switched to, which ties in with replacing `Name.global` with real global
records. Within one module, deferring is enough: globals can't be
redefined, and a name defined after the generic but before the
instantiation was already reported as undefined by the definition parse.

Resolved by D24 (2026-11-13): an instantiation is parsed by the same Parser,
re-pointed at the defining module, so it sees that module's globals.

**Decided (2026-11-08, provisional): `{}`.** The user expects to revisit it
after writing code that uses generics.

```
struct Pair{A, B}:
    A first
    B second

List{[]int} xs = ...
p = Pair{int, str}(.first=1, .second="x")
on Pair{A, B} def A first(self): ...
```

Generic functions (`def T max{T}(...)`, `max{u64}(a, b)`, `f = max{int}`)
were in the original decision, and were removed on 2026-11-13.

Why it's LL(1): a `{` can't otherwise follow a name, a field or a type,
because dict literals and the `{K}V` dict type only appear at the start of an
expression or type. So `{` after `TYPENAME`, `name.TypeName`, a NAME or an
lvalue always opens type arguments. That holds in type position (`type()`)
and in expression position (`postfix()`, where `SH_TYPE`, `SH_NAME` and
`SH_LVALUE` can be followed by type arguments). Explicit call-site type
arguments are decided in `postfix()`, so there's no precedence problem. `{`
already counts as a bracket in the lexer, so long argument lists can wrap
across lines. `}}` is two tokens, so nesting needs no special case. Type
arguments are types only, never expressions.

**Revised (2026-11-12): type parameters are single capital letters.** A
type parameter is exactly one capital letter, `[A-Z]`, and a single capital
letter is always a type parameter. Struct names need at least two letters
(`Elem`, not `T`). The syntax above is unchanged.

- Every type parameter is still declared in a `{...}` list: on the struct
  (`struct Map{K, V}:`), on the function (`def T max{T}`), or on a method's
  receiver (`on List{T} def ...`). The list gives the order of explicit
  type arguments (`Map{str, int}`, `max{u64}(...)`). A single capital that
  isn't declared by an enclosing list is an error ("Undefined type parameter
  'U'."), including in a body. A nested function sees its enclosing
  function's parameters.
- Binding is single-pass. Struct fields and function parameters and bodies
  all come after the list that declares their type parameters. Only a
  function's return type comes first. Because a type parameter is known from
  its spelling, the return type doesn't need resolving where it's parsed.
  The parser records each one used in a 26-bit mask (bit `c - 'A'`), and
  checks the mask against the declared parameters once the list (and the
  receiver, for a method) has been parsed.
- Type parameters and real types can't be confused, so "Undefined type"
  never needs to know about generics. Generic code is also easy to spot
  when reading. The cost is losing descriptive parameter names (`Elem`,
  `Key`), which would be checked as real types instead.
- The lexer classifies a name once per distinct identifier (D4), so telling
  `T` from `Tree` costs nothing at each use.
- Single capitals are currently `TK_TYPENAME`, not constants, so no constant
  spellings are lost. No test here or in luv60 has a single-letter struct.

**Revised (2026-11-13): no free generic functions.** A method is a free
function with nicer syntax and a namespace, nothing more: `xyz.f(5)` is
`Xyz.f(&xyz, 5)`, and `Xyz{int}.f` is a nicer spelling of a mangled
`Xyz$int$_f`. So a method on a generic struct already is a generic
function, and instantiating its body is the same work as instantiating a
free generic function. What this revision restricts is where type
arguments come from.

- Type parameters are only declared by a struct's list
  (`struct Pair{A, B}`) or a method's receiver (`on List{T}`). Functions
  and methods have no list of their own. A method's type parameters are
  exactly its receiver's: `on List{T} def append{T}(...)` would be a
  redundant second list, and `on List{T} def map{U}(...)` adds a parameter
  the receiver can't supply, which makes it a free generic function again.
- So type arguments are never inferred from function arguments. In
  `xs.append(1)`, `T` is read off the declared type of `xs` (`List{int}`,
  or `*List{int}` for `[]int`), which is a one-sided structural match. In
  `List{int}.new(5)` they're written on the type. This removes inference
  across arguments, literals taking their type from the other arguments
  (`max(x, 1)`), and the error for mismatched concrete types.
- Type arguments only follow a type: `{` after a name or a value in
  `postfix()` is an error, so `max{u64}(...)` and `f = max{int}` are gone.
- A method's return type comes after its receiver, so type parameters are
  always declared before they're used, and the 26-bit return type mask
  described above isn't needed.
- Instantiation is unchanged: method bodies are still re-parsed per
  instantiation, queued to the end of the current top-level item, and the
  cross-module problem is still open. It matters early, because the most
  used generic methods are in the prelude (`[]T` is `*List{T}`).
- Lost: functions with no natural receiver. `max`/`min` can be builtins,
  or methods on the builtin types in the prelude (`a.min(b)`, D22a).
  `sort(xs)` becomes `xs.sort()`, `make{T}(n)` becomes `List{T}.new(n)`,
  and comprehensions cover most of `map`/`filter`. `swap{T}(*T, *T)` and
  helpers that span two generic types (`zip` over `List{A}` and `List{B}`)
  have no good form, and need concrete versions.
- Adding free generic functions back later doesn't break any code. The
  next step would be free generic functions that always take explicit type
  arguments (`swap{int}(&a, &b)`), which still needs no inference, but
  brings back `{` after a name in `postfix()`. Inference would come after
  that, if it's needed.

Rules in the prototype (the parts about function lists, the return type
mask and call-site inference are superseded by the 2026-11-13 revision):

- Type parameters (`struct Pair{A, B}`, `def T max{T}`) must be single
  capital letters (see the revision above). Builtins, longer type names and
  `mod.Type` are rejected.
- Generic function parameters come after the function name, matching
  structs. The return type can use `T` before `{T}` declares it, and is
  checked after the list with the mask described above.
- `foreign` functions can't be generic.
- Type arguments can't be given twice (`List{int}{str}`), and can't follow
  a call or other value (`f(){int}`).
- No trailing comma (D19a) and no empty `{}`.
- Default at call sites: type arguments are inferred from the arguments.
  Literals take their type from the other arguments (`max(x, 1)`).
  Different concrete types (`u32` and `u64`) are an error, fixed by `cast`,
  a literal suffix, or explicit `max{u64}(...)`.
- Open semantic questions: what `on Pair{A, B}` binds, whether `on
  Pair{int, int}` (specialization) is allowed, and whether a type argument
  can be inferred from the expected type (`List{int} xs = make()`), which a
  single pass does know first.
- Open (2026-11-12, mostly moot since 2026-11-13, when functions lost
  their own lists): an error for unused type parameters. In a function's
  own list (`def f{T}()` with no `T` in the return type, parameters or
  body), every instantiation is the same code and `T` can't be inferred, so
  it's probably a leftover. It would be checked at `func_close()` with a
  mask of the parameters used, next to the declared mask. A use in a nested
  function counts for the enclosing one, and a use only in the return type
  (`def List{T} make{T}(int n)`) counts. A method's receiver list is exempt,
  because it mirrors the struct's and most methods don't need every
  parameter. A struct's list is exempt too, because an unused parameter
  makes a tag type: `struct Id{T}: int value` gives `Id{User}` and
  `Id{Order}` as distinct types with the same layout.
- Not covered by tests yet (2026-11-12), since type parameters became their
  own token (`TK_TYPEPARAM`): `mod.T` is rejected ("Expected type name after
  module name, found type parameter 'T'."). `T{int}` as a type is an error,
  but `T{int}(...)` in an expression is still accepted by postfix.
  `on List{Elem}` reports "Methods can't be defined for specific type
  arguments."

Known wart: braces now mean both generics and dicts. `{K}V` inside type
arguments reads badly: `List{{str}int}`. The user's inclination is to later
replace both `[]T` and `{K}V` with more verbose generic synonyms (e.g.
`Slice{T}`, `Dict{K, V}`), which would remove the overlap. Superseded
(2026-11-12): `[]T` and `{K}V` are kept as synonyms for `*List{T}` and
`*Dict{K, V}`, pointers to ordinary prelude structs, so user code rarely
needs to nest them inside `{}`. See D23.

Options explored:

- **`<>`** (`List<int>`, `struct Pair<A, B>`, `def T max<T>`). Usually
  rejected because `<` is ambiguous with less-than, but D4 fixes most of
  it. In type position there's no ambiguity. In expression position, `<`
  after `SH_TYPE` is currently always an error (types can't be compared), so
  it can open type arguments. `Color.RED < x` and `MAX < x` stay
  comparisons because their shape isn't `SH_TYPE`. The `,` ambiguity in
  `g(a < b, c > (d))` doesn't arise either, because the type-argument list
  is already open before the `,`. Costs:
  - `List<List<int>>` lexes `>>` as SHR. Fixable in the parser: where a
    `>` is expected and the token is SHR, rewrite it in place to GT with
    offset+1 instead of calling `lex_next()`. `>=` and `>>=` can't validly
    follow a type-argument list.
  - `<` isn't a lexer bracket, so newlines inside `<...>` are layout tokens
    and long argument lists can't wrap.
  - **Explicit call-site type arguments don't work.** In `max<int>(a, b)`,
    `max` is a NAME, so `<` can't be decided from one token. Deciding after
    parsing the operand (as `sizeof_operand` does) works for acceptance, but
    the `<` is reached inside the high-priority operand: in `a + f < b`, `b`
    would be parsed and emitted before the `+`, which breaks single-pass
    emit. Knowing which names are generic functions (C++) needs symbol
    information from imports, which breaks [section 9](formatter.md)'s one-file formatter.
    So `<>` means inference only, with non-inferable cases as static methods
    (`List<int>.new()`), or Rust-like `max.<int>(a, b)` (LL(1) since `.` `<`
    is otherwise unused, but ugly).
- **`[]`** (`List[int]`, `max[int](a, b)`), the earlier D18 plan. LL(1)
  everywhere: after `SH_TYPE`, `[` is currently an error, and for function
  calls the decision is made inside the brackets with the `sizeof_operand`
  first-token rule, with no precedence issue. Rejected for looks:
  `List[[]int]`, `Map[str, [4]u8]`.
- **`'`** (`List'int`, `Pair'(int, str)`, `max'u64(a, b)`) and **`!`**
  (`List!int`, `Pair!(A, B)`, as in D). Both characters are otherwise
  unused (strings are `"` only, and a lone `!` is an error). LL(1). Short for
  one argument, but several arguments need a grouping form, and then struct
  literals read badly (`Pair'(int, str)(1, "x")`). `'` also confuses most
  editors' highlighting until luv has its own syntax definition.
- Placement of function type parameters: `def T max{T}(...)` (chosen,
  matches structs) vs. `def{T} T max(...)`, which declares `T` before the
  return type uses it. Structs could likewise be `struct{A, B} Pair:`.
  Both are LL(1) (`def {` and `struct {` are otherwise errors).
- Inference only (no explicit call-site syntax at all) was considered
  acceptable and doesn't block adding explicit syntax later. `{}` makes the
  question moot since explicit arguments come for free.
- **Implicit `$T` parameters** (2026-11-12): `def $T max($T a, $T b)`, with
  the sigil marking a type parameter and the first use declaring it, and no
  `{T}` list on functions. Single-pass and LL(1) (`$T` is one token). It was
  rejected because explicit type arguments would then need an order taken
  from first appearance, which silently changes if parameters are reordered,
  and because structs would still need a list. It's also noisier. `'T` was
  considered for the same idea and is worse, because an unpaired `'`
  confuses editors and other tooling.
- **Alphabetical order for explicit type arguments** (2026-11-12): with
  single-letter parameters, `Pair{int, str}` could bind `A`, `B` in
  alphabetical order, so no lists are needed at all. Rejected as too subtle.
  `{K, V}` only works by accident of the alphabet, and something like
  `{V, K}` couldn't be written.
