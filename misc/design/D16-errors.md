# D16 Errors and positions

- Track `line` in the lexer. It's free because indentation handling visits
  every `\n`. Each token carries a `u32` byte offset. Compute columns lazily
  on error, like luv60's `get_location_and_line_slow`.
- First error → `longjmp` to the driver (Lua's model). No recovery. If editor
  integration later wants multiple errors, the top-level skim ([D11](D11-forward-refs.md)) already
  gives natural resync points: indent-0 lines.
- Keep `check_match`'s "`)` expected (to close `(` at line N)".
