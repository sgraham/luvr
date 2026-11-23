# 4. Lookahead audit

Every place in the language that looks like it needs more than one token,
and how this design resolves it:

| Construct | Naive k | Resolution | Final k |
|---|---|---|---|
| `Stuff s` vs. `Stuff(1)` | 2 | TYPENAME lexing + left-factor (D4, D5) | 1 |
| `mod.Type v` vs. `mod.func()` | 3 | postfix chain → `expdesc.k == VTYPE` (D5) | 1 |
| `[10]int a` vs. `[1,2]` | 2+ | `[` at statement start is always a type, in an expression always a literal (D5) | 1 |
| `*int p` vs. `*p = 1` | 2+ | no prefix deref (D5) | 1 |
| `def int f(` vs. `def f(` | 2 | NAME is never a type (D4) | 1 |
| `foreign [T] f(` | 2 | same | 1 |
| `int a` vs. `self` param | 1 | — | 1 |
| `f(.a=1, 2)` named arg | 1 | leading `.` can't start an expression (D9) | 1 |
| `a not in b` | 2 | `not` in binop position ⇒ `not in` (D8) | 1 |
| `a[i:j]` vs. `a[i]` | 1 | — | 1 |
| `if ... elif/else` | 1 | after DEDENT (D3) | 1 |
| `return` vs. `return x` | 1 | follow set `{NEWLINE, DEDENT, EOS}` | 1 |
| `x = 1` declares vs. assigns | — | semantic, at first assignment (D6) | 1 |
| `List{int}`, `max{int}(x)` vs. dict literal | 1 | `{` can't otherwise follow a name or type (D21) | 1 |
| `sizeof(T)` vs. `sizeof(e)` | 2+ | first-token rule, D5 left-factor for names, `[`/`{` = type (D7) | 1 |
| `[e for x in xs]` vs. `[e, f]` | unbounded | **D10**: reorder (k=1) or bracket-bounded skim + rewind | 1 or bounded rescan |
| use before definition | whole file | **D11** lazy top-level skim on first miss (free for in-order files) | 1 per pass |

If D10 picks (a), the whole language is LL(1) with *zero* uses of the
lookahead slot, and `next()` loses its branch.
