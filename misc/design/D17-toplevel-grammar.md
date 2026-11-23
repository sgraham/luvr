# D17 Top-level grammar vs. nested grammar

luv60 uses one `parse_statement(bool toplevel)` with `if (toplevel) error(...)`
checks (`parse.c:5637`). Proposal: **two functions**, `toplevel_stat()` and
`stat()`, each with its own `switch`. Each stays smaller and gives better
error messages, and "imports must come first" becomes part of the grammar:

```
module -> { import } { toplevel_stat } EOS
```

Nested `def` stays legal inside functions. `struct`, `on`, `foreign` and
`import` are top-level only.
