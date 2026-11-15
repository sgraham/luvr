" Quit when a (custom) syntax file was already loaded
if exists("b:current_syntax")
  finish
endif

syn case match

" Keywords
syn keyword     luvKeyword       alignof
syn keyword     luvKeyword       and
syn keyword     luvKeyword       as
syn keyword     luvKeyword       break
syn keyword     luvKeyword       cast
syn keyword     luvKeyword       check
syn keyword     luvKeyword       continue
syn keyword     luvKeyword       def
syn keyword     luvKeyword       elif
syn keyword     luvKeyword       else
syn keyword     luvKeyword       for
syn keyword     luvKeyword       foreign
syn keyword     luvKeyword       global
syn keyword     luvKeyword       if
syn keyword     luvKeyword       import
syn keyword     luvKeyword       in
syn keyword     luvKeyword       nonlocal
syn keyword     luvKeyword       not
syn keyword     luvKeyword       offsetof
syn keyword     luvKeyword       on
syn keyword     luvKeyword       or
syn keyword     luvKeyword       pass
syn keyword     luvKeyword       print
syn keyword     luvKeyword       return
syn keyword     luvKeyword       sizeof
syn keyword     luvKeyword       struct
syn keyword     luvKeyword       typeof
syn keyword     luvKeyword       with
hi def link     luvKeyword       Keyword

syn keyword     luvConst true
syn keyword     luvConst false
syn keyword     luvConst null
hi def link     luvConst Constant

" Numbers. Later matches win when several start at the same column, so floats
" are after integers.
syn match luvDec /\<[0-9][0-9_]*\(i8\|u8\|i16\|u16\|i32\|u32\|i64\|u64\|f32\|f64\)\?\>/
syn match luvHex /\<0x[0-9A-Fa-f_]*[0-9A-Fa-f][0-9A-Fa-f_]*\(i8\|u8\|i16\|u16\|i32\|u32\|i64\|u64\)\?\>/
syn match luvOct /\<0o[0-7_]*[0-7][0-7_]*\(i8\|u8\|i16\|u16\|i32\|u32\|i64\|u64\)\?\>/
syn match luvBin /\<0b[01_]*[01][01_]*\(i8\|u8\|i16\|u16\|i32\|u32\|i64\|u64\)\?\>/
syn match luvFloat /\<[0-9][0-9_]*\.[0-9][0-9_]*\([eE][+-]\?[0-9][0-9_]*\)\?\(f32\|f64\)\?\>/
syn match luvFloat /\<[0-9][0-9_]*[eE][+-]\?[0-9][0-9_]*\(f32\|f64\)\?\>/
hi def link     luvDec Number
hi def link     luvHex Number
hi def link     luvOct Number
hi def link     luvBin Number
hi def link     luvFloat Float

" Identifiers are classified by spelling, the same as the lexer does. After
" any leading underscores, a single capital, or a capital followed by
" anything containing a lowercase letter, is a type. Other names starting
" with a capital are ALL_CAPS constants.
syn match       luvConstVar '\<_*[A-Z][A-Z0-9_]\+\>'
hi def link     luvConstVar Preproc

" Basic type declarations
syn keyword     luvType bool byte codept int uint float double
syn keyword     luvType u8 u16 u32 u64 i8 i16 i32 i64
syn keyword     luvType f16 f32 f64
syn keyword     luvType str
syn match       luvType '\<_*[A-Z]\>'
syn match       luvType '\<_*[A-Z][A-Za-z0-9_]*[a-z][A-Za-z0-9_]*\>'
hi def link     luvType Type

" Builtin functions
syn keyword     luvBuiltins enumerate len range reversed self
syn keyword     luvBuiltins alloc
hi def link     luvBuiltins Function

" Decorator
syn region      luvDecorator start=+@+ end=+\w\++
hi def link     luvDecorator         Macro

" Strings. Raw newlines aren't allowed in a string, including inside \( ),
" so everything is oneline. Escapes are matched before \( so that \\( is a
" backslash followed by a plain '('. Any other backslash is an error.
syn region      luvString            start=/"/ end=/"/ oneline contains=luvEscapeError,luvEscape,luvInterpolation
syn match       luvEscapeError       /\\./ contained
syn match       luvEscape            /\\[ntr\\"0]\|\\x\x\x\|\\u{\x\{1,6}}/ contained
syn region      luvInterpolation     matchgroup=luvInterpolationDelim start=/\\(/ end=/)/ oneline contained contains=@luvExpr
syn region      luvParens            start=/(/ end=/)/ oneline contained transparent contains=@luvExpr
syn cluster     luvExpr              contains=luvKeyword,luvConst,luvDec,luvHex,luvOct,luvBin,luvFloat,luvConstVar,luvType,luvBuiltins,luvString,luvParens

hi def link     luvString            String
hi def link     luvEscape            SpecialChar
hi def link     luvEscapeError       Error
hi def link     luvInterpolationDelim Delimiter

" Comments
syn keyword     luvTodo              contained TODO XXX BUG
syn cluster     luvCommentGroup      contains=luvTodo
syn region      luvComment           start="#" end="$" contains=@luvCommentGroup,@Spell

hi def link     luvComment           Comment
hi def link     luvTodo              Todo

syn sync minlines=500

let b:current_syntax = "luv"
