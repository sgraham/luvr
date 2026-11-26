# D35 Interpolation formatting (open)

Written 2026-11-26 to collect a discussion. Nothing here is decided, and
none of it should be built until strings can be built in an arena
([D32](D32-prelude.md), [D23](D23-arrays-lists.md)). Before then, only the
`print` path could use it.

[D13](D13-literals.md) decided `\(expr)` interpolation and deferred format
specs, noting that a spec after a depth-0 `:` could be lexed without
conflict. This note covers three extensions: a format spec, a debug view
with `=`, and how floats are formatted generally.

## Syntax

```
"\(x)"              # as now: the default format
"\(x, 08x)"         # x formatted with the spec 08x
"\(x=)"             # x=4: a label, then the debug view
"\(a + b = )"       # a + b = 7: the label's spacing is copied from the source
"\(x=, 08x)"        # x=000000ff: the label, then the spec instead of the debug view
```

The combined form follows Python's `f"{x=:08x}"`: with a spec, the value
after the label is formatted with the spec, not the debug view.

### Parsing `=`

A single `=` can't occur inside an expression. Assignment isn't an
expression ([D8](D08-expressions.md)), `==` is a separate token, and
designators (`.a=`, [D9](D09-calls.md)) are only inside call parentheses.
So `subexpr` stops at the `=`, and the interpolation accepts an optional
`TK_ASSIGN` before `STRING_MID`/`STRING_END` or the spec. The
name-then-`=` rule that declares a local ([D6](D06-assignment-declares.md))
is only checked at the start of a statement, so it doesn't apply.

The label is the source bytes from the expression's first token up to and
including the `=` and any spaces after it, as in Python, where `{x=}` gives
`x=4` and `{x = }` gives `x = 4` (decided 2026-11-26). A formatter that
changed spacing inside `\( )` would change the program's output, so it has
to leave the inside of a `\(...=)` alone.

### Parsing the spec

A comma at the interpolation's own bracket depth can't be part of the
expression. There are no tuples, and every other comma in an expression is
inside a call, a literal, or a comprehension, all of which are bracketed.
So the grammar is unambiguous:

```
interp -> expr ['='] [',' SPEC]
```

The spec itself can't be lexed as ordinary tokens. `08x` is a malformed
number, `.2f` lexes as the float `.2` followed by `f`, and `>10` is an
operator. Options:

| | Spec read by the string reader | Spec as a string literal |
|---|---|---|
| Looks like | `\(x, 08x)` | `\(x, "08x")` |
| Lexer | On `,` at `interp_stack[interp_sp-1] + 1`, the string reader resumes in a spec mode and reads raw bytes up to the closing `)`, returning `TK_STRING_SPEC`. The `)` then resumes the string as usual | No change |
| Cost | One more compare per `,` while `interp_sp` is non-zero, the same test that `)` already does. Nothing when not inside an interpolation | None |
| Limits | A spec can't contain `)` | None, but nested quotes inside a string are hard to read |

Spaces at either end of the spec are trimmed, so `\(x,08d)` and
`\(x, 08d )` are the same. That can't lose anything in Python's grammar: a
space is only meaningful as a fill character, which comes before an align
character, and space is already the default fill.

The first option isn't really a new lexer mode. It's the existing string
reader with a flag, entered at the same point as `)` re-enters it. The
concern raised is whether it's confusing to have a few bytes after a comma
that aren't tokens. Leaning: the first option, and measure the `,` compare
on betterbench (strings with interpolations are rare there).

`:` would parse as well as `,` would, since a depth-0 `:` can't occur in an
expression either (slices and dict literals are bracketed). `,` was
preferred because `:` makes Python-style specs busy (`{x:>10:...}`).

Python's nested replacement fields in specs (`{x:{width}}`) aren't
supported. A `\(` inside a spec could be added later.

## `__format__`

The text after the comma is passed to a `__format__` method, as in Python,
so luv only defines spec rules for the builtin types.

Unlike Python, the spec is always a literal in the source, so:

- For builtin types, the compiler parses and checks the spec, and emits a
  call with the width, precision, fill and flags already decoded. The
  runtime never parses a spec, and `\(n, 08q)` on an `int` is a compile
  error.
- For user types, `__format__` receives the spec as a `str`. It's only
  checked when it runs.

The signature should write to a sink rather than return a `str`, so that
`print` still allocates nothing:

```
def __format__(self, *Writer w, str spec)
```

`Writer` would be a prelude struct. The `print` path passes one that writes
to stdout. Interpolation outside `print` passes one that appends to an
arena string. The same writer would be used by `__str__`, which would then
have the same signature without the spec.

The builtin spec grammar would be a subset of Python's
`[[fill]align][sign][#][0][width][.precision][type]`, with types `d x X o b`
for integers, `e f g` for floats, and `s` for `str`. Nothing has been
decided about which subset.

## The debug view

`\(x=)` shows a debug view of the value. For scalars that's the obvious
form (`x = 4`, `s = "a\n"` with quotes and escapes, `f = 0.1`). For
structs, the compiler can generate it field by field, recursively, without
any user code:

```
p = Point{x=1, y=2}
```

A user method overrides the generated one. Pointers are shown as addresses
and not followed, which avoids cycles. Lists and dicts show their elements,
with a limit to be decided.

An embedder that hosts the JIT might want the debug view as structure
rather than text, for example to show it as a tree. So the debug view
should be produced as a sequence of calls on a sink, not as a string:

```
begin_struct(name) field(name) int(v) float(v) str(v) pointer(v) ... end_struct()
begin_list(len) ... end_list()
```

This works like Rust's `Debug` with `Formatter::debug_struct`, or a serde
`Serializer`. The text for `\(x=)` is produced by one built-in sink, and an
embedder supplies another, for example as a struct of function pointers
with the C ABI. Nothing is allocated in between, and the generated code for
a struct is the same whichever sink it's given.

Open: the name of the overriding method (`__debug__`?), and whether `print`
of a struct with no `__str__` uses the debug view by default.

## Float formatting

Decided 2026-11-26. `print`, `\(f)`, error messages and IL dumps all format
an f64 as Python's `repr()` does: the shortest digits that read back as the
same value, in fixed notation for decimal exponents from -4 to 15 and with an
exponent otherwise. So `0.1`, `300.0`, `0.0001`, `10000000000.0`, `1e-05`,
`1e+16`, `5e-324`, `inf`, `-inf`, `nan` and `-0.0`. An integral value always
has a `.0`, so it can't be mistaken for an integer in output, in errors, or
in the IL, which needs it to tell constants apart.

Python has no f32. An f32 gets the shortest digits that read back as the
same f32 (`0.1`, not the f64 `0.10000000149011612`), laid out the same way,
except that the exponent is used from `1e7` rather than `1e16`. An f32 only
has about 7 significant digits, so a fixed `123456789.0` would print as
`123456790.0` (the f32 is 123456792), padded with zeros that look like
digits that aren't there. f32 `16777216` is the unlucky case that's exact
but prints as `1.6777216e+07`.

The test interpreter uses `repr()` for f64. `float_as_str_precise_slow` in
ctx.c finds the shortest digits by trying each precision with `%.*e` and
reading back with `strtod`, plus the next digits up for the lopsided
interval of a power of two.

The C runtime will use zmij (MIT or Boost, `zmij.c` and `zmij-c.h`, about
3,000 lines and 17 KB of tables), a fast shortest-digits library, and the
compiler will share it in place of the slow helper. Its output is the same
format except that it leaves off the `.0` and writes NaN with its sign bit as
`-nan`, so those two are fixed up after it. The helper agreed with zmij and
those two fix-ups on 7.2 million f64 and f32 values, including every power
of two and its neighbours (checked 2026-11-26, not committed). zmij also has
correctly rounded fixed precision (`zmij_write_fixed`, which matched macOS
`printf("%.*f")` on a million values), `%e` and `%g`, which covers the float
specs above without libc.

Libraries considered (checked 2026-11-26): zmij, Schubfach and Ryu gave the
same digits on 2 million values. Only their layout differs. Schubfach is
C++ only and f64 only, and always uses an exponent (`3.e+02`). Ryu is C,
but its public functions always use an exponent (`3E2`). Teju Jagua returns
only digits and an exponent, which would suit doing the layout ourselves,
but it only handles positive finite values and needs a more involved build.
Dragonbox is C++.
