# D12 Nested functions and captures

luv captures **by value at definition time** (`nested_simple_upval.luv`,
`nested_capturing_func_with_upval.luv`). Lua 4.0.1 had the same semantics,
spelled `%name` and limited to the immediately enclosing function
(`pushupvalue`, `lparser.c:229`).

- Implicit capture (current luv): the parser walks the `FuncState->prev`
  chain like `search_local` (`lparser.c:187`) and records upvalues per
  function. LL(1) either way, since this is name resolution.
- Explicit `%x` (Lua 4): cheaper analysis, uglier.

**Recommendation: implicit**, with multi-level capture as luv already
supports (`nested_two_levels.luv`). Use Lua's `FuncState` chain directly.
