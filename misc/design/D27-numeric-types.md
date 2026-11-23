# D27 Numeric types and conversions

Decided 2026-11-15. Follows Go: one default integer type, untyped
constants that take their type from context, and no implicit conversions
between numeric types.

## `int`

`int` is `i64`. It's an alias, not a distinct type, so `int` and `i64` mix
freely. Error messages say `int`, even when the source wrote `i64`.
That's occasionally a little confusing, but `int` is the name nearly all
code will use.

The alternatives considered:

- `int` as `i32`. Smaller arrays and struct fields, twice the SIMD lanes,
  and a match for C's `int`. Rejected because 2³¹ is reachable by byte
  counts, file sizes, sums and millisecond timestamps (24.8 days), and
  because it makes `len`, `sizeof` and indexes a different type from the
  default, which is where most mixed-width code comes from. Where storage
  size matters, `i32` is written explicitly.
- No `int` at all, only sized types. Rejected because `x = 5` still needs a
  default type, which would then be unnamed, and because `int` is easier to
  type and says "the width doesn't matter here". The friction in languages
  without an `int` comes from defaults that disagree (i32 literals and
  `usize` indexes), not from the name.
- Removing `i64` and keeping only `int`. Rejected because `foreign`
  declarations need a 64-bit signed name (below), and because code where
  the width is the point (struct layouts, binary formats) reads better with
  the whole `i8`..`i64`, `u8`..`u64` family.

`len` and indexes are `int`. They're signed, so `len(xs) - 1` on an empty
list is -1, not 2⁶⁴-1. `sizeof`, `alignof` and `offsetof` are untyped
constants (D28), so they can be used as any integer type. (Revised
2026-11-21: `offsetof` was `int`. As a constant, its arithmetic is exact
and can't wrap, and it's used alongside sizes and passed to C as `u64`.)

Each type has one name, apart from `int`, which is there because it's the
default and is easier to type. So there's no `uint` (`u64`), `byte` (`u8`),
or `float` and `double` (`f32` and `f64`). `f16` is removed too until
there's something to do with it. SIMD types like a 4-wide float vector or a
4x4 matrix are more likely to come first.

`codept` stays as it is for now. It's meant to be a code point type that's
distinct from the integers, like Go's `rune`, and is mostly unused until
strings are settled (D23).

There's no `void` type in luv code. A function without a return type
returns nothing, and an untyped pointer is `*u8`. The parser's `TY_VOID` is
only the internal return type of such a function.

## `foreign`

`foreign` is C's `extern "C"`, where `int` is 32 bits. `int` is an error
in `foreign` signatures, including inside pointer and other compound
types, so a C `int` is always written `i32`. Passing an `i64`
where the callee reads an `int` silently uses the low half, and the upper
half of an `int` result is garbage, so this would be an ABI mismatch with
no error.

There are no foreign-only types. A `foreign` signature uses luv types and
only has to match the C function's ABI, not its C types. `char*`,
`const char*` and `void*` are all `*u8`, and `size_t`, `long` and
`uint64_t` are `u64` or `i64` on LP64 targets. Only width matters, and for
arguments narrower than 32 bits, signedness.

Matching C's types exactly was considered, with foreign-only keywords such
as `c_char` (`const char`) and `voidptr`. It doesn't converge:

- Same-width integer typedefs are distinct C types that differ by platform.
  On macOS `uint64_t` is `unsigned long long` and `size_t` is `unsigned
  long`, and on Linux both are `unsigned long`.
- `const` appears on struct pointers (`const struct timespec*`), at other
  levels (`char *const argv[]`), and inside function pointer types (`qsort`'s
  comparator).

Exact types only matter for a C backend that also includes system headers.
A C backend doesn't need to include them. It can declare only the foreign
functions that are used, from the luv signatures, and then nothing
conflicts. clang and gcc still warn about redeclaring library functions
they know (`memcpy`, `printf`) with different types, which `-fno-builtin`
or `-Wno-incompatible-library-redeclaration` and
`-Wno-builtin-declaration-mismatch` turn off. If that's not enough, a
decorator on a `foreign` declaration could give the C declaration to emit
verbatim. Things that are macros in C (`errno`, `stdin`) need a C shim
either way.

`int` in struct fields isn't checked, so a struct passed to C with an
`int` field is the same silent mismatch. Finding these from the `foreign`
signatures means following fields and pointers transitively, with cycles,
structs declared later and generic instantiations. If it's checked, the
tentative choice is to mark C-compatible structs at their declaration
(`foreign struct S:`), require the mark on structs passed by value in
`foreign` signatures, and reject `int` fields and unmarked struct fields
in marked structs. Every check is then local to one declaration, and the
mark is also where C layout is promised. Nothing is done for now. It may
be left to a lint tool instead. D31 proposes no mark, since every struct
has C layout.

## Untyped constants

Integer and float constants without a suffix are untyped until context
gives them a type. A declaration by assignment gives `int` for an integer
constant and `f64` for a float constant. The value must fit the type it
gets (`u8 x = 300` and `u32 x = -1` are errors). A float constant is
rounded to an `f32` or `f64` target, and only overflow is an error.

- An untyped integer constant can become a float (`f64 x = 1`, `y * 2` with
  `f32 y`).
- An untyped float constant can't become an integer, even when it's
  integral (`int x = 2.0` is an error).
- A constant takes the other operand's type, and never makes the other
  operand wider. `x + 3000000000` with `i32 x` is a range error, not an
  `int`.
- With untyped float constants, `x * 0.1` with `f32 x` stays `f32`. In C,
  `0.1` is a `double`, so the whole expression is promoted.

## No implicit conversions

Between different numeric types, nothing converts without `cast`:
integers of different widths or signedness, `f32` and `f64`, and integers
and floats. That applies everywhere: assignment, arguments, `return`,
operands of arithmetic and comparisons, and `+=`.

Two places accept any integer type, because neither changes the type of a
value:

- An index. It's extended to 64 bits and bounds checked as unsigned, so a
  negative index fails the check.
- A shift count.

Lossless widening (`i32` to `int`, `u8` to `int`, `u32` to `int`, `f32` to
`f64`) was considered, either only where a target type is known or for
binary operands too, as in Zig's peer type resolution. With `int` as `i64`,
`len` and indexes as `int`, and adapting constants, little code mixes
widths, and what's left is mostly narrow storage being read into `int`
arithmetic. Not done, for these reasons:

- It hides where arithmetic happens. `int r = a * b` with `i32` operands
  multiplies in `i32` and wraps before widening. That's worse with narrow
  types: `int d = c - 48` with `u8 c` wraps at 256 when `c < 48`. Byte
  processing code, where widening would save the most casts, is where this
  happens most.
- Changing a variable's type to a narrower one is flagged at every use
  that mixes, instead of moving where its arithmetic wraps.
- int to float in particular is never added: `f64 avg = sum / n` with
  integer operands would be integer division followed by a conversion.
- Adding implicit widening later doesn't break any code, and removing it
  would. It's revisited if real code shows the casts are too painful.

The rule doesn't stop `cast int(a * b)` either, which has the same wrap as
the widened version. The error for a narrower value doesn't suggest
casting the operands instead. Errors say what's wrong, not how to fix it.

Comparisons need the same type, so signed and unsigned don't compare
(`compare_signed_unsigned.luv`). Comparing by mathematical value across all
integer types, as Swift does, would be correct and costs one extra
compare for a signed/unsigned pair, but isn't needed while `len` is signed.
Integers and floats never compare.

The IL already has the operations for `cast`: `SEXT32`, `ZEXT32` and
`TRUNC` between integer widths, `ITOF`, `UTOF`, `FTOI` and `FTOU`, and
`FEXT` and `FTRUNC` between float widths.

## Casts

`cast T(x)` converts between these:

| From | To | Result |
|---|---|---|
| `T` | `T` | No-op, so generic code doesn't need a special case. |
| Any integer type | Any integer type | Truncated, or extended by the signedness of the source. |
| Integer or float | Integer or float | `ITOF`, `UTOF`, `FTOI`, `FTOU`, `FEXT`, `FTRUNC`. |
| Untyped constant | Any numeric type | The constant, which must be in range for `T`. |
| `*T` | `*U` | Same address. |
| `*T` | `i64` or `u64` | The address, and the reverse. |

Everything else is an error, including:

- Pointers to or from integers narrower than 64 bits, which would silently
  truncate an address. A cast through `u64` is explicit about it.
- `bool` to or from numbers. Comparisons give a `bool`, and `x != 0` goes
  the other way.
- Structs, arrays, lists and dicts, since reinterpreting a layout isn't
  what `cast` is for.
- `str` to or from anything, for now. `*u8` from a `str` for foreign calls
  depends on how strings are represented (D23).

A float that's out of range for the integer type, or NaN, gives an
unspecified value, but isn't undefined behaviour. Saturating, as in Rust,
gives the same result on every target, but costs extra instructions on
x86 for conversions that are wrong either way.

A constant out of range for `T` is an error, as in Go, so `cast u8(300)`
and `cast u64(-1)` are both rejected. Untyped constants are conceptually
arbitrarily large, so wrapping one to fit is a category error rather than
a conversion. `~cast u64(0)` or a hex literal gives all ones.

## Steps

1. `int` as an alias of `i64`, untyped integer constants defaulting to
   `int`, and `describe_type()` printing `int`.
2. `uint`, `byte`, `float`, `double` and `f16` removed from the lexer, the
   tests and the vim syntax file.
3. `int` rejected in `foreign` signatures.
4. Indexes and `len` typed as `int`, any integer type accepted as an index.
5. Floats: `f32` and `f64` types, untyped float constants, and the
   conversion errors above.

Steps 1, 2, 3 and 5 are done, and step 4 only for `sizeof`, `alignof` and
`offsetof`. Remaining:

- `len` typed as `int`. It's one of the builtin names in `lex.c`, and calls
  aren't typed yet. It should get `int` from its declaration once builtins
  are in a prelude and calls have signatures.
- Indexes. Pointers and fixed-size arrays are typed and their indexes are
  checked (2026-11-21, D31 step 1). Lists and dicts are still `ty_unknown`,
  and a dict key isn't an integer, so those indexes can't be checked until
  their container types exist.
- Range checks on constants: `u8 x = 300`, `u32 x = -1`, and overflow of
  an `f32` or `f64` target. These need the constant's value in `ExpDesc`,
  and unary `-` folded, since it currently gives an unknown type.
- Casts are only checked between the types the checker has. Pointers are
  still `ty_unknown`, so their rows in the cast table aren't enforced, and
  constant range checks wait for constant folding.
