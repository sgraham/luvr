// Recursive-descent parser for luv.
//
// The only lookahead is the lexer's current token. There's no backtracking
// and no token buffer. It only accepts or rejects. There's no code generation
// or symbol resolution yet.

#ifndef PARSER_H
#define PARSER_H

#include "lex.h"

// Returns normally if the file is syntactically valid. On the first syntax
// error, the message and location are stored in ctx (see
// ctx_error_location()) and it longjmps to ctx->on_error.
//
// As with lex_init(), buf must have LEX_PADDING zero bytes after len and
// must outlive ctx.
void parse_file(Ctx* ctx, const char* filename, const u8* buf, size_t len);

#endif  // PARSER_H
