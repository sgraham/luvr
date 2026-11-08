// parser.h: recursive-descent parser for luv, structured after Lua 4.0.1's
// lparser.
//
// One token of lookahead (the lexer's current token), no backtracking, no
// token buffer. For now it only accepts or rejects: there's no code
// generation or symbol resolution yet.

#ifndef PARSER_H
#define PARSER_H

#include "lex.h"

// Parses a whole source file. Returns normally if the file is syntactically
// valid; on the first syntax error, longjmps to ctx->on_error with the
// message and location stored in ctx (see ctx_error_location()).
//
// As with lex_init(), buf must have LEX_PADDING zero bytes after len and
// must outlive ctx.
void parse_file(Ctx* ctx, const char* filename, const u8* buf, size_t len);

#endif  // PARSER_H
