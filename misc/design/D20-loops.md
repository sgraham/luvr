# D20 Loops

Written and decided 2026-11-24. There's no `while` keyword, so `for` has to cover
conditional loops as well as iteration. Iteration itself is
[D33](D33-iteration.md). This covers the syntax that tells the forms
apart, `break` and `continue`, and how conditional loops are emitted.

## The forms

```
for:                # until a break or return
for cond:           # while cond is true
for x in xs:        # iteration (D33)
for k, v in d:
```

There's no three-part form (`for init; cond; step:`), since `;` isn't
otherwise a token, and `for i in range(...)` covers counting once
[D33](D33-iteration.md) exists. Until then, a counting loop is written by
hand. A `continue` in it skips an increment at the end of the body, the
usual problem with conditional counting loops. That's accepted for now.

There's no `else` clause on loops (see "Possible later additions").

## Telling a condition from loop variables

After `for`, the next token decides every case but one:

- `:` is the unconditional form.
- Any token other than a NAME starts a condition. Loop variables are only
  ever names ([D10](D10-comprehensions.md)'s `for_targets`).
- A NAME starts either loop variables or a condition (`for done:`,
  `for i < n:`, `for x.ok:`).

For a NAME, the parser does what [D5](D05-decl-vs-expr.md)'s statement
dispatch does with `=`. It parses the primary (the name and any postfix
chain) and then decides:

- A bare name followed by `in` or `,` is the first loop variable. The rest
  is `for_targets` as it is now.
- Anything else is the left operand of a condition. `binop_rest()`
  continues the expression from it, as `sizeof_operand()` already does.

`primary()` reports an undefined name unless it's followed by `=`, since
an assignment can declare it ([D6](D06-assignment-declares.md)). In a `for`
header, `in` and `,` are allowed after an undefined name too. Nothing is
emitted for the name before the decision, because an `ExpDesc` for a name
is only a reference to the local.

This is LL(1) apart from treating the parsed primary as one unit, which is
already how statements work.

### Consequences

- `for x in xs:` is always iteration, never a loop on the membership test
  `x in xs`. Python makes the same choice. A membership condition needs
  parentheses: `for (x in xs):`.
- `for x not in xs:` is a condition, since `not in` can't follow loop
  variables. It reads oddly next to `for x in xs:`, but it's unambiguous.
- `for x.y in xs:` and `for f() in xs:` aren't loop variables, so they
  are membership conditions. Decided (2026-11-24): this isn't checked, like
  `for x not in xs:`. It's left to a linter, and revisited if it turns out
  to be a common mistake. Reserving `in` for loops and spelling membership
  differently was considered and rejected. Python, Odin, Nim, Julia and
  Kotlin all use `in` for both, and the header rule here is the same as
  theirs. JavaScript's trouble with `in` comes from expressions in its
  three-part `for`, which luv doesn't have.
- A parameter or constant followed by `in` is an error, as it is now
  ("A constant can't be a loop variable.").

## `break` and `continue`

- They're errors outside a loop. A loop count is kept per function, so a
  nested `def` inside a loop starts at zero.
- They apply to the innermost loop. There are no labels.
- They end their block, so a statement after them in the same block is
  an error ("Expected end of block"), as it is after `return`. All three
  are `FLOW_ENDS`.

## Flow

Like Go's terminating statements, `for:` with no `break` that applies to it
never falls through. So a function whose body ends in such a loop doesn't
need a `return` after it ("Missing return at the end of a function" would
otherwise require a dead one). Every other loop might not run its body, or
might `break`, so it falls through (as `stat()` already says for `for`).

Decided. This needs each loop to record whether a `break` targeted it. Emission needs
that anyway: without a `break`, `:exit` has no predecessors, and nothing
should be emitted into it. The pattern is common in loops that return from
inside, such as a lexer or a dispatch loop. The C source has about 25
`for (;;)` loops, several of them in functions that only return from inside
the loop (`skim_to_for()`, for example).

## Emission

`for cond:`

```
    JMP :head
:head
    %c = <cond>
    BR %c, :body, :exit
:body
    ... (continue -> :head, break -> :exit)
    JMP :head
:exit
```

`for:` is the same without the test: `:head` is the body, so `continue`
jumps to the start of the body.

The condition is emitted with `cond()`, the same as `if`, so `and` and
`or` branch directly to `:body` or `:exit` as they do in `if`. A constant
condition gets no special handling. `for true:` isn't checked for: it's an
ordinary condition, so the loop falls through, and a function that ends in
one needs a `return` after it, which points towards `for:`.

Loops are the first back edges in the IL. The SSA pass needs to know which
slots each loop stores ([D30](D30-emission.md) lists that as left for the
SSA pass), so nothing here depends on it yet.

## Possible later additions

Neither is planned now. Both fit the header decision above, and neither
changes the meaning of code that's valid without it.

### Pointer loop variables

`for &item in xs:` declares `item` as a `*T` to each element in place,
and `for i, &p in xs:` gives the index and a pointer.

- Syntax: in a `for` header, a leading `&` always starts a pointer loop
  variable. That's decided by one token, and a condition that starts with
  `&` (`for &x == p:`) needs parentheses. After the first loop variable,
  `for_targets` accepts `& NAME` as well as `NAME`. Only the last variable
  can be a pointer (`for &i, x in xs:` is an error). Comprehensions share
  `for_targets`, so they'd get it too.
- Two loop variables are a key and a value, as in Go's `for i, v := range`:
  the index for lists, arrays and `str`, and the key for dicts. That's also
  [D33](D33-iteration.md)'s `for i, x in xs`, so `&` doesn't need anything
  like `enumerate` either.
- Only iterables with storage can give pointers. A range has none, so
  `for &i in range(n):` is an error.
- In the protocol ([D33](D33-iteration.md)), the value form writes `T`
  through `*T`. The pointer form would need a second method, for example
  `bool __next_ref__(self, **T out)`, or the cases baked into `for`.
- The pointers stay valid during the loop, since changing the collection's
  size while iterating over it is an error (D33). A pointer kept after the
  loop is [D23](D23-arrays-lists.md)'s `&xs[i]` hazard.

### `nobreak` on loops

Python's `else` on loops, spelled `nobreak:`: the suite runs when the loop
ends without a `break`. Python's `else` is often misread as "if the loop
didn't run", so if it's added, it's with a new keyword. The preferred
alternative is a nested function ([D12](D12-nested-functions.md)) that
returns from inside the loop, once those work, so this may never be needed.

- Syntax: `nobreak` at the loop's indentation, right after its suite, as
  `else` is for `if`. Adding a keyword later only breaks code that uses it
  as a name.
- Emission: the condition's false edge goes to the `nobreak` block, and
  `break` jumps past it to `:exit`.
- Flow: a loop with `nobreak` falls through if a `break` targets it or
  the `nobreak` suite falls through. That reuses the per-loop "a break
  targeted this" flag. So a search loop that ends in `nobreak: return ...`
  needs no `return` after it. On `for:`, it could never run, so it would
  be an error.
