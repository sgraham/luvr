# D28 Constants and folding

Decided 2026-11-15. Constant expressions are evaluated in `ExpDesc` while
they're parsed, before anything is emitted, so that range errors are found
and so that `[BLAH]int` has a size. The rules follow Go, apart from the
limits on precision.

## Representation

`ExpDesc` records whether the expression is a constant, and if so its value:
an integer, a `double` or a `bool`. Literal values are already in the token,
so a literal only costs a copy.

Untyped integer constants are `__int128`, behind a `ConstInt` typedef, with
all arithmetic in helpers. Go uses arbitrary precision. 128 bits covers
every value of every integer type, and intermediates such as
`(1 << 64) - 1` that go past 64 bits and come back. A result past 128 bits
is an overflow error at the operator, like a typed one. MSVC has no `__int128`,
so the helpers will need a version over two `u64`s there. Callers don't
change.

Untyped float constants are a `double`, not arbitrary precision.

Typed constants fold at their own width, and overflow is an error at the
operator: `0x4000_0000_0000_0000i64 * 2`. The overflow checks use the
`__builtin_*_overflow` functions in helpers, which will be `#ifdef`ed to
MSVC's equivalents (such as `_mul_overflow_u64`).

## Untyped constants

An untyped constant has no width, so its arithmetic can't overflow a type.
It's checked when it's converted to one, for an assignment, a declaration,
a `return`, an argument, a cast, or an operand of a typed value, and the
error is at the start of the value:

    x = 0x7fff_ffff_ffff_ffff + 1     # 9223372036854775808 doesn't fit in int.
    u64 HIGH = 1 << 63                # fine

Checking at the operator would mean picking a width, probably `int`, and
then `1 << 63` would be an error even when the target is `u64`.

A float constant is rounded to `f32` or `f64`, and is an error only if it
rounds to infinity, as in Go. An integer constant converted to a float type
has to be exactly representable, so `f32 x = 16777217` is an error. Go
rounds it. Decimal fractions like `0.1` have to be rounded, but an integer
that changes is probably a mistake.

`~` on an untyped constant is `-x - 1`, which is the bitwise complement of a
two's complement number with unlimited width. So `~0` is `-1`, and
`u8 MASK = ~0x0f` is a range error. `0xf0` or `~cast u8(0x0f)` is what's
meant. On a typed constant, `~` flips the bits of its width.

## Errors

- Overflow and divide by zero are reported at the operator, like the type
  errors for operators.
- A constant zero divisor is an error even when the dividend isn't a
  constant (`x / 0`), for floats as well as integers.
- The minimum signed value divided by `-1` overflows.
- Division truncates toward zero, and `%` has the sign of the dividend.
- A negative shift count, or one at least the width of a typed left
  operand, is an error. `<<` that loses bits of a typed constant overflows.
- Negating a typed unsigned constant other than 0 is an error, and so is
  converting a negative constant to an unsigned type.
- A cast of a constant gives a typed constant, and its value has to be in
  range of the target, including for typed constants (`cast i8(200u8)`). A
  float constant cast to an integer type has to be integral, as in Go. An
  integer constant cast to a float type is rounded, unlike an implicit
  conversion, which has to be exact.

## Named constants

An ALL_CAPS name is a constant, and its initializer must be a constant
expression. The constant has the type of its initializer, so `BLAH = 3`
is untyped, as in Go's `const BLAH = 3`, and `u8 x = BLAH` works.

That makes `xx = 3` and `XX = 3` different. `xx` is a variable, which has
storage and so needs a type, and gets `int`. `XX` is a value without
storage, which gets its type where it's used. Case already carries meaning
(D4), and with no implicit numeric conversions (D27), untyped constants are
what keeps casts from spreading. `BUF_SIZE = 4096` can be compared with a
`u32` length and used as an `i32` bound.

A constant is typed if its initializer is: `XX = 3i16` or `NEG = -1i64`.
`sizeof` and `alignof` of a type whose layout is known are untyped constants
(changed 2026-11-16, they were `int`), so `sizeof(int) * 100` can be passed
as C's `u64` size without a cast, and `SIZE = sizeof(T)` is untyped. `i16 XX = 3` is the same as `XX = 3i16`. A typed constant
doesn't adapt, so `x = MASK` with `MASK = 0x0fu8` makes `x` a `u8`.

Values of named constants are in a side array, indexed from the `Local` or
global, so variables don't pay for them.

## Global variables

Decided 2026-11-16. A global variable's initializer must also be a constant
expression, as in C, so that no code runs before `main` and globals don't
need an initialization order. `myvar = somefunc()` at the top level is an
error. C-style constant literals (strings, and struct, list and dict
literals of constants) are planned, but are errors for now, so that emission
and the backends don't have to handle initialized aggregate data yet.
Evaluating calls at compile time is a much later idea.

## Steps

Each step is driven by the todo tests named.

1. The constant value in `ExpDesc`, set by literals, `true` and `false`.
   Unary `-` and `~` fold untyped and typed constants. On other values they
   check that the operand is a number or an integer, and give its type
   instead of `ty_unknown`. `neg_const_not.luv`. Benched, since it's on the
   path of every literal.
2. Range checks when an untyped constant is converted, everywhere
   `check_assign` is used, and for `f32`. `const_out_of_range.luv`,
   `const_negative_unsigned.luv`, `return_too_big.luv`.
3. Folding `+`, `-` and `*`. The `*_add_overflow`, `*_sub_overflow` and
   `*_mul_overflow` tests, with their messages reworded where the error is
   now a range error.
4. `/` and `%`. The `*_div_zero` and `*_mod_zero` tests.
5. Shifts, and `|`, `&` and `^`, which can't overflow. The
   `*_shl_overflow` tests.
6. Casts of constants, which are range checked (D27), and comparisons,
   `not`, `and` and `or` on constants.
7. Named constants: a constant expression required for an ALL_CAPS
   initializer, the value stored and produced by uses of the name,
   `sizeof` and `alignof` of builtin types as constants, and the size of
   `[BLAH]int`. `const_invalid_function_call.luv` and a rewritten
   `const_no_cast_eval_yet.luv`. Forward references to constants wait for
   the D11 skim.

Strings as constants, and `sizeof` of structs, which needs their layout,
come later.
