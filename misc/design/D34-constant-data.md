# D34 Constant data (decided)

Written 2026-11-24. Strings, struct literals and array literals can't be
used anywhere a constant is required: global initializers, field defaults
and named constants. The trigger was `str entry = "..."` as a field default
in betterbench, which the generator avoids. This collects what each case
needs, and proposes one mechanism for all of them instead of a special case
for strings.

## Where things are

Probed 2026-11-24, in the checking build (`luv parse`) and the emitting
build (`luv il`):

| Form | Checking | Emitting |
|---|---|---|
| local `str s = "hi"` | ok | ok |
| global `greeting = "hi"` | "Expected a constant expression." | same |
| `GREETING = "hi"` | "Expected a constant expression." | same |
| field default `str s = "hi"` | "Expected a constant expression." | same |
| field default `In i = In(.x=4)`, global `g = In(.x=4)` | "Expected a constant expression." | same |
| local `[3]int a = [1, 2, 3]` | ok, but not type checked | "This expression isn't supported yet." |
| global or field default `[3]int a = [1, 2, 3]` | "Expected a constant expression." | same |
| local `[]int a = [1, 2, 3]` | ok, but not type checked | "an unknown type isn't supported yet." |
| global `count = 1` | ok | globals aren't emitted at all |

Four separate gaps show up:

1. **Only scalars are constants.** A constant is an `ExpDesc` with
   `EK_CONST` and a `ConstValue` ([D28](D28-constants.md)). A string
   literal is `EK_STR` in the emitting build, and unemitted in the checking
   build. A struct literal is built by stores into a temporary as its
   arguments are parsed ([D31](D31-aggregates.md)).
2. **Data images can't contain pointers.** A struct's defaults are a byte
   image in `Ctx.data`, copied when one is created. A `str` is
   `{ptr, len}`, and the pointer to the bytes would need a relocation
   (the comment in `emit_agg_addr()` says so), which `IlData` can't
   express.
3. **List and array literals aren't typed.** `list_literal()` parses the
   elements, and the result has an unknown type, so `[3]u8 a = [1, 2, 300]`
   isn't checked, and nothing knows whether a literal is an array or a list.
4. **Global variables aren't emitted.** There are no global symbols in the
   data, and no loads or stores of them.

## Proposal: constant data is bytes plus relocations

A constant of aggregate type, or a `str`, is an `IlData` holding its
bytes, plus a list of relocations, each saying "the 8 bytes at offset N are
the address of data M". Every case above fits this:

- `"hi"` as a value is a 16-byte image: a relocation to the 2 bytes, and
  the length 2.
- A field default image is the current struct image with relocations added.
  The relocations are applied once, when the data is placed in memory before
  `main` (by the JIT when it lays out its sections, or by the linker or
  loader for an object file). So the image in memory holds the real pointer,
  and creating a struct is still one `COPY` with nothing to fix up.
- A struct or array literal of constants is an image. In a function, using
  it is a `COPY` from the image, as a default image is used now.
- A global's initial value is its image. D28 already requires global
  initializers to be constant, so that no code runs before `main`, and it
  lists "C-style constant literals (strings, and struct, list and dict
  literals of constants)" as planned.
- A named constant of one of these types (`GREETING = "hi"`,
  `ORIGIN = Point(0, 0)`) refers to an image, and each use is a `COPY`.
- A list literal of constants copies its elements from an image into arena
  memory (see Lists below).

### In the checker

`ExpDesc` gets a kind for constant data, `EK_CONST_DATA` (name decided
2026-11-24). Both it and
`EK_CONST` are compile-time constants. They differ in where the value is,
which is what `ExpKind` describes: an `EK_CONST` value is in the `ExpDesc`
itself (`ConstValue`), so it can be folded and emitted as an immediate. An
`EK_CONST_DATA` value is in memory, the compiler only has its address, and
nothing folds it. A helper such as `exp_is_const()` covers both for the
"Expected a constant expression." checks.

Like `EK_CONST`, it's set in both builds, so that the checking build knows
that a string, struct or array literal of constants is a constant, and
accepts it as a field default or global initializer. The `DataId` is only
filled in when emitting, so the checking build doesn't copy any bytes.

How a literal knows it's building constant data was a choice between:

- **(a) Constant contexts.** A global initializer, a field default and an
  ALL_CAPS initializer are all known before their expression is parsed. In
  those, a parser flag makes struct and array literals build an image
  instead of emitting stores, and a non-constant element is "Expected a
  constant expression." at the element. In a function body, literals emit
  stores as they do now. A literal of constants in a function could be
  turned into a `COPY` from an image later, as an optimization.
- **(b) Always both.** Every literal builds an image while it's being
  parsed, and stores too, and keeps the image only if every element turned
  out to be constant. This costs on every literal, and in a constant
  context there's no function to emit the stores into.

**Decided (2026-11-24): (a).** The surface syntax is the same, but building
a constant and running code are mostly unrelated, and the context is always
known before the expression. Both options needed a stack of image buffers, because literals
nest (`Outer(In(1), "x")`), and `Parser.image` is currently a single buffer
for the struct body that's being parsed.

**Decided (2026-11-24): `EK_STR` is removed.** A `str` literal is
`EK_CONST_DATA` like other constant data, and a use is a `COPY` of its
16-byte image instead of two stores into a temporary. They should cost the
same once [D31](D31-aggregates.md)'s slot splitting exists. `print "x"`
currently passes the characters' data and length directly, without building
a `str`. It reads both from the image instead (the target of the relocation at
offset 0, and the 8 bytes at offset 8), so its output doesn't change. Folding `len("abc")`
could work the same way.

### In the IL

- `IlData` gets relocations: for each one, the offset in this data and the
  `DataId` of the target. An addend isn't needed yet, since nothing points
  into the middle of another data item.
- **Decided (2026-11-24):** data is dumped as a sequence of bytes, as it
  is now, with a second way of writing it that can include relocations:
  `data @.dat1 align 8 @.dat0 05 00 00 00 00 00 00 00` for a `str`. A
  token starting with `@` is an 8-byte relocation to that data, and any
  other token is one byte in two lowercase hex digits. Data without
  relocations whose bytes are all printable (or `\n`, `\t`) stays a quoted
  string, `data @.dat0 "hello"`, and anything else uses the hex form. There
  are no sizes, padding or fills, since a back end only needs bytes and
  relocations. Nothing is inserted before a relocation, so in
  `data @.dat6 01 02 03 @.dat5` the relocation is at offset 3, unaligned,
  which is probably an error. The bytes are little-endian, so the image
  builder is slightly target specific. The alternatives were sized items with assembler names
  (`dq @.dat0 dq 5`) and QBE's typed items (`{ l @.dat0, l 5 }`), which
  are easier to read field by field, but need the image builder to record
  each field's size for the dumper.
- The dumper only needs the bytes and the relocation list, which the back
  end needs anyway. It prints `@.datN` at each relocation's offset and
  skips its 8 bytes.
- `interp.py` parses the tokens, and writes each relocation's address once
  all data has been placed, since a relocation can refer to later data.
- Read-only and writable data: images and string bytes never change, but a
  global's storage does. **Decided (2026-11-24): a flag on `IlData`** that
  chooses `.rodata` or `.data`. A global's initial image is the global's
  storage itself, as in C.

[D25](D25-backend.md)'s backend already plans code and data in sections
with relocation records, so these relocations go straight through.

## Typing list and array literals

The parser is single pass, so a literal can't be built first and converted
when its type becomes known. Every place that gives a literal its type
knows that type before the literal is parsed:

- A declaration: `[3]u8 a = [...]`.
- A field default, which comes after the field's type.
- A call argument, since the parameter types are known ([D9](D09-calls.md)).
- An assignment to an existing variable, field or element.
- A `return`, which has the function's return type.

So the proposal is an expected type passed down into expression parsing. It
only matters when a literal is the whole expression, which is decided by
the first token, so it doesn't need threading through `subexpr()` and
`primary()`. An entry point `expr_expect(p, t, e)` calls
`list_literal(p, t, e)` directly when the first token is `[` (or `{` for a
dict), and continues with `postfix()` and `binop_rest()`. Otherwise it's
`subexpr()`. Its callers are the places listed above, and a literal's own
elements, which handles nesting (`[2][2]int`). Other expressions pay one
token compare. A literal in parentheses or as an operand (`-[...]`) gets no
expected type, so it's a list and a type mismatch, which is acceptable for
now. `list_literal()` reads the type: `[N]T` makes an array of exactly N elements, each checked
against `T` (untyped constants adapt as they do in `check_assign`).
`[]T` makes a list. With no expected type, `a = [1, 2, 3]` is a `[]int`
list ([D23](D23-arrays-lists.md)), with element types from the first
element. Dict literals would work the same way.

This is needed for every literal, not only constant ones, so it's probably
the first piece.

An array literal needs exactly N elements for now. A trailing `...` that
fills the rest with zeros (as C does for a short initializer), or repeats
something, might be added later.

## Arrays

- In a function: an array literal fills the destination's storage, with
  one store per element (or a `COPY` from an image, once that optimization
  exists).
- Fixed arrays are values ([D23](D23-arrays-lists.md)), so an array global
  or field default is just part of the image.

## Globals

Globals need emission before any of the global cases work, even
`count = 1`:

- A symbol per global, in data: zero-filled if there's no initializer,
  otherwise its image.
- Loads and stores through the global's address, a `KSYM` like a
  function's (D26 already describes `IT_SYM` as "a function, later a
  global").
- Scalars first, then aggregates through images.

## Lists

`[]T` is `*List{T}`, and `List{T}` is a generic prelude struct
([D32](D32-prelude.md)), so list literals wait for generic instantiation.
Once that exists:

- A list literal allocates a `List{T}` and its elements in the current
  arena. A literal of constants copies its elements from an image.
- A global list could be a static `List{T}` header and static elements in
  writable data, with growth moving the elements into the arena, as for any
  list. **Decided (2026-11-24): global lists are errors for now.**

## Order

Each is a small piece, reviewed separately.

1. Typing list and array literals from the expected type, in the checking
   build. No emission.
2. Array literals in functions: one store per element.
3. `EK_CONST_DATA` in the checking build, so string, struct and array literals of
   constants are accepted as field defaults, global initializers and
   ALL_CAPS initializers. No emission.
4. Relocations in `IlData`, the dump format, and
   `interp.py`.
5. `str` field defaults and struct literal field defaults, through images
   with relocations.
6. Named constants of `str`, struct and array type.
7. Global variables: scalars, then aggregates.
8. Lists, after generic instantiation.

Piece 6 is independent of the others for `str` alone, since a use could
build the value as a literal does now. It's later here so that it uses
images like the rest.

## Decided in review (2026-11-24)

- Literals build constant data only in constant contexts, (a) above.
- `EK_CONST` and `EK_CONST_DATA`. `EK_STR` is removed.
- Dump format: hex bytes and `@.datN` relocations, or a quoted string when
  there are no relocations and the bytes are printable.
- A flag on `IlData` for read-only or writable data.
- Identical images aren't shared. A literal written twice gets two images.
  Sharing can be added later if it matters.
- Global lists are errors for now.
- The expected type is a parameter of `expr_expect()`, only used when a
  literal starts the expression.
