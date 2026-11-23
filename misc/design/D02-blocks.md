# D2 ★ How blocks are delimited

- **(a) Significant indentation, fixed 4-space unit.** This is what luv60 does
  (`categorizer.in.c` matches `\n` followed by 4·k spaces). The indent level
  is `spaces/4`. Anything that isn't a multiple of 4, and any tab, is an
  error. No indent stack is needed.
- **(b) Python indentation with any width** and an indent stack. More
  flexible, a little more work per line, and the error messages are worse.
- **(c) Braces or `end`** (Lua). Free-form layout, so the lexer never looks at
  column positions. `misc/notes.txt:491` already asks whether braces would be
  faster. They don't change the asymptotics. Indentation work only happens
  once per line, and newlines get scanned anyway.

**Recommendation: (a).** It's cheap and it's the luv look. The token protocol
is in D3.
