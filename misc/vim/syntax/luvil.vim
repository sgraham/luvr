" Quit when a (custom) syntax file was already loaded
if exists("b:current_syntax")
  finish
endif

syn case match

syn keyword     luvilKeyword     data func slot
hi def link     luvilKeyword     Keyword

syn keyword     luvilType        void i8 u8 i16 u16 i32 i64 f32 f64 sym
hi def link     luvilType        Type

" Opcodes are always all caps, so new ones don't need to be listed here.
syn match       luvilOp          /\<[A-Z][A-Z0-9_]*\>/
hi def link     luvilOp          Statement

syn match       luvilNumber      /-\=\<\d\+\>/
hi def link     luvilNumber      Number

syn match       luvilRef         /%\d\+/
syn match       luvilSlot        /\$\d\+/
syn match       luvilSym         /@\.\=\w\+/
syn match       luvilBlock       /:\d\+/
hi def link     luvilRef         Identifier
hi def link     luvilSlot        Identifier
hi def link     luvilSym         Function
hi def link     luvilBlock       Label

syn region      luvilString      start=/"/ skip=/\\./ end=/"/ oneline contains=luvilEscape
syn match       luvilEscape      /\\\(x\x\x\|.\)/ contained
hi def link     luvilString      String
hi def link     luvilEscape      SpecialChar

syn keyword     luvilTodo        contained TODO XXX BUG
syn region      luvilComment     start=";" end="$" contains=luvilTodo,@Spell
hi def link     luvilComment     Comment
hi def link     luvilTodo        Todo

let b:current_syntax = "luvil"
