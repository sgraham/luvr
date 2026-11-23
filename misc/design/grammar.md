# 3. Grammar sketch (with the recommendations applied)

FIRST sets are shown where they decide anything. `TYPESTART` = `{TYPENAME, TYPEOF,
builtin-type keywords, '*', '[', '{'}`.

```
module        -> { import } { toplevel_stat } EOS
import        -> IMPORT STRING [AS NAME] NEWLINE

toplevel_stat -> funcdef
               | ON receiver ( DEF funcsig suite | FOREIGN funcsig NEWLINE )   # D22
               | STRUCT TYPENAME [ type_params ] ':' INDENT field { field } DEDENT
               | FOREIGN funcsig NEWLINE
               | simple_stat end                 # global decls / `X = const`

funcdef       -> DEF funcsig ':' suite
funcsig       -> [ type ] NAME '(' [ param { ',' param } ] ')'
                 # [type] is decided by TYPESTART vs NAME: LL(1) via D4
param         -> type NAME | NAME                # bare NAME: `self` only
field         -> type NAME [ '=' expr ] end

suite         -> INDENT { stat } DEDENT
               | simple_stat end                 # D3a one-liner

stat          -> IF expr ':' suite { ELIF expr ':' suite } [ ELSE ':' suite ]
               | WHILE expr ':' suite
               | FOR NAME [ ',' NAME ] IN expr ':' suite
               | WITH expr [ AS NAME ] ':' suite
               | funcdef
               | simple_stat end

simple_stat   -> RETURN [ expr ]                 # expr absent iff t in {NEWLINE,DEDENT,EOS}
               | BREAK | CONTINUE | PASS
               | PRINT expr | CHECK expr
               | GLOBAL NAME { ',' NAME }
               | decl_or_expr_stat

decl_or_expr_stat
               -> TYPESTART-led:  type NAME [ '=' expr ]           # D5
                | NAME/TYPENAME-led: primary_chain
                     ( NAME [ '=' expr ]        # chain was VTYPE → decl
                     | '=' expr | AUGOP expr    # chain was lvalue
                     | <nothing> )              # chain was a call

end           -> NEWLINE | <peek DEDENT> | <peek EOS>

type          -> '*' type
               | '[' [ expr ] ']' type          # []T list, [N]T array
               | '{' type '}' type              # {K}V dict
               | BUILTIN_TYPE
               | TYPENAME [ type_args ] | NAME '.' TYPENAME [ type_args ]
               | TYPEOF '(' expr ')'
type_args     -> '{' type { ',' type } '}'       # D21
type_params   -> '{' TYPEPARAM { ',' TYPEPARAM } '}'

expr          -> subexpr(-1)                    # Lua 4 precedence climbing, D8
primary       -> NUMBER | string | TRUE | FALSE | NULL
               | NAME | TYPENAME                # TYPENAME '(' → compound literal
               | '(' expr ')'
               | '[' list_or_compr ']'          # D10; list form allows trailing ',' (D19a)
               | '{' [ expr ':' expr { ',' expr ':' expr } [','] ] '}'   # trailing ',' ok (D19a)
               | (SIZEOF|ALIGNOF) '(' type_or_expr ')'   # D7 first-token rule
               | OFFSETOF '(' type ',' NAME ')'
               | CAST type '(' expr ')'           # D7
string        -> STRING
               | STRING_BEGIN expr { STRING_MID expr } STRING_END   # D13 \( )
postfix       -> { '.' NAME | '.' TYPENAME | '[' slice_or_index ']' | '(' args ')'
                 | type_args }                  # only after a type (D21)
args          -> [ arg { ',' arg } ]
arg           -> '.' NAME '=' expr | expr       # designator, D9
```
