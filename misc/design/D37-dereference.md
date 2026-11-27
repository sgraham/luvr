# D37 Dereference (prototyped)

Written 2026-11-27 to collect options. It came up in
[D33](D33-iteration.md)'s `bool __next__(self, *T out)`, where every write
to an out parameter is spelled `out[0] = v`:

    on WordsIter def bool __next__(self, *int index, *str word):
        ...
        index[0] = self._i
        word[0] = "zero"

`p[0]` works because pointers can be indexed, but it reads as if `p` were
an array, and `p[1]` is accepted just as well.

## Where it's needed

`.` auto-dereferences ([D22](D22-methods.md)), through any number of
pointers, so fields and methods never need an explicit dereference. What's
left is reading or writing the whole pointee:

- Out parameters of scalars and strs, as in `__next__`. This is the case
  that prompted the note, and the most common one.
- Assigning a whole struct through a pointer, `p[0] = Point(1, 2)`.
- Locals holding a pointer to a scalar, `*int p = &nums[2]` then
  `p[0] = 333` (`address_of_into_array.luv`). Mostly seen in tests and
  foreign interop.
- [D20](D20-loops.md)'s possible `for &item in xs:` would give pointer
  loop variables, which need a dereference to replace the element.

## Leaning: a general dereference, preferably prefix `*p`

Something specific to out parameters (reference or `out` parameters, below)
is more machinery than a general form, and doesn't cover the other cases. So
the leaning is a general dereference, and the preferred spelling is C's
`*p`, which matches `*T` in declarations. The prototype is the next step,
to see whether it stays simple.

### Prefix `*` at statement start

[D5](D05-decl-vs-expr.md) and [D18](D18-future-syntax.md) ruled out prefix
`*` because a statement that starts with `*` is a pointer type (`*int p`),
and deciding at the `*` needs unbounded lookahead (`**p` vs. `**int`).

The decision doesn't have to be made at the `*`. D5 already left-factors
statements that start with a name, deciding `mod.Type v` vs. `mod.x = 1`
after the postfix chain. The same works after a run of `*`s:

1. Count the leading `*`s.
2. Parse the operand:
   - A builtin type keyword, `[` or `{` starts a type, so it's a
     declaration (`**int p`, `*[]str q`).
   - Otherwise, parse a primary and its postfix chain as now. If the result
     is a type (`Stuff`, `mod.Type`), it's a declaration of a pointer to
     it. If it's a value (`p`, `mod.x`, `s.ptr`, `Stuff.make()`), it's that
     many dereferences of it, which then has to be followed by `=` or an
     augmented assignment.

There's no lookahead past the `*`s, only a decision after the operand.

Elsewhere:

- In expressions, `*` where an operand is expected is a dereference, and
  `*` after an operand is multiplication, as `-` is now.
- `sizeof_operand()` uses the same left factoring (`star_operand()`), so
  `sizeof(*int)` and `sizeof(*Stuff)` are the size of a pointer, and
  `sizeof(*p)` is the size of what `p` points to, as in C's
  `n * sizeof(*p)`. Before, a leading `*` always started a type, and
  `sizeof(*p)` was a confusing "Expected '.'" error.
- C's precedence trap (`*p.x` is `*(p.x)`) mostly goes away, because `.`
  already looks through pointers. `*p.x` is only written when the field is
  itself a pointer, and then dereferencing the field is the natural reading.
- D5's rule changes from "a statement starting with `*` is a type" to
  "decided after the operand".

Prototyped 2026-11-27. `star_operand()` counts the `*`s and parses the
operand, and is shared by statements and `sizeof`. A statement starting
with `*` enters `decl_or_expr_stat()`'s name branch: a type goes to
`decl_rest()`, and a value is the target of the same assignment code as
`a.b = v`. Measured on the desktop against the previous parser, this is
within noise or slightly faster. Two other layouts were slower: moving the
assignment code out of the name branch so that `*` could share it (about 2%
on dumbbench, with the same instruction count), and a separate `NOINLINE`
path for `*` statements with its own assignment code (0 to 1.5%, varying
between builds). Errors:

- `*n = 1`: "'*' expects a pointer, not int."
- `*NOWHERE = 1` for a pointer constant: "Can't dereference a null constant.", since
  a pointer constant can only be null.
- `*p` alone: "Expression can't be used as a statement."
- `*Point` with no name: the usual "Expected variable name".
- `*ptr = 1` at top level: "Only a name can be assigned to at top level.",
  which also covers `b.x = 1`, previously accepted.

### Fallback: `p.deref`

If prefix `*` has a snag, `deref` as a keyword after `.` is LL(1): the
lexer gives a keyword token, so it can't clash with a field, and fields
can't be named `deref`. It reads plainly, in the way Python does, but it's
long for out parameters (`index.deref = self._i`). Unlike `.` on fields,
it would remove exactly one level of pointer.

## Rejected

| Syntax | Used by | Why not |
|---|---|---|
| `p^` | Pascal, Modula-2, Odin | `^` is xor, and `p^=5` lexes as `p ^= 5`. |
| prefix `^p` | (none) | LL(1), since `^` never starts a type, but it doesn't match declaring with `*`. |
| `p.*` | Zig | Postfix and LL(1), but busy, and `p.*=5` lexes as `.` `*=`. |
| `p[]` | Nim, Mojo | Postfix and LL(1), but `[]` already means lists, so it's confusing with pointers to lists and inside list literals (`[xs[][0]]`). |
| `p.all`, `p.pointee` | Ada, Swift | A reserved field name, the same as `.deref` but less clear or longer. |
| `<<p` | Jai | Prefix with a symbol that's a shift. |

## Out parameters, possibly later

These let the callee write `index = v` with no dereference. They're not the
plan, but one could be added later without conflicting with a general
dereference.

- **Reference parameters, `&T`**: passed as a pointer, and the name is an
  lvalue of `T` in the callee. The caller passes `&x`, as with Swift's and
  Hylo's `inout`. `&` doesn't start a type, so it's LL(1) with no keyword,
  and since parameters are immutable, `index = v` can only mean writing
  through it.
- **An `out` (or `inout`, `ref`, `mut`) mode**, like C#, Ada and Mojo. That
  `out` is currently used as a variable name doesn't matter at this stage,
  since keywords can still change.
- **Assignment through a pointer parameter** with no syntax at all. Too
  implicit, since `=` on a `*T` local would still rebind it.
- **Multiple returns** ([D5b](D05-decl-vs-expr.md)), which remove the need
  for out parameters. D33 chose out parameters instead.

The case for an `out` mode is optimization rather than syntax. A `&T` is a
pointer in the ABI, the same as `*T`, so it gains nothing by itself. A
write-only `out` that can't be stored or have its address taken could:

- be returned in registers alongside the result on calls between luv
  functions, whose calling convention the compiler owns, which is
  multiple returns under another name, and saves a store and a load per
  `__next__` step.
- let the caller's hidden slot be promoted to a register without analysing
  the callee, since it can't escape.

Until there's a backend this is speculative. D33's baked cases and
inlining cover the hot loops.

## Unrelated: `~` and `^`

Raised in the same discussion, not needed for any option above. Go has no
`~`: unary `^x` is bitwise complement and binary `a ^ b` is xor, which is
consistent because complement is xor with all ones. Doing the same would
free `~` for something else later. Python and C use `~`, so it's a small
departure from both. `&` and `|` wouldn't change.

## Open questions

- Whether to make unary `^` the complement, freeing `~`.
