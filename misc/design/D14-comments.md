# D14 Comments

- `#` to end of line, skipped with `memchr('\n')`.
- **Trailing comments** (`x = 1  # why`): luv60 rejects them
  (`TOK_INVALID_TRAILING_COMMENT`) because of the SIMD indexer. A scalar
  lexer handles them for free. **Allow them.**
- Block comments: none.
