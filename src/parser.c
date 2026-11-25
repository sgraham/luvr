#include "luvr.h"

#include "prelude.h"

// This file is compiled twice, by parser_check.c with EMIT 0 and by
// parser_emit.c with EMIT 1.
#if !defined(EMIT)
#  error "parser.c is only compiled through parser_check.c or parser_emit.c"
#endif
#if EMIT
#  define PARSE_FILE parse_file_emit
#else
#  define PARSE_FILE parse_file
#endif

typedef union ConstValue ConstValue;
typedef struct ExpDesc ExpDesc;
typedef struct Field Field;
typedef struct GlobalFunc GlobalFunc;
typedef struct Local Local;
typedef struct LogicalJoin LogicalJoin;
typedef struct Loop Loop;
typedef struct NamedConst NamedConst;
typedef struct Parser Parser;
typedef struct Targets Targets;
typedef struct TypeId TypeId;
typedef struct TypeInfo TypeInfo;

// Types are compared by index. u is a FixedType, or for types that are
// created while parsing, an index >= TY_NUM_FIXED in Parser.types.
struct TypeId {
  u32 u;
};

typedef enum FixedType {
  TY_UNKNOWN,        // Not inferred yet. Checks against it always pass.
  TY_UNTYPED_INT,    // An integer constant that hasn't been given a type yet.
  TY_UNTYPED_FLOAT,  // Similarly, a float constant.
  TY_VOID,           // The return type of a function without one.
  TY_BOOL,
  TY_STR,
  TY_NULL,  // The type of `null`, which can be assigned to any pointer.
  // Integer types are contiguous, from TY_FIRST_INT to TY_LAST_INT.
  TY_I8,
  TY_I16,
  TY_I32,
  TY_I64,
  TY_U8,
  TY_U16,
  TY_U32,
  TY_U64,
  TY_F32,
  TY_F64,
  TY_NUM_FIXED,  // TypeIds from here on are created while parsing.
  TY_FIRST_INT = TY_I8,
  TY_LAST_INT = TY_U64,
} FixedType;

static const TypeId ty_unknown = {TY_UNKNOWN};
static const TypeId ty_untyped_int = {TY_UNTYPED_INT};
static const TypeId ty_untyped_float = {TY_UNTYPED_FLOAT};
static const TypeId ty_void = {TY_VOID};
static const TypeId ty_bool = {TY_BOOL};
static const TypeId ty_str = {TY_STR};
static const TypeId ty_null = {TY_NULL};
static const TypeId ty_i8 = {TY_I8};
static const TypeId ty_i16 = {TY_I16};
static const TypeId ty_i32 = {TY_I32};
static const TypeId ty_i64 = {TY_I64};
static const TypeId ty_u8 = {TY_U8};
static const TypeId ty_u16 = {TY_U16};
static const TypeId ty_u32 = {TY_U32};
static const TypeId ty_u64 = {TY_U64};
static const TypeId ty_f32 = {TY_F32};
static const TypeId ty_f64 = {TY_F64};

// Locals that can be in scope at once, including those of enclosing functions.
#define MAX_LOCALS 1024

// The emitting build keeps a call's argument refs on the C stack while they're
// parsed.
#define MAX_CALL_ARGS 64

typedef enum LocalKind {
  LK_VAR,
  LK_PARAM,
  LK_CONST,
  // Named in a 'global' statement, so assignments in this function go to the
  // global rather than declaring a local.
  LK_GLOBAL,
} LocalKind;

// Untyped integer constants need more than 64 bits so that every value of
// every integer type fits, as well as intermediates like (1 << 64) - 1.
#if defined(__SIZEOF_INT128__)
typedef __int128 ConstInt;
typedef unsigned __int128 ConstUInt;
#else
#  error "ConstInt needs an implementation without __int128"
#endif

// The value of a constant, which is chosen by its type.
union ConstValue {
  ConstInt i;
  double f;
  bool b;
};

// The FixedTypes' entries in Parser.types are TYPE_NONE, except for str, which
// the prelude defines as a struct.
typedef enum TypeKind {
  TYPE_NONE,
  TYPE_FUNC,
  TYPE_PTR,
  TYPE_STRUCT,
  TYPE_ARRAY,
} TypeKind;

// A type that's created while parsing. Function types aren't interned, so
// each function has its own, even when signatures are the same. Pointer types
// are interned through `ptr`, and fixed-size arrays through
// Parser.array_types, so that they can be compared by index. Each struct
// definition creates one type.
struct TypeInfo {
  TypeKind kind;
  TypeId ptr;  // *T for this T, or TY_UNKNOWN if it hasn't been created yet
  union {
    struct {  // TYPE_FUNC
      TypeId ret;
      u32 first_param;  // in Parser.param_types
      u32 num_params;
    };
    TypeId pointee;  // TYPE_PTR
    struct {         // TYPE_STRUCT and TYPE_ARRAY (because of shared .agg/.image)
      // A struct's agg.u is AGG_NONE if its layout isn't known (see
      // type_has_layout()). An array's is added by type_agg() when it's
      // first needed.
      AggId agg;
      // The initial value, or DATA_NONE if it's all zeros. Only set when
      // EMIT. An array only has one when its elements are structs with
      // defaults, and it's added along with its agg.
      DataId image;
      union {
        struct {  // TYPE_STRUCT
          NameId name;
          u32 first_field;  // in Parser.fields
          // The fields' offsets are only set if the layout is known.
          u32 num_fields;
        };
        struct {  // TYPE_ARRAY
          TypeId elem;
          u32 count;
        };
      };
    };
  };
};

// A struct's field. The offset is only set if the struct's layout is known.
struct Field {
  NameId name;
  TypeId type;
  u32 offset;
};

// A named constant's value, which is kept apart from Local and Module.globals
// to reduce unnecessarily large searches.
struct NamedConst {
  TypeId type;
  ConstValue cv;
};

// A top-level function. Its sym is FUNC_NO_SYM in the checking build, and
// for foreign functions, which don't have symbols yet.
struct GlobalFunc {
  TypeId type;
  u32 sym;
};

#define FUNC_NO_SYM UINT32_MAX

// A parameter or local variable of the function being parsed, or of one that
// encloses it.
struct Local {
  NameId name;
  TypeId type;
  LocalKind kind;
  // Only when EMIT. A parameter is a value, so it doesn't need a slot.
  union {
    SlotId slot;  // LK_VAR
    Ref ref;      // LK_PARAM
  };
  // The Name's binding before this was declared. It's restored when the
  // function ends.
  Local* shadowed;
};

typedef enum GlobalKind {
  GK_NONE,
  GK_VAR,
  GK_FUNC,
  GK_TYPE,
  GK_MODULE,
  GK_CONST,
  // Only in Parser.prelude_globals, for functions that the compiler
  // implements, such as len.
  GK_BUILTIN,
} GlobalKind;

// GlobalInfo is a GlobalKind in the low bits. The higher bits are the index of
// the value in Parser.global_consts for GK_CONST, and in Parser.global_funcs
// for GK_FUNC. For GK_TYPE, they're the TypeId, which is TY_UNKNOWN for a
// generic struct, since each instantiation will be a different type.
#define GLOBAL_KIND_BITS 3
#define GLOBAL_KIND_MASK ((1u << GLOBAL_KIND_BITS) - 1)

// Initial capacity of Parser.types and Parser.param_types, so that they don't
// need to grow for small programs.
#define INITIAL_TYPES 1024

// Minimum size of Module.globals. It must be a power of 2.
#define MIN_GLOBALS 64

struct Parser {
  LexState ls;
  Module* mod;
  Local locals[MAX_LOCALS];
  NamedConst local_consts[MAX_LOCALS];  // for LK_CONST locals, by index in locals
  VEC(NamedConst) global_consts;
  VEC(GlobalFunc) global_funcs;
  VEC(TypeInfo) types;  // by TypeId, with unused entries for the FixedTypes
  VEC(TypeId) param_types;
  VEC(Field) fields;
  // The image of the struct whose body is being parsed. Only used when EMIT.
  VEC(u8) image;
  // Each [N]T, by a hash of N and T. The slot's val is the TypeId.
  HashTable array_types;
  // The prelude's globals by NameId, for names below prelude_name_end. See
  // prelude_load().
  GlobalInfo* prelude_globals;
  u32 prelude_name_end;
  u32 num_locals;
  u32 func_base;   // first of the current function's entries in locals
  u32 func_depth;  // 0 at top level
  // Bit c - 'A' is set for each type parameter declared by the current
  // struct's list or method's receiver.
  u32 type_params;
  TypeId ret_type;  // of the current function
  Loop* loop;       // the current function's innermost loop, or NULL
  // The first name in the current for header. It's a loop variable if it's
  // followed by 'in' or ','. Names can't be at offset 0 after 'for', so 0 is
  // none.
  SrcLoc loop_var_at;
  // The type that the next list literal gets, set by expr_expect(). It's a
  // field rather than a parameter so that primary() is the only caller of
  // postfix(), which keeps it inlined there.
  TypeId literal_expected;
  // Set while parsing an expression that has to be a constant, so that struct
  // literals are constant data instead of stores into a temporary.
  bool const_context;
  bool in_foreign;  // in a foreign function's signature
  // The offsets of the prelude str's _data and _len, which are both 8 bytes.
  // Set at the end of its definition.
  u32 str_data_offset;
  u32 str_len_offset;
  IlFunc il;  // of the current function, only used when EMIT
};

// The syntactical shape of parsing an expression. Used to decide whether
// an expression can be assigned to, used as a statement, or was a type.
typedef enum Shape {
  SH_VALUE,   // anything else: literals, operators, ...
  SH_NAME,    // a bare name: assignable
  SH_LVALUE,  // a field or index: assignable
  // A parameter of the current function, or a field or element of one that's
  // a struct or array. Parameters are immutable, so this isn't assignable,
  // and its address can't be taken.
  SH_PARAM,
  SH_CALL,            // a function or method call: valid as a statement
  SH_TYPE,            // TypeName or module.TypeName
  SH_STRUCT_LITERAL,  // Stuff(...): '&' allocates it in the current arena
} Shape;

// How control leaves a statement or block. A block's flow is its last
// statement's, since a return, break or continue has to be last.
typedef enum Flow {
  FLOW_FALLS_THROUGH,  // can reach whatever follows
  // Can't reach whatever follows. It always ends in a return, break or
  // continue, or it's a 'for:' without a break.
  FLOW_ENDS,
} Flow;

// Where an expression's value is.
typedef enum ExpKind {
  EK_REF,         // the result of an instruction or constant, in ExpDesc.ref
  EK_CONST,       // in ExpDesc.cv
  EK_LOCAL,       // in ExpDesc.slot, not loaded yet
  EK_ADDR,        // in memory at ExpDesc.base + ExpDesc.offset, not loaded yet
  EK_CONST_DATA,  // in Ctx.data at ExpDesc.data. Only string literals so far.
  EK_FUNC,        // a function, whose symbol is ExpDesc.sym
} ExpKind;

// What is known about an expression that has been parsed but not yet emitted.
struct ExpDesc {
  TypeId type;
  u8 shape;  // Shape
  u8 kind;   // ExpKind
  union {
    SlotId slot;  // EK_LOCAL
    Ref ref;      // EK_REF
    DataId data;  // EK_CONST_DATA
    u32 sym;      // EK_FUNC
    struct {      // EK_ADDR
      Ref base;
      u32 offset;
    };
  };
  ConstValue cv;
};

static_assert(sizeof(ExpDesc) == 32, "unexpected ExpDesc size");

static void expr(Parser* p);
static void subexpr(Parser* p, int limit, ExpDesc* e);
static void binop_rest(Parser* p, int limit, ExpDesc* e, SrcLoc start);
static void primary(Parser* p, ExpDesc* e);
static TypeId type(Parser* p);
static u32 array_size(Parser* p);
static IlType il_mem_type_of(Parser* p, TypeId t);
static AggId type_agg(Parser* p, TypeId t);
static Flow stat(Parser* p);
static Flow suite(Parser* p);
static void parse_module(Parser* p);

// Token helpers

#define T(p) ((p)->ls.pos.t.kind)

static void next(Parser* p) {
  lex_next(&p->ls);
}

#define error_at(p, offset, ...) \
  ctx_error((p)->ls.ctx, (p)->ls.src.filename, (p)->ls.src.buf, (offset), __VA_ARGS__)

static bool type_is_created(TypeId t) {
  return t.u >= TY_NUM_FIXED;
}

static TypeInfo* type_info(Parser* p, TypeId t) {
  return &p->types.items[t.u];
}

static bool type_is_function(Parser* p, TypeId t) {
  return type_is_created(t) && type_info(p, t)->kind == TYPE_FUNC;
}

static bool type_is_pointer(Parser* p, TypeId t) {
  return type_is_created(t) && type_info(p, t)->kind == TYPE_PTR;
}

// str is the only FixedType that's a struct. Checking the kind alone would be
// enough, but testing for a created type first avoids loading the TypeInfo of
// the other builtin types, which was measurable.
static bool type_is_struct(Parser* p, TypeId t) {
  return (type_is_created(t) || t.u == TY_STR) && type_info(p, t)->kind == TYPE_STRUCT;
}

static bool type_is_array(Parser* p, TypeId t) {
  return type_is_created(t) && type_info(p, t)->kind == TYPE_ARRAY;
}

static bool type_is_aggregate(Parser* p, TypeId t) {
  return type_is_struct(p, t) || type_is_array(p, t);
}

// Describes a type for error messages, as it would follow "not". The result
// is only for the arguments of error_at().
static const char* type_str(Parser* p, TypeId t) {
  if (type_is_pointer(p, t)) {
    return ctx_err_str(p->ls.ctx, "*%s", type_str(p, type_info(p, t)->pointee));
  }
  if (type_is_array(p, t)) {
    const TypeInfo* info = type_info(p, t);
    return ctx_err_str(p->ls.ctx, "[%u]%s", info->count, type_str(p, info->elem));
  }
  if (type_is_struct(p, t)) {
    const Name* n = lex_name(p->ls.ctx, type_info(p, t)->name);
    return ctx_err_str(p->ls.ctx, "%.*s", (int)n->len, (const char*)n->str);
  }
  if (type_is_function(p, t)) {
    return "a function";
  }
  switch ((FixedType)t.u) {
    case TY_UNKNOWN:
    case TY_STR:  // a struct, so it's described above
    case TY_NUM_FIXED:
      break;
    case TY_UNTYPED_INT:
      return "an integer constant";
    case TY_UNTYPED_FLOAT:
      return "a float constant";
    case TY_VOID:
      return "void";
    case TY_BOOL:
      return "bool";
    case TY_NULL:
      return "null";
    case TY_I8:
      return "i8";
    case TY_I16:
      return "i16";
    case TY_I32:
      return "i32";
    case TY_I64:
      return "int";
    case TY_U8:
      return "u8";
    case TY_U16:
      return "u16";
    case TY_U32:
      return "u32";
    case TY_U64:
      return "u64";
    case TY_F32:
      return "f32";
    case TY_F64:
      return "f64";
  }
  return "an unknown type";
}

// Describes the current token for error messages. The result is only for the
// arguments of error_at().
static const char* token_str(Parser* p) {
  const Token* t = &p->ls.pos.t;
  switch (t->kind) {
    case TK_TYPEPARAM:
      return ctx_err_str(p->ls.ctx, "type parameter '%c'", lex_name(p->ls.ctx, t->v.name)->str[0]);
    case TK_NAME:
    case TK_TYPENAME: {
      const Name* n = lex_name(p->ls.ctx, t->v.name);
      return ctx_err_str(p->ls.ctx, "'%.*s'", (int)n->len, (const char*)n->str);
    }
    case TK_INTLIT:
    case TK_FLOATLIT:
      return "number";
    case TK_STRING:
    case TK_STRING_BEGIN:
      return "string";
    case TK_NEWLINE:
      return "end of line";
    case TK_INDENT:
      return "indent";
    case TK_DEDENT:
      return "dedent";
    case TK_EOS:
      return "end of file";
    default:
      return ctx_err_str(p->ls.ctx, "'%s'", tok_kind_name(t->kind));
  }
}

NORETURN static void error_expected(Parser* p, const char* what) {
  error_at(p, p->ls.pos.t.loc, "Expected %s, found %s.", what, token_str(p));
}

static void check(Parser* p, TokenKind kind) {
  if (T(p) != kind) {
    error_expected(p, ctx_err_str(p->ls.ctx, "'%s'", tok_kind_name(kind)));
  }
  next(p);
}

static bool optional(Parser* p, TokenKind kind) {
  if (T(p) != kind) {
    return false;
  }
  next(p);
  return true;
}

// Like check(), but if the closer is missing and the opener was on another
// line, the error includes the opener's line.
static void check_match(Parser* p, TokenKind what, TokenKind who, SrcLoc who_loc) {
  if (T(p) == what) {
    next(p);
    return;
  }
  ctx_error_unclosed(&p->ls, what, who, who_loc, token_str(p));
}

static void check_name(Parser* p, const char* what) {
  if (T(p) != TK_NAME) {
    error_expected(p, what);
  }
  next(p);
}

// Like check_name(), but returns the name, and its location in *at.
static NameId expect_name(Parser* p, const char* what, SrcLoc* at) {
  if (T(p) != TK_NAME) {
    error_expected(p, what);
  }
  NameId name = p->ls.pos.t.v.name;
  *at = p->ls.pos.t.loc;
  next(p);
  return name;
}

// Global table

static GlobalInfo module_global_info(const Module* m, NameId name) {
  const GlobalInfo* info = namemap_find(&m->globals, name);
  return info ? *info : GK_NONE;
}

static GlobalKind module_global_kind(const Module* m, NameId name) {
  return (GlobalKind)(module_global_info(m, name) & GLOBAL_KIND_MASK);
}

static GlobalKind global_kind(Parser* p, NameId name) {
  return module_global_kind(p->mod, name);
}

// Looks for a name that's used, which is in the module's own globals or the
// prelude. Definitions only check the module's globals, so they can shadow
// the prelude.
static GlobalInfo global_info(Parser* p, NameId name) {
  GlobalInfo info = module_global_info(p->mod, name);
  if (info == GK_NONE && name.u < p->prelude_name_end) {
    info = p->prelude_globals[name.u];
  }
  return info;
}

static void declare_global(Parser* p, NameId name, GlobalInfo info) {
  namemap_set(p->ls.ctx, &p->mod->globals, name, info);
}

// Out of line, since uses of constants are rare compared to variables.
static NOINLINE void load_named_const(Parser* p, ExpDesc* e, const NamedConst* c) {
  e->type = c->type;
  e->kind = type_is_aggregate(p, c->type) ? EK_CONST_DATA : EK_CONST;
  e->cv = c->cv;
}

// Returns whether e's value is known at compile time, either in e itself or
// as constant data.
static bool exp_is_const(const ExpDesc* e) {
  return e->kind == EK_CONST || e->kind == EK_CONST_DATA;
}

// Locals
//
// Scope is the whole function: a local is declared by its first assignment
// and is visible from there to the end of the function.

static Name* name_of(Parser* p, NameId name) {
  return &p->ls.ctx->names.items[name.u];
}

static Local* lookup(Parser* p, NameId name) {
  return (Local*)name_of(p, name)->binding;
}

static Local* declare_local(Parser* p, NameId name, SrcLoc at, LocalKind kind) {
  if (p->num_locals == MAX_LOCALS) {
    error_at(p, at, "Too many local variables.");
  }
  Local* l = &p->locals[p->num_locals++];
  l->name = name;
  l->type = ty_unknown;
  Name* n = name_of(p, name);
  l->kind = kind;
  l->shadowed = (Local*)n->binding;
  n->binding = l;
  return l;
}

// The current function's local for name, ignoring those of enclosing functions.
static Local* lookup_in_func(Parser* p, NameId name) {
  Local* l = lookup(p, name);
  return l && l >= &p->locals[p->func_base] ? l : NULL;
}

// Declares name in the current function with type t, unless it's already one
// of the function's locals.
static void declare_if_new(Parser* p, NameId name, SrcLoc at, TypeId t) {
  if (p->func_depth == 0) {
    return;
  }
  if (!lookup_in_func(p, name)) {
    Local* l = declare_local(p, name, at, LK_VAR);
    l->type = t;
  }
}

static void pop_locals(Parser* p, u32 base) {
  for (u32 i = p->num_locals; i-- > base;) {
    const Local* l = &p->locals[i];
    name_of(p, l->name)->binding = l->shadowed;
  }
  p->num_locals = base;
}

// Returns the enclosing function's base, to pass to func_close().
static u32 func_open(Parser* p) {
  u32 outer = p->func_base;
  p->func_base = p->num_locals;
  ++p->func_depth;
  return outer;
}

static void func_close(Parser* p, u32 outer) {
  pop_locals(p, p->func_base);
  p->func_base = outer;
  --p->func_depth;
}

static bool is_builtin_type(TokenKind kind) {
  return kind >= TK_FIRST_TYPE && kind <= TK_LAST_TYPE;
}

// Tokens that can only start a type, never an expression statement. A NAME
// can start a type too (module.Type), but that case is handled by parsing the
// postfix chain first.
static bool is_type_start(TokenKind kind) {
  return is_builtin_type(kind) || kind == TK_TYPENAME || kind == TK_TYPEPARAM || kind == TK_STAR ||
         kind == TK_LSQUARE || kind == TK_LBRACE || kind == TK_TYPEOF;
}

// A simple statement ends at a NEWLINE, which it consumes, or at a DEDENT or
// end of file, which it leaves for the enclosing block.
static bool block_follow(TokenKind kind) {
  return kind == TK_DEDENT || kind == TK_EOS;
}

static void end_of_stat(Parser* p) {
  if (T(p) == TK_NEWLINE) {
    next(p);
  } else if (!block_follow(T(p))) {
    error_expected(p, "end of line");
  }
}

// Types
//
//   type -> '*' type
//         | '[' [expr] ']' type        ([]T list, [N]T array)
//         | '{' type '}' type          ({K}V dict)
//         | builtin
//         | TypeName [type_args] | name '.' TypeName [type_args]
//         | 'typeof' '(' expr ')'

// type_args -> '{' type {',' type} '}'
static void type_args(Parser* p) {
  SrcLoc at = p->ls.pos.t.loc;
  next(p);  // '{'
  for (;;) {
    type(p);
    if (!optional(p, TK_COMMA)) {
      break;
    }
  }
  check_match(p, TK_RBRACE, TK_LBRACE, at);
}

static void type_param(Parser* p) {
  if (T(p) != TK_TYPEPARAM) {
    error_expected(p, "type parameter (A-Z)");
  }
  p->type_params |= 1u << (name_of(p, p->ls.pos.t.v.name)->str[0] - 'A');
  next(p);
}

// type_params -> '{' TypeParam {',' TypeParam} '}'
static void type_params(Parser* p) {
  SrcLoc at = p->ls.pos.t.loc;
  next(p);  // '{'
  for (;;) {
    type_param(p);
    if (!optional(p, TK_COMMA)) {
      break;
    }
  }
  check_match(p, TK_RBRACE, TK_LBRACE, at);
}

// Imported modules

// The module that name is bound to by an import, or NULL if it isn't an
// import or a local shadows it.
static const Module* imported_module(Parser* p, NameId name) {
  if (name_of(p, name)->binding) {
    return NULL;
  }
  const u32* idx = namemap_find(&p->mod->imports, name);
  return idx ? p->ls.ctx->modules.items[*idx] : NULL;
}

// Checks that the current token, which follows `mod.`, is a type or variable
// that m defines.
static void check_module_member(Parser* p, const Module* m, NameId mod) {
  const Name* mn = name_of(p, mod);
  const Name* n = name_of(p, p->ls.pos.t.v.name);
  GlobalKind kind = module_global_kind(m, p->ls.pos.t.v.name);
  if (T(p) == TK_TYPENAME && kind != GK_TYPE) {
    error_at(p, p->ls.pos.t.loc, "Type '%.*s' not found in imported module '%.*s'.", (int)n->len,
             (const char*)n->str, (int)mn->len, (const char*)mn->str);
  }
  if (T(p) == TK_NAME && kind != GK_VAR && kind != GK_FUNC && kind != GK_CONST) {
    error_at(p, p->ls.pos.t.loc, "Variable '%.*s' not found in imported module '%.*s'.",
             (int)n->len, (const char*)n->str, (int)mn->len, (const char*)mn->str);
  }
}

static TypeId global_type(GlobalInfo info) {
  return (TypeId){info >> GLOBAL_KIND_BITS};
}

// The rest of a module.TypeName type, after the '.'. mod is the module's name,
// and at is its location. Type arguments make a type that isn't known yet.
static TypeId qualified_type_rest(Parser* p, NameId mod, SrcLoc at) {
  const Module* m = imported_module(p, mod);
  if (!m) {
    const Name* n = name_of(p, mod);
    error_at(p, at, "Undefined module '%.*s'.", (int)n->len, (const char*)n->str);
  }
  if (T(p) != TK_TYPENAME) {
    error_expected(p, "type name after module name");
  }
  check_module_member(p, m, mod);
  TypeId t = global_type(module_global_info(m, p->ls.pos.t.v.name));
  next(p);
  if (T(p) == TK_LBRACE) {
    type_args(p);
    return ty_unknown;
  }
  return t;
}

static void check_type_param(Parser* p) {
  const Name* n = name_of(p, p->ls.pos.t.v.name);
  u32 i = (u32)(n->str[0] - 'A');
  if (!(p->type_params & (1u << i))) {
    error_at(p, p->ls.pos.t.loc, "Undefined type parameter '%c'.", 'A' + i);
  }
}

static TypeId check_type_name(Parser* p) {
  NameId name = p->ls.pos.t.v.name;
  GlobalInfo info = global_info(p, name);
  if ((info & GLOBAL_KIND_MASK) != GK_TYPE) {
    const Name* n = name_of(p, name);
    error_at(p, p->ls.pos.t.loc, "Undefined type '%.*s'.", (int)n->len, (const char*)n->str);
  }
  return global_type(info);
}

static TypeId builtin_type(TokenKind kind) {
  switch (kind) {
    case TK_BOOL:
      return ty_bool;
    case TK_STR:
      return ty_str;
    case TK_I8:
      return ty_i8;
    case TK_I16:
      return ty_i16;
    case TK_I32:
      return ty_i32;
    case TK_I64:
    case TK_INT:
      return ty_i64;
    case TK_U8:
      return ty_u8;
    case TK_U16:
      return ty_u16;
    case TK_U32:
      return ty_u32;
    case TK_U64:
      return ty_u64;
    case TK_F32:
      return ty_f32;
    case TK_F64:
      return ty_f64;
    default:
      return ty_unknown;
  }
}

// A pointer to an unknown type is unknown too, so that it passes checks
// against any other pointer.
static TypeId type_pointer_to(Parser* p, TypeId t) {
  if (t.u == TY_UNKNOWN) {
    return ty_unknown;
  }
  if (type_info(p, t)->ptr.u == TY_UNKNOWN) {
    TypeId pt = {p->types.num};
    *vec_push(p->ls.ctx, &p->types) = (TypeInfo){.kind = TYPE_PTR, .pointee = t};
    type_info(p, t)->ptr = pt;
  }
  return type_info(p, t)->ptr;
}

static bool type_has_layout(Parser* p, TypeId t);
static u32 type_size(Parser* p, TypeId t);

// [count]elem, which is at `at` for errors. Like pointers, an array of an
// unknown type is unknown.
static TypeId type_array_of(Parser* p, TypeId elem, u32 count, SrcLoc at) {
  if (elem.u == TY_UNKNOWN) {
    return ty_unknown;
  }
  if (type_has_layout(p, elem) && (u64)count * type_size(p, elem) > UINT32_MAX) {
    error_at(p, at, "Array is too large.");
  }
  u32 tag = hashtable_tag_of_key((u64)elem.u << 32 | count);
  HashSlot* s = hashtable_first(&p->array_types, tag);
  for (; s->tag != 0; s = hashtable_next(&p->array_types, s, tag)) {
    const TypeInfo* info = &p->types.items[s->val];
    if (info->elem.u == elem.u && info->count == count) {
      return (TypeId){s->val};
    }
  }
  TypeId arr = {p->types.num};
  *vec_push(p->ls.ctx, &p->types) = (TypeInfo){
      .kind = TYPE_ARRAY, .agg = {AGG_NONE}, .image = {DATA_NONE}, .elem = elem, .count = count};
  hashtable_insert(p->ls.ctx, &p->array_types, s, tag, arr.u);
  return arr;
}

static TypeId type(Parser* p) {
  SrcLoc at = p->ls.pos.t.loc;
  switch (T(p)) {
    case TK_STAR:
      next(p);
      return type_pointer_to(p, type(p));
    case TK_LSQUARE: {
      next(p);
      if (T(p) == TK_RSQUARE) {  // []T isn't typed yet
        next(p);
        type(p);
        return ty_unknown;
      }
      u32 count = array_size(p);
      check_match(p, TK_RSQUARE, TK_LSQUARE, at);
      return type_array_of(p, type(p), count, at);
    }
    case TK_LBRACE:
      next(p);
      type(p);
      check_match(p, TK_RBRACE, TK_LBRACE, at);
      type(p);
      return ty_unknown;
    case TK_TYPEPARAM:
      check_type_param(p);
      next(p);
      return ty_unknown;
    case TK_TYPENAME: {
      TypeId t = check_type_name(p);
      next(p);
      if (T(p) == TK_LBRACE) {
        type_args(p);
        return ty_unknown;
      }
      return t;
    }
    case TK_NAME: {
      NameId mod = p->ls.pos.t.v.name;
      next(p);
      check(p, TK_DOT);
      return qualified_type_rest(p, mod, at);
    }
    case TK_TYPEOF:
      next(p);
      at = p->ls.pos.t.loc;
      check(p, TK_LPAREN);
      expr(p);
      check_match(p, TK_RPAREN, TK_LPAREN, at);
      return ty_unknown;
    default: {
      if (!is_builtin_type(T(p))) {
        error_expected(p, "type");
      }
      if (T(p) == TK_INT && p->in_foreign) {
        error_at(p, at, "Can't use int in a foreign declaration, use i32 or i64.");
      }
      TypeId t = builtin_type(T(p));
      next(p);
      return t;
    }
  }
}

// Expressions
//
// Precedence climbing. `not` has lower precedence than comparisons, and
// comparisons don't chain.

typedef enum BinOpr {
  OPR_OR,
  OPR_AND,
  OPR_EQ,
  OPR_NE,
  OPR_LT,
  OPR_LE,
  OPR_GT,
  OPR_GE,
  OPR_IN,
  OPR_NOTIN,
  OPR_BOR,
  OPR_BXOR,
  OPR_BAND,
  OPR_SHL,
  OPR_SHR,
  OPR_ADD,
  OPR_SUB,
  OPR_MUL,
  OPR_DIV,
  OPR_MOD,
  OPR_NONE,
} BinOpr;

// Priority of each binary operator. All are left-associative, so the right
// operand is parsed with the operator's priority as the limit.
static const u8 priority[] = {
    1,                          // or
    2,                          // and
    4,  4,  4,  4, 4, 4, 4, 4,  // comparisons, in, not in
    5,                          // |
    6,                          // ^
    7,                          // &
    8,  8,                      // << >>
    9,  9,                      // + -
    10, 10, 10,                 // * / %
};

enum {
  PRIORITY_NOT = 3,     // unary `not`: binds looser than comparisons
  PRIORITY_UNARY = 11,  // unary - ~ &: binds tighter than any binary operator
};

static BinOpr tok_to_binopr(TokenKind kind) {
  switch (kind) {
    case TK_OR:
      return OPR_OR;
    case TK_AND:
      return OPR_AND;
    case TK_EQEQ:
      return OPR_EQ;
    case TK_NE:
      return OPR_NE;
    case TK_LT:
      return OPR_LT;
    case TK_LE:
      return OPR_LE;
    case TK_GT:
      return OPR_GT;
    case TK_GE:
      return OPR_GE;
    case TK_IN:
      return OPR_IN;
    case TK_NOT:
      return OPR_NOTIN;  // in operator position, `not` must be `not in`
    case TK_PIPE:
      return OPR_BOR;
    case TK_CARET:
      return OPR_BXOR;
    case TK_AMP:
      return OPR_BAND;
    case TK_SHL:
      return OPR_SHL;
    case TK_SHR:
      return OPR_SHR;
    case TK_PLUS:
      return OPR_ADD;
    case TK_MINUS:
      return OPR_SUB;
    case TK_STAR:
      return OPR_MUL;
    case TK_SLASH:
      return OPR_DIV;
    case TK_PERCENT:
      return OPR_MOD;
    default:
      return OPR_NONE;
  }
}

static bool binopr_is_comparison(BinOpr op) {
  return op >= OPR_EQ && op <= OPR_NOTIN;
}

// Returns t unchanged unless it's an untyped constant, in which case it returns
// a storage type for it.
static TypeId type_concrete(TypeId t) {
  switch ((FixedType)t.u) {
    case TY_UNTYPED_INT:
      return ty_i64;
    case TY_UNTYPED_FLOAT:
      return ty_f64;
    default:
      return t;
  }
}

static bool type_is_untyped(TypeId t) {
  return t.u == TY_UNTYPED_INT || t.u == TY_UNTYPED_FLOAT;
}

static bool type_is_integer(TypeId t) {
  return t.u == TY_UNTYPED_INT || (t.u >= TY_FIRST_INT && t.u <= TY_LAST_INT);
}

static bool type_is_float(TypeId t) {
  return t.u == TY_UNTYPED_FLOAT || t.u == TY_F32 || t.u == TY_F64;
}

static bool type_is_unsigned(TypeId t) {
  return t.u >= TY_U8 && t.u <= TY_U64;
}

// The integer types are ordered by width, signed and then unsigned.
static int type_int_bits(TypeId t) {
  return 8 << ((t.u - TY_I8) & 3);
}

static bool const_fits_in_type(ConstInt v, TypeId t) {
  ConstInt limit = (ConstInt)1 << (type_int_bits(t) - !type_is_unsigned(t));
  return v < limit && v >= (type_is_unsigned(t) ? 0 : -limit);
}

// The result is only for the arguments of error_at(), as are those of
// float_str() and const_str().
static const char* int_str(Parser* p, ConstInt v) {
  // A sign, up to 39 digits, and the terminator.
  char buf[41];
  ConstUInt u = v < 0 ? -(ConstUInt)v : (ConstUInt)v;
  char* s = buf + sizeof(buf) - 1;
  *s = 0;
  for (;;) {
    *--s = (char)('0' + (int)(u % 10));
    u /= 10;
    if (!u) {
      break;
    }
  }
  if (v < 0) {
    *--s = '-';
  }
  return ctx_err_str(p->ls.ctx, "%s", s);
}

// The shortest form that reads back as the same double, so that the value
// shown in errors is the one that was written.
static const char* float_str(Parser* p, double v) {
  // %.17g is at most 24 chars, as in -2.2250738585072014e-308.
  char buf[32];
  int prec = 1;
  for (;;) {
    snprintf(buf, sizeof(buf), "%.*g", prec, v);
    if (strtod(buf, NULL) == v || prec == 17) {
      break;
    }
    ++prec;
  }
  return ctx_err_str(p->ls.ctx, "%s", buf);
}

static const char* const_str(Parser* p, const ExpDesc* e) {
  return type_is_float(e->type) ? float_str(p, e->cv.f) : int_str(p, e->cv.i);
}

// Whether a value of type `from` can be used, without a cast, where `to` is
// expected. An integer constant can become a float, but not the reverse, even
// when the float constant is integral.
static bool assignable(Parser* p, TypeId from, TypeId to) {
  return to.u == TY_UNKNOWN || from.u == TY_UNKNOWN || (to.u == from.u && from.u != TY_VOID) ||
         (from.u == TY_NULL && type_is_pointer(p, to)) ||
         (from.u == TY_UNTYPED_INT && (type_is_integer(to) || type_is_float(to))) ||
         (from.u == TY_UNTYPED_FLOAT && type_is_float(to));
}

// Whether `cast to(...)` accepts a value of type `from`. Numbers can be cast
// to any numeric type, and pointers to other pointers or to u64.
static bool castable(Parser* p, TypeId from, TypeId to) {
  bool from_num = type_is_integer(from) || type_is_float(from);
  bool to_num = type_is_integer(to) || type_is_float(to);
  bool from_ptr = type_is_pointer(p, from);
  bool to_ptr = type_is_pointer(p, to);
  return to.u == TY_UNKNOWN || from.u == TY_UNKNOWN || to.u == from.u || (from_num && to_num) ||
         (from_ptr && (to_ptr || to.u == TY_U64)) || (to_ptr && from.u == TY_U64);
}

// Integer constants have to be exactly representable as a float, since
// unlike decimal fractions they can be, and rounding one is likely a mistake.
// The bound keeps the conversion back to ConstInt in range when v rounds up
// to 2^127.
static bool const_exact_in_float(ConstInt v, TypeId t) {
  double d = t.u == TY_F32 ? (double)(float)v : (double)v;
  return d < 0x1p127 && (ConstInt)d == v;
}

// Out of line, since check_const_conversion() handles nearly all constants
// without it. Float constants are rounded, and are only out of range if they
// round to infinity.
static NOINLINE void check_const_fits(Parser* p, const ExpDesc* e, TypeId t, SrcLoc at) {
  if (!type_is_integer(e->type)) {
    float f = (float)e->cv.f;
    if (t.u == TY_F32 && (f > FLT_MAX || f < -FLT_MAX)) {
      error_at(p, at, "%s doesn't fit in f32.", float_str(p, e->cv.f));
    }
  } else if (type_is_integer(t) && !type_is_untyped(t) && !const_fits_in_type(e->cv.i, t)) {
    error_at(p, at, "%s doesn't fit in %s.", int_str(p, e->cv.i), type_str(p, t));
  } else if ((t.u == TY_F32 || t.u == TY_F64) && !const_exact_in_float(e->cv.i, t)) {
    error_at(p, at, "%s can't be represented exactly in %s.", int_str(p, e->cv.i), type_str(p, t));
  }
}

// Untyped constants are only range checked when they're given a type.
// Integers from 0 to 127 fit in every numeric type, and nearly all constants
// are in that range, so they skip the full check.
static void check_const_conversion(Parser* p, TypeId to, const ExpDesc* e, SrcLoc at) {
  if (e->kind != EK_CONST || !type_is_untyped(e->type)) {
    return;
  }
  if (e->type.u != TY_UNTYPED_INT || (ConstUInt)e->cv.i >= 128) {
    check_const_fits(p, e, to, at);
  }
}

// Out of line so that check_assign() is small enough to be inlined.
static NOINLINE NORETURN void assign_type_error(Parser* p, TypeId from, TypeId to, SrcLoc at) {
  error_at(p, at, "Can't assign %s to %s.", type_str(p, from), type_str(p, to));
}

// The size in [N]T has to be a constant integer, and can't be negative. It's
// limited to u32, like the size in bytes of every type.
static u32 array_size(Parser* p) {
  SrcLoc at = p->ls.pos.t.loc;
  ExpDesc e;
  subexpr(p, -1, &e);
  if (e.kind != EK_CONST || !type_is_integer(e.type)) {
    error_at(p, at, "Array size must be a constant integer.");
  }
  if (e.cv.i < 0) {
    error_at(p, at, "Array size can't be negative.");
  }
  if (e.cv.i > UINT32_MAX) {
    error_at(p, at, "Array size %s is too large.", int_str(p, e.cv.i));
  }
  return (u32)e.cv.i;
}

// Loop variables are only checked for in the error paths, so that primary()
// doesn't need to know about for headers.
static bool is_loop_var(Parser* p, SrcLoc at) {
  return at.u == p->loop_var_at.u && (T(p) == TK_IN || T(p) == TK_COMMA);
}

// There's no syntax for function types, so functions can't be values. A
// function's name followed by '=', or as a loop variable, declares a local
// that shadows it, as for other globals. Out of line, since it's only for
// names whose type isn't builtin.
static NOINLINE void check_function_called(Parser* p, const ExpDesc* e, SrcLoc at) {
  if (T(p) != TK_LPAREN && T(p) != TK_EQ && type_is_function(p, e->type) && !is_loop_var(p, at)) {
    error_at(p, at, "A function can only be called.");
  }
}

// Most uses of a void result are caught as type mismatches. This is for the
// uses that accept any type.
static void check_not_void(Parser* p, const ExpDesc* e, SrcLoc at) {
  if (e->type.u == TY_VOID) {
    error_at(p, at, "Can't use the result of a function with no return type.");
  }
}

// Checks that a value can be assigned to something of type `to`.
static void check_assign(Parser* p, TypeId to, const ExpDesc* e, SrcLoc at) {
  if (!assignable(p, e->type, to)) {
    assign_type_error(p, e->type, to, at);
  }
  check_const_conversion(p, to, e, at);
}

// Error messages for arithmetic and bitwise operators, taking the left and
// right operand types in order, except for subtraction, which reads better
// with them swapped.
// clang-format off
static const char* const arith_error[] = {
    [OPR_BOR] = "Can't bitwise or %s with %s.",
    [OPR_BXOR] = "Can't bitwise xor %s with %s.",
    [OPR_BAND] = "Can't bitwise and %s with %s.",
    [OPR_SHL] = "Can't shift %s left by %s.",
    [OPR_SHR] = "Can't shift %s right by %s.",
    [OPR_ADD] = "Can't add %s and %s.",
    [OPR_SUB] = "Can't subtract %s from %s.",
    [OPR_MUL] = "Can't multiply %s by %s.",
    [OPR_DIV] = "Can't divide %s by %s.",
    [OPR_MOD] = "Can't take %s modulo %s.",
};
// clang-format on

// The type of `a op b` for an arithmetic or bitwise op. Operands are numbers
// of the same type, apart from untyped constants. Floats only have +, -, *
// and /. A shift count can be any integer type, and strings can be added.
static TypeId type_for_arith(Parser* p, BinOpr op, TypeId a, TypeId b, SrcLoc at) {
  if (a.u == TY_UNKNOWN || b.u == TY_UNKNOWN) {
    return ty_unknown;
  }
  bool is_shift = op == OPR_SHL || op == OPR_SHR;
  bool is_concat = op == OPR_ADD && a.u == TY_STR && b.u == TY_STR;
  bool same_type = assignable(p, a, b) || assignable(p, b, a);
  bool ints = type_is_integer(a) && type_is_integer(b) && (is_shift || same_type);
  bool floats =
      op >= OPR_ADD && op <= OPR_DIV && same_type && (type_is_float(a) || type_is_float(b));
  if (!is_concat && !ints && !floats) {
    bool swap = op == OPR_SUB;
    error_at(p, at, arith_error[op], type_str(p, swap ? b : a), type_str(p, swap ? a : b));
  }
  if (is_shift || !type_is_untyped(a)) {
    return a;
  }
  // Two constants give a float constant if either is one.
  return type_is_untyped(b) && a.u == TY_UNTYPED_FLOAT ? a : b;
}

// `what` is the start of the error message, naming what needed the bool.
static void expect_bool(Parser* p, const ExpDesc* e, SrcLoc at, const char* what) {
  if (e->type.u != TY_UNKNOWN && e->type.u != TY_BOOL) {
    error_at(p, at, "%s expects bool, not %s.", what, type_str(p, e->type));
  }
}

// Messages for constant overflow, taking the operands in order, except for
// addition and subtraction, which read better with them swapped. Only
// division can overflow out of / and %, for MIN / -1, and only << out of the
// bitwise operators.
// clang-format off
static const char* const overflow_error[] = {
    [OPR_ADD] = "%s added to %s overflows.",
    [OPR_SUB] = "%s subtracted from %s overflows.",
    [OPR_MUL] = "%s multiplied by %s overflows.",
    [OPR_DIV] = "%s divided by %s overflows.",
    [OPR_SHL] = "%s shifted left by %s overflows.",
};
// clang-format on

static bool const_is_zero(const ExpDesc* e) {
  ASSERT(e->kind == EK_CONST);
  if (type_is_integer(e->type)) {
    return e->cv.i == 0;
  }
  return type_is_float(e->type) && e->cv.f == 0;
}

// A constant zero divisor is an error even when the dividend isn't constant.
static void check_divisor(Parser* p, BinOpr op, const ExpDesc* rhs, SrcLoc at) {
  if ((op == OPR_DIV || op == OPR_MOD) && rhs->kind == EK_CONST && const_is_zero(rhs)) {
    error_at(p, at, "Divide by zero.");
  }
}

// A constant shift count can't be negative, or at least the width of a typed
// value being shifted. `t` is the type of the value being shifted.
static void check_shift_count(Parser* p, BinOpr op, TypeId t, const ExpDesc* rhs, SrcLoc at) {
  if ((op != OPR_SHL && op != OPR_SHR) || rhs->kind != EK_CONST || !type_is_integer(rhs->type)) {
    return;
  }
  if (rhs->cv.i < 0) {
    error_at(p, at, "Shift count %s is negative.", int_str(p, rhs->cv.i));
  }
  if (type_is_integer(t) && !type_is_untyped(t) && rhs->cv.i >= type_int_bits(t)) {
    error_at(p, at, "Shift count %s is too large for %s.", int_str(p, rhs->cv.i), type_str(p, t));
  }
}

// Checks constant operands of an arithmetic, bitwise or comparison op, where
// t is the type that untyped constants are converted to. Out of line, since
// most operands aren't constants.
static NOINLINE void check_const_operands(Parser* p,
                                          BinOpr op,
                                          TypeId t,
                                          const ExpDesc* lhs,
                                          SrcLoc lhs_at,
                                          const ExpDesc* rhs,
                                          SrcLoc rhs_at,
                                          SrcLoc at) {
  // A shift count keeps its own type.
  if (op != OPR_SHL && op != OPR_SHR && !type_is_untyped(t)) {
    check_const_conversion(p, t, lhs, lhs_at);
    check_const_conversion(p, t, rhs, rhs_at);
  }
  check_divisor(p, op, rhs, at);
  check_shift_count(p, op, t, rhs, at);
}

// Folds constants `e op rhs` into e for an arithmetic or bitwise op, giving a
// value of type t. Typed results overflow at their own width, and untyped ones
// past the range of ConstInt or double. Divisors and shift counts have already
// been checked.
static void fold_arith(Parser* p, BinOpr op, TypeId t, ExpDesc* e, const ExpDesc* rhs, SrcLoc at) {
  bool overflow = false;
  double fr = 0;
  ConstInt ir = 0;
  if (type_is_float(t)) {
    double a = type_is_float(e->type) ? e->cv.f : (double)e->cv.i;
    double b = type_is_float(rhs->type) ? rhs->cv.f : (double)rhs->cv.i;
    switch (op) {
      case OPR_ADD:
        fr = a + b;
        break;
      case OPR_SUB:
        fr = a - b;
        break;
      case OPR_MUL:
        fr = a * b;
        break;
      case OPR_DIV:
        fr = a / b;
        break;
      default:
        ASSERT(false && "not a folded float operator");
        break;
    }
    if (t.u == TY_F32) {
      fr = (float)fr;
    }
    overflow = isinf(fr);
  } else {
    ConstInt a = e->cv.i;
    ConstInt b = rhs->cv.i;
    switch (op) {
      case OPR_ADD:
        overflow = __builtin_add_overflow(a, b, &ir);
        break;
      case OPR_SUB:
        overflow = __builtin_sub_overflow(a, b, &ir);
        break;
      case OPR_MUL:
        overflow = __builtin_mul_overflow(a, b, &ir);
        break;
      // C's / and % truncate toward zero, but are undefined for the minimum
      // ConstInt and -1.
      case OPR_DIV:
        if (b == -1) {
          overflow = __builtin_sub_overflow((ConstInt)0, a, &ir);
        } else {
          ir = a / b;
        }
        break;
      case OPR_MOD:
        ir = b == -1 ? 0 : a % b;
        break;
      case OPR_BOR:
        ir = a | b;
        break;
      case OPR_BXOR:
        ir = a ^ b;
        break;
      case OPR_BAND:
        ir = a & b;
        break;
      // An untyped count can be as large as ConstInt's limit. Shifting in
      // ConstUInt is required to keep overflow defined. Overflow is tested by
      // checking for any 1s that are lost by shifting left and then right.
      case OPR_SHL:
        if (b >= 128) {
          overflow = a != 0;
        } else {
          ir = (ConstInt)((ConstUInt)a << b);
          overflow = ir >> b != a;
        }
        break;
      // Negative values shift in sign bits, so they stay negative.
      case OPR_SHR:
        if (b >= 128) {
          ir = a < 0 ? -1 : 0;
        } else {
          ir = a >> b;
        }
        break;
      default:
        ASSERT(false && "not a folded integer operator");
        break;
    }
    overflow = overflow || (!type_is_untyped(t) && !const_fits_in_type(ir, t));
  }
  if (overflow) {
    const char* as = const_str(p, e);
    const char* bs = const_str(p, rhs);
    bool swap = op == OPR_ADD || op == OPR_SUB;
    error_at(p, at, overflow_error[op], swap ? bs : as, swap ? as : bs);
  }
  if (type_is_float(t)) {
    e->cv.f = fr;
  } else {
    e->cv.i = ir;
  }
}

// Casts of numeric constants give a constant, which has to be in range of
// the target. Integers can be rounded when cast to a float type, but a float
// has to be integral to be cast to an integer type. Out of line, since casts
// of constants are rare.
static NOINLINE void fold_cast(Parser* p, ExpDesc* e, TypeId t, SrcLoc at) {
  if (type_is_float(t)) {
    double d;
    if (type_is_integer(e->type)) {
      // Every ConstInt is in range of f32.
      d = t.u == TY_F32 ? (double)(float)e->cv.i : (double)e->cv.i;
    } else {
      float f = (float)e->cv.f;
      if (t.u == TY_F32 && (f > FLT_MAX || f < -FLT_MAX)) {
        error_at(p, at, "%s doesn't fit in f32.", float_str(p, e->cv.f));
      }
      d = t.u == TY_F32 ? (double)f : e->cv.f;
    }
    e->cv.f = d;
    return;
  }
  if (type_is_float(e->type)) {
    // Doubles outside of ConstInt's range are all integral, so they only need
    // the range check.
    double d = e->cv.f;
    bool in_range = d >= -0x1p127 && d < 0x1p127;
    if (in_range && (double)(ConstInt)d != d) {
      error_at(p, at, "%s isn't an integer.", float_str(p, d));
    }
    if (!in_range || !const_fits_in_type((ConstInt)d, t)) {
      error_at(p, at, "%s doesn't fit in %s.", float_str(p, d), type_str(p, t));
    }
    e->cv.i = (ConstInt)d;
    return;
  }
  if (!const_fits_in_type(e->cv.i, t)) {
    error_at(p, at, "%s doesn't fit in %s.", int_str(p, e->cv.i), type_str(p, t));
  }
}

// The value of a comparison of two numeric or bool constants. NaN can't be
// a constant, so the ordering is total.
static NOINLINE bool fold_compare(BinOpr op, const ExpDesc* a, const ExpDesc* b) {
  int c;
  if (a->type.u == TY_BOOL) {
    c = (int)a->cv.b - (int)b->cv.b;
  } else if (type_is_float(a->type) || type_is_float(b->type)) {
    // An integer here can only be an untyped constant, which is converted to
    // the float type, as in arithmetic.
    double x = type_is_float(a->type) ? a->cv.f : (double)a->cv.i;
    double y = type_is_float(b->type) ? b->cv.f : (double)b->cv.i;
    c = (x > y) - (x < y);
  } else {
    c = (a->cv.i > b->cv.i) - (a->cv.i < b->cv.i);
  }
  switch (op) {
    case OPR_EQ:
      return c == 0;
    case OPR_NE:
      return c != 0;
    case OPR_LT:
      return c < 0;
    case OPR_LE:
      return c <= 0;
    case OPR_GT:
      return c > 0;
    case OPR_GE:
      return c >= 0;
    default:
      ASSERT(false && "not a folded comparison");
      return false;
  }
}

// Emission

// bool and integers up to 32 bits are i32, and the other integers are i64.
static IlType il_type_of(Parser* p, TypeId t, SrcLoc at) {
  if (type_is_pointer(p, t)) {
    return IT_I64;
  }
  switch (t.u) {
    case TY_VOID:
      return IT_VOID;
    case TY_BOOL:
    case TY_I8:
    case TY_I16:
    case TY_I32:
    case TY_U8:
    case TY_U16:
    case TY_U32:
      return IT_I32;
    case TY_I64:
    case TY_U64:
      return IT_I64;
    case TY_F32:
      return IT_F32;
    case TY_F64:
      return IT_F64;
    default:
      error_at(p, at, "%s isn't supported yet.", type_str(p, t));
  }
}

// The IL op for an arithmetic operator, or OP_NUM_OPS if it isn't emitted yet.
static IlOp il_arith_op(BinOpr op) {
  switch (op) {
    case OPR_ADD:
      return OP_ADD;
    case OPR_SUB:
      return OP_SUB;
    case OPR_MUL:
      return OP_MUL;
    case OPR_BXOR:
      return OP_XOR;
    default:
      return OP_NUM_OPS;
  }
}

// The IL op for a comparison of integers, bools or pointers. Narrow integers
// are kept extended to i32, so the compare is the same for every width.
static IlOp il_compare_op(BinOpr op, bool is_unsigned) {
  switch (op) {
    case OPR_EQ:
      return OP_EQ;
    case OPR_NE:
      return OP_NE;
    case OPR_LT:
      return is_unsigned ? OP_ULT : OP_LT;
    case OPR_LE:
      return is_unsigned ? OP_ULE : OP_LE;
    case OPR_GT:
      return is_unsigned ? OP_UGT : OP_GT;
    case OPR_GE:
      return is_unsigned ? OP_UGE : OP_GE;
    default:
      ASSERT(false && "not a comparison");
      return OP_NUM_OPS;
  }
}

// A value that the emitting build doesn't emit yet. Its ref is 0, which
// isn't a valid Ref, so that emit_to_ref() gives an error if it's used,
// rather than using whatever e was before.
static void exp_unemitted(ExpDesc* e) {
  e->kind = EK_REF;
  if (EMIT) {
    e->ref = 0;
  }
}

static Ref emit_offset_addr(Parser* p, Ref base, u32 offset) {
  if (offset == 0) {
    return base;
  }
  return il_emit(&p->il, OP_ADD, IT_I64, base, il_const_int(&p->il, IT_I64, offset));
}

static Ref emit_addr(Parser* p, const ExpDesc* e) {
  return emit_offset_addr(p, e->base, e->offset);
}

// Emits whatever is needed to make e one value of type t, and returns its Ref.
// t has already been checked against e's type. If e is an untyped constant
// it will take on t's type.
static Ref emit_to_ref(Parser* p, const ExpDesc* e, TypeId t, SrcLoc at) {
  IlType ty = il_type_of(p, t, at);
  if (e->kind == EK_CONST && t.u == TY_BOOL) {
    return il_const_int(&p->il, ty, e->cv.b);
  }
  if (e->kind == EK_CONST && (type_is_integer(t) || type_is_pointer(p, t))) {
    return il_const_int(&p->il, ty, (u64)e->cv.i);
  }
  if (e->kind == EK_LOCAL) {
    Ref addr = il_slot_addr(&p->il, e->slot);
    return il_emit(&p->il, OP_LOAD, ty, addr, 0);
  }
  if (e->kind == EK_ADDR) {
    return il_emit(&p->il, OP_LOAD, il_mem_type_of(p, e->type), emit_addr(p, e), 0);
  }
  if (e->kind == EK_REF && e->ref != 0) {
    return e->ref;
  }
  error_at(p, at, "This expression isn't supported yet.");
}

// Returns the address of the aggregate that e is.
static Ref emit_agg_addr(Parser* p, const ExpDesc* e, SrcLoc at) {
  if (e->kind == EK_LOCAL) {
    return il_slot_addr(&p->il, e->slot);
  }
  if (e->kind == EK_ADDR) {
    return emit_addr(p, e);
  }
  if (e->kind == EK_CONST_DATA) {
    // A string literal is only data and a length until it's used as a str
    // value, and then it's built in a new temporary. It can't be a constant
    // image in the data, because the pointer to the bytes would need a
    // relocation.
    AggId agg = type_agg(p, ty_str);
    Ref temp = il_slot_addr(&p->il, il_slot_add_agg(&p->il, agg, (NameId){IL_SLOT_UNNAMED}));
    Ref len = il_const_int(&p->il, IT_I64, p->ls.ctx->data.items[e->data.u].len);
    il_emit(&p->il, OP_STORE, IT_I64, il_const_data(&p->il, e->data),
            emit_offset_addr(p, temp, p->str_data_offset));
    il_emit(&p->il, OP_STORE, IT_I64, len, emit_offset_addr(p, temp, p->str_len_offset));
    return temp;
  }
  error_at(p, at, "This expression isn't supported yet.");
}

static void emit_assign_to_addr(Parser* p, const ExpDesc* dst, const ExpDesc* e, SrcLoc at) {
  if (type_is_aggregate(p, dst->type)) {
    Ref src = emit_agg_addr(p, e, at);
    il_copy(&p->il, emit_addr(p, dst), src, type_agg(p, dst->type));
    return;
  }
  Ref v = emit_to_ref(p, e, dst->type, at);
  il_emit(&p->il, OP_STORE, il_mem_type_of(p, dst->type), v, emit_addr(p, dst));
}

static void emit_assign_to_local(Parser* p, const Local* l, const ExpDesc* e, SrcLoc at) {
  Ref addr = il_slot_addr(&p->il, l->slot);
  if (type_is_aggregate(p, l->type)) {
    il_copy(&p->il, addr, emit_agg_addr(p, e, at), type_agg(p, l->type));
    return;
  }
  Ref v = emit_to_ref(p, e, l->type, at);
  il_emit(&p->il, OP_STORE, il_type_of(p, l->type, at), v, addr);
}

// Gives the struct or array of type t at addr its initial value.
static void emit_agg_init(Parser* p, Ref addr, TypeId t) {
  AggId agg = type_agg(p, t);
  DataId image = type_info(p, t)->image;
  if (image.u == DATA_NONE) {
    il_emit(&p->il, OP_ZERO, IT_VOID, addr, agg.u);
  } else {
    il_copy(&p->il, addr, il_const_data(&p->il, image), agg);
  }
}

static SlotId emit_local_slot(Parser* p, TypeId t, NameId name, SrcLoc at) {
  if (type_is_aggregate(p, t) && type_has_layout(p, t)) {
    return il_slot_add_agg(&p->il, type_agg(p, t), name);
  }
  return il_slot_add(&p->il, il_type_of(p, t, at), name);
}

// Integers narrower than 64 bits are i32s. Extends v to i64 if it's narrower.
static Ref emit_extend_if_necessary(Parser* p, Ref v, TypeId t) {
  if (type_int_bits(t) == 64) {
    return v;
  }
  return il_emit(&p->il, type_is_unsigned(t) ? OP_ZEXT32 : OP_SEXT32, IT_I64, v, 0);
}

// Prints e without ending the line.
static void emit_print_value(Parser* p, const ExpDesc* e, SrcLoc at) {
  TypeId t = type_concrete(e->type);
  RuntimeSym sym;
  switch ((FixedType)t.u) {
    case TY_BOOL:
      sym = SYM_luv_print_bool;
      break;
    case TY_I8:
    case TY_I16:
    case TY_I32:
    case TY_I64:
      sym = SYM_luv_print_i64;
      break;
    case TY_U8:
    case TY_U16:
    case TY_U32:
    case TY_U64:
      sym = SYM_luv_print_u64;
      break;
    case TY_STR:
      sym = SYM_luv_print_str;
      break;
    default:
      error_at(p, at, "Printing %s isn't supported yet.", type_str(p, t));
  }
  Ref args[2];
  u32 nargs = 1;
  if (t.u == TY_STR && e->kind == EK_CONST_DATA) {
    // A literal is printed from its data, without building a str.
    args[0] = il_const_data(&p->il, e->data);
    args[1] = il_const_int(&p->il, IT_I64, p->ls.ctx->data.items[e->data.u].len);
    nargs = 2;
  } else if (t.u == TY_STR) {
    Ref addr = emit_agg_addr(p, e, at);
    args[0] = il_emit(&p->il, OP_LOAD, IT_I64, emit_offset_addr(p, addr, p->str_data_offset), 0);
    args[1] = il_emit(&p->il, OP_LOAD, IT_I64, emit_offset_addr(p, addr, p->str_len_offset), 0);
    nargs = 2;
  } else {
    args[0] = emit_to_ref(p, e, t, at);
  }
  if (type_is_integer(t)) {
    args[0] = emit_extend_if_necessary(p, args[0], t);
  }
  il_call(&p->il, IT_VOID, il_const_sym(&p->il, sym), args, nargs);
}

static void emit_print_end(Parser* p) {
  il_call(&p->il, IT_VOID, il_const_sym(&p->il, SYM_luv_print_end), NULL, 0);
}

// Prints the literal part of the current STRING_BEGIN, STRING_MID or
// STRING_END token, if it isn't empty.
static void emit_print_str_piece(Parser* p) {
  u32 len;
  const u8* s = lex_str(&p->ls, &p->ls.pos.t, &len);
  if (len == 0) {
    return;
  }
  Ref args[] = {il_const_data(&p->il, il_data_add(p->ls.ctx, s, len, 1)),
                il_const_int(&p->il, IT_I64, len)};
  il_call(&p->il, IT_VOID, il_const_sym(&p->il, SYM_luv_print_str), args, 2);
}

// Called after a binary operator and before its right operand, so that the
// left operand is evaluated first. Constants are kept so they can be folded.
static void emit_infix(Parser* p, ExpDesc* e, SrcLoc at) {
  if (e->kind == EK_LOCAL || e->kind == EK_ADDR) {
    e->ref = emit_to_ref(p, e, e->type, at);
    e->kind = EK_REF;
  }
}

// The value of an 'and' or 'or' is stored to a temporary slot on each path,
// and loaded after the paths join.
struct LogicalJoin {
  SlotId slot;
  BlockId end;
};

// Called after the left operand of 'and' or 'or'. The right operand is only
// evaluated when the left one hasn't determined the final result.
static void emit_logical_begin(Parser* p,
                               BinOpr op,
                               const ExpDesc* e,
                               SrcLoc at,
                               LogicalJoin* join) {
  Ref left = emit_to_ref(p, e, ty_bool, at);
  join->slot = il_slot_add(&p->il, IT_I32, (NameId){IL_SLOT_UNNAMED});
  join->end = il_block_declare(&p->il);
  il_emit(&p->il, OP_STORE, IT_I32, left, il_slot_addr(&p->il, join->slot));
  if (op == OPR_AND) {
    il_branch(&p->il, left, join->end);
  } else {
    BlockId right = il_block_declare(&p->il);
    il_branch(&p->il, left, right);
    il_jump(&p->il, join->end);
    il_block_start(&p->il, right);
  }
}

static void emit_logical_end(Parser* p, const ExpDesc* rhs, SrcLoc at, const LogicalJoin* join) {
  Ref right = emit_to_ref(p, rhs, ty_bool, at);
  il_emit(&p->il, OP_STORE, IT_I32, right, il_slot_addr(&p->il, join->slot));
  il_block_start(&p->il, join->end);
}

// Unary '-' or '~', which give the operand's type. Constants are folded.
static void unary_arith(Parser* p, bool neg, ExpDesc* e, SrcLoc at) {
  TypeId t = e->type;
  if (t.u == TY_UNKNOWN) {
    exp_unemitted(e);
    return;
  }
  if (neg && !type_is_integer(t) && !type_is_float(t)) {
    error_at(p, at, "'-' expects a number, not %s.", type_str(p, t));
  }
  if (!neg && !type_is_integer(t)) {
    error_at(p, at, "'~' expects an integer, not %s.", type_str(p, t));
  }
  if (e->kind != EK_CONST) {
    exp_unemitted(e);
    return;
  }
  if (type_is_float(t)) {
    e->cv.f = -e->cv.f;
    return;
  }
  ConstInt r;
  if (!neg) {
    // Complementing a two's complement value of unlimited width gives
    // -x - 1, which is also right for signed types of any width.
    r = type_is_unsigned(t) ? (((ConstInt)1 << type_int_bits(t)) - 1) ^ e->cv.i : ~e->cv.i;
  } else if (__builtin_sub_overflow((ConstInt)0, e->cv.i, &r)) {
    error_at(p, at, "Constant overflows.");
  }
  if (!type_is_untyped(t) && !const_fits_in_type(r, t)) {
    error_at(p, at, "%s doesn't fit in %s.", int_str(p, r), type_str(p, t));
  }
  e->cv.i = r;
}

// subexpr -> (primary | unop subexpr) { binop subexpr }
// where binop is any binary operator with a priority higher than `limit`.
static void subexpr(Parser* p, int limit, ExpDesc* e) {
  SrcLoc start = p->ls.pos.t.loc;
  switch (T(p)) {
    case TK_NOT: {
      if (limit >= PRIORITY_NOT) {
        error_at(p, p->ls.pos.t.loc, "'not' needs parentheses here.");
      }
      next(p);
      SrcLoc at = p->ls.pos.t.loc;
      subexpr(p, PRIORITY_NOT, e);
      expect_bool(p, e, at, "'not'");
      e->shape = SH_VALUE;
      if (e->kind == EK_CONST && e->type.u == TY_BOOL) {
        e->cv.b = !e->cv.b;
      } else if (EMIT && e->type.u == TY_BOOL) {
        // A bool is 0 or 1.
        Ref v = emit_to_ref(p, e, ty_bool, at);
        e->ref = il_emit(&p->il, OP_XOR, IT_I32, v, il_const_int(&p->il, IT_I32, 1));
        e->kind = EK_REF;
      } else {
        exp_unemitted(e);
      }
      e->type = ty_bool;
      break;
    }
    case TK_MINUS:
    case TK_TILDE: {
      bool neg = T(p) == TK_MINUS;
      SrcLoc at = p->ls.pos.t.loc;
      next(p);
      subexpr(p, PRIORITY_UNARY, e);
      unary_arith(p, neg, e, at);
      e->shape = SH_VALUE;
      break;
    }
    case TK_AMP: {
      next(p);
      SrcLoc at = p->ls.pos.t.loc;
      // Only valid if the operand turns out to be a bare name.
      NameId name = p->ls.pos.t.v.name;
      subexpr(p, PRIORITY_UNARY, e);
      const Local* l = e->shape == SH_NAME ? lookup(p, name) : NULL;
      if ((e->shape != SH_NAME && e->shape != SH_LVALUE && e->shape != SH_STRUCT_LITERAL) ||
          (l && l->kind == LK_PARAM)) {
        error_at(p, at, "Can't take the address of non-lvalue.");
      }
      if (e->kind == EK_CONST) {
        error_at(p, at, "Can't take the address of a constant.");
      }
      if (EMIT && e->shape != SH_STRUCT_LITERAL && e->kind == EK_LOCAL) {
        e->ref = il_slot_addr(&p->il, e->slot);
        e->kind = EK_REF;
      } else if (EMIT && e->shape != SH_STRUCT_LITERAL && e->kind == EK_ADDR) {
        e->ref = emit_addr(p, e);
        e->kind = EK_REF;
      } else {
        exp_unemitted(e);
      }
      e->shape = SH_VALUE;
      e->type = type_pointer_to(p, e->type);
      break;
    }
    default:
      primary(p, e);
      break;
  }
  binop_rest(p, limit, e, start);
}

// The { binop subexpr } loop of subexpr(). It's separate so that
// sizeof_operand() can continue an expression after already parsing a
// primary. `start` is the location of the left operand.
static void binop_rest(Parser* p, int limit, ExpDesc* e, SrcLoc start) {
  BinOpr op = tok_to_binopr(T(p));
  while (op != OPR_NONE && priority[op] > limit) {
    SrcLoc at = p->ls.pos.t.loc;
    next(p);
    if (EMIT) {
      emit_infix(p, e, start);
    }
    if (op == OPR_NOTIN) {
      check(p, TK_IN);
    }
    const char* logical = op == OPR_AND ? "'and'" : op == OPR_OR ? "'or'" : NULL;
    LogicalJoin join = {0};
    if (logical) {
      expect_bool(p, e, at, logical);
      if (EMIT) {
        emit_logical_begin(p, op, e, start, &join);
      }
    }
    SrcLoc rhs_at = p->ls.pos.t.loc;
    ExpDesc rhs;
    subexpr(p, priority[op], &rhs);
    if (logical) {
      expect_bool(p, &rhs, rhs_at, logical);
      if (EMIT) {
        emit_logical_end(p, &rhs, rhs_at, &join);
      }
    }
    BinOpr nextop = tok_to_binopr(T(p));
    if (binopr_is_comparison(op) && binopr_is_comparison(nextop)) {
      error_at(p, at, "Comparisons can't be chained. Use 'and' to combine them.");
    }
    if (op >= OPR_EQ && op <= OPR_GE && !assignable(p, rhs.type, e->type) &&
        !assignable(p, e->type, rhs.type)) {
      error_at(p, at, "Can't compare %s and %s.", type_str(p, e->type), type_str(p, rhs.type));
    }
    e->shape = SH_VALUE;
    bool is_compare = op >= OPR_EQ && op <= OPR_GE;
    bool is_bool = binopr_is_comparison(op) || logical;
    bool is_arith = op >= OPR_BOR && op <= OPR_MOD;
    if (is_compare) {
      TypeId t = type_is_untyped(e->type) ? rhs.type : e->type;
      bool folded = false;
      if (e->kind == EK_CONST || rhs.kind == EK_CONST) {
        check_const_operands(p, op, t, e, start, &rhs, rhs_at, at);
        bool foldable = t.u == TY_BOOL || type_is_integer(t) || type_is_float(t);
        if (e->kind == EK_CONST && rhs.kind == EK_CONST && foldable) {
          e->cv.b = fold_compare(op, e, &rhs);
          folded = true;
        }
      }
      if (t.u == TY_NULL) {
        t = rhs.type;
      }
      bool is_pointer = type_is_pointer(p, t);
      if (!folded && EMIT && (t.u == TY_BOOL || type_is_integer(t) || is_pointer)) {
        Ref left = emit_to_ref(p, e, t, start);
        Ref right = emit_to_ref(p, &rhs, t, rhs_at);
        IlOp cmp = il_compare_op(op, type_is_unsigned(t) || is_pointer);
        e->kind = EK_REF;
        e->ref = il_emit(&p->il, cmp, il_type_of(p, t, at), left, right);
      } else if (!folded) {
        exp_unemitted(e);
      }
      e->type = ty_bool;
    } else if (is_bool) {
      // Only 'and' and 'or' of two constants are constant, even when one
      // operand is enough to decide the value.
      if (logical && e->kind == EK_CONST && rhs.kind == EK_CONST && e->type.u == TY_BOOL &&
          rhs.type.u == TY_BOOL) {
        e->cv.b = op == OPR_AND ? e->cv.b && rhs.cv.b : e->cv.b || rhs.cv.b;
      } else if (EMIT && logical) {
        e->kind = EK_LOCAL;
        e->slot = join.slot;
      } else {
        exp_unemitted(e);
      }
      e->type = ty_bool;
    } else if (is_arith) {
      TypeId t = type_for_arith(p, op, e->type, rhs.type, at);
      if (e->kind == EK_CONST || rhs.kind == EK_CONST) {
        check_const_operands(p, op, t, e, start, &rhs, rhs_at, at);
      }
      if (e->kind == EK_CONST && rhs.kind == EK_CONST && t.u != TY_UNKNOWN && t.u != TY_STR) {
        fold_arith(p, op, t, e, &rhs, at);
      } else if (EMIT && il_arith_op(op) != OP_NUM_OPS && (t.u == TY_I32 || t.u == TY_I64)) {
        Ref left = emit_to_ref(p, e, t, start);
        Ref right = emit_to_ref(p, &rhs, t, rhs_at);
        e->kind = EK_REF;
        e->ref = il_emit(&p->il, il_arith_op(op), il_type_of(p, t, at), left, right);
      } else {
        exp_unemitted(e);
      }
      e->type = t;
    } else {
      e->type = ty_unknown;
      exp_unemitted(e);
    }
    op = nextop;
  }
}

static void expr(Parser* p) {
  ExpDesc e;
  subexpr(p, -1, &e);
}

// A list or array literal gets its type from what it's assigned to, which is
// known before it's parsed. Only a literal at the start of the expression gets
// `t`, since that's decided by the first token.
static void expr_expect(Parser* p, TypeId t, ExpDesc* e) {
  if (T(p) == TK_LSQUARE) {
    p->literal_expected = t;
  }
  subexpr(p, -1, e);
}

// Returns the condition's value when emitting, and otherwise 0.
static Ref cond_value(Parser* p, ExpDesc* e, SrcLoc at) {
  expect_bool(p, e, at, "Condition");
  return EMIT ? emit_to_ref(p, e, ty_bool, at) : 0;
}

static Ref cond(Parser* p) {
  SrcLoc at = p->ls.pos.t.loc;
  ExpDesc e;
  subexpr(p, -1, &e);
  return cond_value(p, &e, at);
}

// "...\(e)...\(e)..."
static void interpolated_string(Parser* p) {
  next(p);  // STRING_BEGIN
  for (;;) {
    expr(p);
    if (T(p) == TK_STRING_MID) {
      next(p);
    } else if (T(p) == TK_STRING_END) {
      next(p);
      return;
    } else {
      error_expected(p, "')' to end the string interpolation");
    }
  }
}

// 'print' of an interpolated string, which prints each piece in turn rather
// than building the string, so that nothing has to be allocated. The pieces
// are printed as they're parsed, so the string has to be the whole
// expression.
static void print_interpolated(Parser* p) {
  emit_print_str_piece(p);
  next(p);  // STRING_BEGIN
  for (;;) {
    SrcLoc at = p->ls.pos.t.loc;
    ExpDesc e;
    subexpr(p, -1, &e);
    check_not_void(p, &e, at);
    emit_print_value(p, &e, at);
    if (T(p) != TK_STRING_MID && T(p) != TK_STRING_END) {
      error_expected(p, "')' to end the string interpolation");
    }
    emit_print_str_piece(p);
    bool is_end = T(p) == TK_STRING_END;
    next(p);
    if (is_end) {
      break;
    }
  }
  if (T(p) != TK_NEWLINE && !block_follow(T(p))) {
    error_at(p, p->ls.pos.t.loc, "This expression isn't supported yet.");
  }
  emit_print_end(p);
}

// Globals
//
// Locals are looked up first, so they shadow globals.

// Reports an error at `at` if name is already a global.
static void check_not_global(Parser* p, NameId name, SrcLoc at) {
  if (global_kind(p, name) != GK_NONE) {
    const Name* n = name_of(p, name);
    error_at(p, at,
             n->is_const ? "Cannot assign a new value to a constant."
                         : "Cannot re-initialize an existing global.");
  }
}

// Declares a function or type, which can only be defined once.
//
// TODO: This is measurable on dumbbench, which has 1.25M top-level functions.
// Part of it is code layout in funcsig() (moving the error report out of line
// recovers most of that), and part is page faults from growing
// Module.globals.
static void declare_definition(Parser* p, NameId name, SrcLoc at, GlobalInfo info) {
  if (global_kind(p, name) != GK_NONE) {
    const Name* n = name_of(p, name);
    error_at(p, at, "Duplicate top-level definition of '%.*s'.", (int)n->len, (const char*)n->str);
  }
  declare_global(p, name, info);
}

#define MAX_TARGETS 8

// Loop variables, which are bound after the expression that's iterated.
struct Targets {
  u32 num_targets;
  NameId name[MAX_TARGETS];
  SrcLoc at[MAX_TARGETS];
};

// A loop whose body is being parsed.
struct Loop {
  BlockId head;  // where continue goes
  BlockId exit;  // where break goes
  bool has_break;
};

// for_targets -> name {',' name}
// The first name has already been parsed.
static void for_targets(Parser* p, Targets* t, NameId first, SrcLoc first_at) {
  t->num_targets = 0;
  NameId name = first;
  SrcLoc at = first_at;
  for (;;) {
    if (t->num_targets == MAX_TARGETS) {
      error_at(p, at, "Too many loop variables.");
    }
    if (name_of(p, name)->is_const) {
      error_at(p, at, "A constant can't be a loop variable.");
    }
    t->name[t->num_targets] = name;
    t->at[t->num_targets] = at;
    ++t->num_targets;
    if (!optional(p, TK_COMMA)) {
      break;
    }
    name = expect_name(p, "loop variable name", &at);
  }
}

// Lexes past the first element of a list literal, without parsing it. Stops at
// a 'for' or ',' in the list itself, or at its ']'. Returns whether it stopped
// at a 'for'.
static bool skim_to_for(Parser* p, u32 depth) {
  for (;;) {
    TokenKind k = T(p);
    if (p->ls.pos.bracket_depth < depth || k == TK_EOS) {
      return false;
    }
    if (p->ls.pos.bracket_depth == depth && (k == TK_FOR || k == TK_COMMA)) {
      return k == TK_FOR;
    }
    next(p);
  }
}

// comprehension -> 'for' for_targets 'in' expr { 'for' ... | 'if' expr }
//
// The element comes before the 'for' clauses that bind its names. The clauses
// are parsed first, and then the lexer is rewound to parse the element with
// the loop variables bound. Expressions can't declare new names, so there's
// no parser state to be saved.
static void comprehension(Parser* p, const LexPos* elem, SrcLoc at) {
#if BUILD_DEBUG
  LexPos at_for;
  lex_mark(&p->ls, &at_for);
#endif
  u32 base = p->num_locals;
  while (T(p) == TK_FOR || T(p) == TK_IF) {
    if (optional(p, TK_FOR)) {
      Targets t;
      SrcLoc first_at;
      NameId first = expect_name(p, "loop variable name", &first_at);
      for_targets(p, &t, first, first_at);
      check(p, TK_IN);
      expr(p);
      // Loop variables are always new, so they shadow any outer name.
      for (u32 i = 0; i < t.num_targets; ++i) {
        declare_local(p, t.name[i], t.at[i], LK_VAR);
      }
    } else {
      next(p);
      expr(p);
    }
  }
  LexPos end;
  lex_mark(&p->ls, &end);
  lex_rewind(&p->ls, elem);
  expr(p);
  if (T(p) != TK_FOR) {
    check_match(p, TK_RSQUARE, TK_LSQUARE, at);
  }
  // Both lexed the same text from elem, so a difference means some lexer state
  // isn't restored by lex_rewind().
  ASSERT(lex_same_pos_for_testing(&p->ls, &at_for, &p->ls.pos));
  lex_rewind(&p->ls, &end);
  pop_locals(p, base);
  check_match(p, TK_RSQUARE, TK_LSQUARE, at);
}

// An element of a list or array literal, checked against `elem` if it's known.
// When emitting an array, it's stored at temp + offset.
static void list_element(Parser* p, TypeId elem, Ref temp, u32 offset) {
  SrcLoc at = p->ls.pos.t.loc;
  ExpDesc v;
  expr_expect(p, elem, &v);
  if (elem.u != TY_UNKNOWN) {
    check_assign(p, elem, &v, at);
  }
  if (p->const_context && !exp_is_const(&v)) {
    error_at(p, at, "Expected a constant expression.");
  }
  if (EMIT && temp) {
    ExpDesc dst = {.type = elem, .kind = EK_ADDR, .base = temp, .offset = offset};
    emit_assign_to_addr(p, &dst, &v, at);
  }
}

// '[' ']' | '[' expr comprehension ']' | '[' expr {',' expr} [','] ']'
//
// The literal is an array if Parser.literal_expected is an array type, and
// otherwise its type isn't known yet. It's cleared so that it doesn't apply
// to later literals.
static void list_literal(Parser* p, ExpDesc* e) {
  TypeId expected = p->literal_expected;
  p->literal_expected = ty_unknown;
  SrcLoc at = p->ls.pos.t.loc;
  u32 depth = p->ls.pos.bracket_depth;
  e->shape = SH_VALUE;
  e->type = ty_unknown;
  exp_unemitted(e);
  next(p);
  bool is_array = type_is_array(p, expected);
  TypeId elem_type = is_array ? type_info(p, expected)->elem : ty_unknown;
  if (p->const_context && !is_array) {
    error_at(p, at, "A list can't be a constant yet.");
  }
  u32 count = 0;
  if (T(p) != TK_RSQUARE) {
    LexPos elem;
    lex_mark(&p->ls, &elem);
    if (skim_to_for(p, depth)) {
      comprehension(p, &elem, at);
      return;
    }
    lex_rewind(&p->ls, &elem);
  }
  // Every element is stored, so the temporary doesn't need initializing.
  Ref temp = 0;
  u32 elem_size = 0;
  if (EMIT && is_array && p->const_context) {
    error_at(p, at, "This array literal isn't supported yet.");
  }
  if (EMIT && is_array) {
    if (!type_has_layout(p, expected)) {
      error_at(p, at, "This array literal isn't supported yet.");
    }
    temp = il_slot_addr(&p->il,
                        il_slot_add_agg(&p->il, type_agg(p, expected), (NameId){IL_SLOT_UNNAMED}));
    elem_size = type_size(p, elem_type);
  }
  if (T(p) != TK_RSQUARE) {
    list_element(p, elem_type, temp, 0);
    count = 1;
    while (optional(p, TK_COMMA)) {
      if (T(p) == TK_RSQUARE) {
        break;  // trailing comma
      }
      list_element(p, elem_type, temp, count * elem_size);
      ++count;
    }
  }
  check_match(p, TK_RSQUARE, TK_LSQUARE, at);
  if (is_array) {
    u32 want = type_info(p, expected)->count;
    if (count != want) {
      error_at(p, at, "Expected %u element%s, not %u.", want, want == 1 ? "" : "s", count);
    }
    e->type = expected;
  }
  if (is_array && p->const_context) {
    e->kind = EK_CONST_DATA;
  } else if (EMIT && is_array) {
    e->kind = EK_ADDR;
    e->base = temp;
    e->offset = 0;
  }
}

// '{' '}' | '{' expr ':' expr {',' expr ':' expr} [','] '}'
static void dict_literal(Parser* p) {
  SrcLoc at = p->ls.pos.t.loc;
  next(p);
  if (T(p) != TK_RBRACE) {
    expr(p);
    check(p, TK_COLON);
    expr(p);
    if (T(p) == TK_FOR) {
      error_at(p, p->ls.pos.t.loc, "Dict comprehensions aren't supported yet.");
    }
    while (optional(p, TK_COMMA)) {
      if (T(p) == TK_RBRACE) {
        break;  // trailing comma
      }
      expr(p);
      check(p, TK_COLON);
      expr(p);
    }
  }
  check_match(p, TK_RBRACE, TK_LBRACE, at);
}

// The operand of sizeof or alignof is either a type or an expression.
// Builtin types, '*', 'typeof', '[' and '{' always start a type, so
// sizeof([2]int) is a type, and an expression needs extra parentheses, as in
// sizeof(([1, 2])). A name or TypeName is parsed as a postfix chain, which is
// a type if it's TypeName or module.TypeName, and is otherwise continued as an
// expression. Returns the type, or the expression's type.
static TypeId sizeof_operand(Parser* p) {
  TokenKind k = T(p);
  if (is_builtin_type(k) || k == TK_STAR || k == TK_LSQUARE || k == TK_LBRACE || k == TK_TYPEOF) {
    return type(p);
  }
  ExpDesc e;
  if (k == TK_NAME || k == TK_TYPENAME || k == TK_TYPEPARAM) {
    SrcLoc start = p->ls.pos.t.loc;
    primary(p, &e);
    if (e.shape == SH_TYPE) {
      return e.type;
    }
    binop_rest(p, -1, &e, start);
  } else {
    subexpr(p, -1, &e);
  }
  return e.type;
}

static const AggInfo* struct_agg(Parser* p, TypeId st) {
  return il_agg(p->ls.ctx, type_info(p, st)->agg);
}

// Whether a type's size and alignment are known. They're not for a struct
// until the end of its definition, or for a struct with a field whose layout
// isn't known, as with generic struct fields. An array has a layout if its
// element type does.
static bool type_has_layout(Parser* p, TypeId t) {
  if (type_is_struct(p, t)) {
    return type_info(p, t)->agg.u != AGG_NONE;
  }
  if (type_is_array(p, t)) {
    return type_has_layout(p, type_info(p, t)->elem);
  }
  if (type_is_integer(t)) {
    return !type_is_untyped(t);
  }
  return type_is_pointer(p, t) || t.u == TY_BOOL || t.u == TY_F32 || t.u == TY_F64;
}

static u32 type_size(Parser* p, TypeId t) {
  ASSERT(type_has_layout(p, t));
  if (type_is_integer(t)) {
    return (u32)type_int_bits(t) / 8;
  }
  if (type_is_pointer(p, t)) {
    return 8;
  }
  if (type_is_struct(p, t)) {
    return struct_agg(p, t)->size;
  }
  if (type_is_array(p, t)) {
    return type_info(p, t)->count * type_size(p, type_info(p, t)->elem);
  }
  switch ((FixedType)t.u) {
    case TY_BOOL:
      return 1;
    case TY_F32:
      return 4;
    case TY_F64:
      return 8;
    default:
      ASSERT(false && "type without a size");
      return 0;
  }
}

// Scalars are aligned to their size, and arrays to their element type.
static u32 type_align(Parser* p, TypeId t) {
  ASSERT(type_has_layout(p, t));
  if (type_is_struct(p, t)) {
    return 1u << struct_agg(p, t)->align_log2;
  }
  if (type_is_array(p, t)) {
    return type_align(p, type_info(p, t)->elem);
  }
  return type_size(p, t);
}

// The type that a scalar is loaded and stored as.
static IlType il_mem_type_of(Parser* p, TypeId t) {
  if (t.u == TY_F32) {
    return IT_F32;
  }
  if (t.u == TY_F64) {
    return IT_F64;
  }
  bool is_unsigned = t.u == TY_BOOL || type_is_unsigned(t);
  switch (type_size(p, t)) {
    case 1:
      return is_unsigned ? IT_U8 : IT_I8;
    case 2:
      return is_unsigned ? IT_U16 : IT_I16;
    case 4:
      return IT_I32;
    default:
      ASSERT(type_size(p, t) == 8);
      return IT_I64;
  }
}

// Appends the scalars of t, which is at offset, with structs and arrays
// flattened. t's size can be at most AGG_MAX_CLASSIFIED, so that leaves has
// room even if every scalar is a byte.
static void type_agg_leaves(Parser* p, TypeId t, u32 offset, AggLeaf* leaves, u32* num_leaves) {
  if (type_is_struct(p, t)) {
    const TypeInfo* info = type_info(p, t);
    for (u32 i = 0; i < info->num_fields; ++i) {
      const Field* f = &p->fields.items[info->first_field + i];
      type_agg_leaves(p, f->type, offset + f->offset, leaves, num_leaves);
    }
  } else if (type_is_array(p, t)) {
    TypeId elem = type_info(p, t)->elem;
    u32 elem_size = type_size(p, elem);
    for (u32 i = 0; i < type_info(p, t)->count; ++i) {
      type_agg_leaves(p, elem, offset + i * elem_size, leaves, num_leaves);
    }
  } else {
    leaves[(*num_leaves)++] = (AggLeaf){offset, il_mem_type_of(p, t)};
  }
}

static const Field* struct_field(Parser* p, TypeId st, NameId name) {
  const TypeInfo* info = type_info(p, st);
  for (u32 i = 0; i < info->num_fields; ++i) {
    const Field* f = &p->fields.items[info->first_field + i];
    if (f->name.u == name.u) {
      return f;
    }
  }
  return NULL;
}

// Returns the offset of a field of the prelude's str, which has to be 8 bytes.
static u32 str_field_offset(Parser* p, const char* name) {
  const Field* f = struct_field(p, ty_str, lex_intern_safe(p->ls.ctx, name));
  ASSERT(f && type_size(p, f->type) == 8);
  return f->offset;
}

static NOINLINE NORETURN void field_error(Parser* p, TypeId st, NameId name, SrcLoc at) {
  const Name* n = name_of(p, name);
  error_at(p, at, "'%.*s' isn't a field of %s.", (int)n->len, (const char*)n->str, type_str(p, st));
}

// Arguments are checked against callee if it's a function type. When EMIT,
// the arguments are put in args, and the return value is how many there are.
static u32 call_args(Parser* p, TypeId callee, Ref* args) {
  SrcLoc at = p->ls.pos.t.loc;
  bool is_function = type_is_function(p, callee);
  u32 num_params = is_function ? type_info(p, callee)->num_params : 0;
  u32 first_param = is_function ? type_info(p, callee)->first_param : 0;
  u32 num_args = 0;
  SrcLoc extra_at = {0};  // the first argument past num_params
  next(p);                // '('
  if (T(p) != TK_RPAREN) {
    for (;;) {
      if (num_args++ == num_params) {
        extra_at = p->ls.pos.t.loc;
      }
      if (T(p) == TK_RPAREN) {
        error_at(p, p->ls.pos.t.loc, "Trailing commas aren't allowed in calls.");
      }
      bool has_param = num_args <= num_params;
      TypeId t = has_param ? p->param_types.items[first_param + num_args - 1] : ty_unknown;
      SrcLoc arg_at = p->ls.pos.t.loc;
      ExpDesc e;
      expr_expect(p, t, &e);
      check_not_void(p, &e, arg_at);
      if (has_param) {
        if (!assignable(p, e.type, t)) {
          error_at(p, arg_at, "Argument %u is %s, but the parameter is %s.", num_args,
                   type_str(p, e.type), type_str(p, t));
        }
        check_const_conversion(p, t, &e, arg_at);
        if (EMIT) {
          if (num_args > MAX_CALL_ARGS) {
            error_at(p, arg_at, "Calls can't have more than %d arguments.", MAX_CALL_ARGS);
          }
          if (type_is_aggregate(p, t)) {
            Ref addr = emit_agg_addr(p, &e, arg_at);
            args[num_args - 1] = il_emit(&p->il, OP_ARGAGG, IT_I64, addr, type_agg(p, t).u);
          } else {
            args[num_args - 1] = emit_to_ref(p, &e, t, arg_at);
          }
        }
      }
      if (!optional(p, TK_COMMA)) {
        break;
      }
    }
  }
  SrcLoc close_at = p->ls.pos.t.loc;
  check_match(p, TK_RPAREN, TK_LPAREN, at);
  if (is_function && num_args != num_params) {
    error_at(p, num_args > num_params ? extra_at : close_at, "Expected %u argument%s, not %u.",
             num_params, num_params == 1 ? "" : "s", num_args);
  }
  return num_args;
}

// struct_args -> '(' [struct_arg {',' struct_arg}] ')'
// struct_arg  -> ['.' name '='] expr
//
// e is the struct type, and becomes the literal. The arguments are checked
// against the struct's fields.
// They're given in order, a designator moves to its field, and later
// arguments continue from there. Fields that aren't given get their defaults.
// Literals of generic structs aren't checked yet.
static void struct_args(Parser* p, ExpDesc* e) {
  SrcLoc at = p->ls.pos.t.loc;
  TypeId st = e->type;
  bool has_fields = type_is_struct(p, st);
  // The literal is built in a temporary, because most uses of it, such as an
  // argument or a field of another literal, have nowhere else to build it.
  Ref temp = 0;
  if (EMIT && p->const_context) {
    error_at(p, at, "This struct literal isn't supported yet.");
  }
  if (EMIT) {
    if (!has_fields || !type_has_layout(p, st)) {
      error_at(p, at, "This struct literal isn't supported yet.");
    }
    AggId agg = type_info(p, st)->agg;
    temp = il_slot_addr(&p->il, il_slot_add_agg(&p->il, agg, (NameId){IL_SLOT_UNNAMED}));
    emit_agg_init(p, temp, st);
  }
  u32 num_fields = has_fields ? type_info(p, st)->num_fields : 0;
  u32 first_field = has_fields ? type_info(p, st)->first_field : 0;
  u32 field = 0;  // the field that the next positional argument gives
  // Which fields have been given. Nested literals are recursive calls, so
  // each has its own.
  bool* given = (bool*)ALLOCA(num_fields * sizeof(bool));
  memset(given, 0, num_fields * sizeof(bool));
  u32 num_args = 0;
  next(p);  // '('
  if (T(p) != TK_RPAREN) {
    for (;;) {
      ++num_args;
      if (T(p) == TK_RPAREN) {
        error_at(p, p->ls.pos.t.loc, "Trailing commas aren't allowed in calls.");
      }
      SrcLoc field_at = p->ls.pos.t.loc;
      if (optional(p, TK_DOT)) {
        NameId name = p->ls.pos.t.v.name;
        SrcLoc name_at = p->ls.pos.t.loc;
        check_name(p, "field name");
        if (has_fields) {
          const Field* f = struct_field(p, st, name);
          if (!f) {
            field_error(p, st, name, name_at);
          }
          field = (u32)(f - &p->fields.items[first_field]);
        }
        check(p, TK_EQ);
      }
      // Parsing the argument can't add fields, so f stays valid.
      const Field* f = NULL;
      if (has_fields) {
        if (field == num_fields) {
          error_at(p, field_at, "Too many arguments for %s, which has %u field%s.", type_str(p, st),
                   num_fields, num_fields == 1 ? "" : "s");
        }
        f = &p->fields.items[first_field + field];
        if (given[field]) {
          const Name* n = name_of(p, f->name);
          error_at(p, field_at, "Field '%.*s' is given twice.", (int)n->len, (const char*)n->str);
        }
        given[field] = true;
        ++field;
      }
      SrcLoc arg_at = p->ls.pos.t.loc;
      ExpDesc arg;
      expr_expect(p, f ? f->type : ty_unknown, &arg);
      check_not_void(p, &arg, arg_at);
      if (p->const_context && !exp_is_const(&arg)) {
        error_at(p, arg_at, "Expected a constant expression.");
      }
      if (f && !assignable(p, arg.type, f->type)) {
        const Name* n = name_of(p, f->name);
        error_at(p, arg_at, "Argument %u is %s, but field '%.*s' is %s.", num_args,
                 type_str(p, arg.type), (int)n->len, (const char*)n->str, type_str(p, f->type));
      }
      if (f) {
        check_const_conversion(p, f->type, &arg, arg_at);
      }
      if (EMIT) {
        ExpDesc dst = {.type = f->type, .kind = EK_ADDR, .base = temp, .offset = f->offset};
        emit_assign_to_addr(p, &dst, &arg, arg_at);
      }
      if (!optional(p, TK_COMMA)) {
        break;
      }
    }
  }
  check_match(p, TK_RPAREN, TK_LPAREN, at);
  e->shape = SH_STRUCT_LITERAL;
  if (p->const_context) {
    e->kind = EK_CONST_DATA;
  } else if (EMIT) {
    e->kind = EK_ADDR;
    e->base = temp;
    e->offset = 0;
  } else {
    exp_unemitted(e);
  }
}

// Makes elem the element at index idx of the pointer or array at base +
// offset. Its type has already been changed to the element's. A constant
// index that fits is kept in the offset, so that a field of the element adds
// to it.
static void emit_index(Parser* p,
                       ExpDesc* elem,
                       Ref base,
                       u32 offset,
                       const ExpDesc* idx,
                       SrcLoc at) {
  if (!type_has_layout(p, elem->type)) {
    error_at(p, at, "This expression isn't supported yet.");
  }
  u32 size = type_size(p, elem->type);
  elem->kind = EK_ADDR;
  elem->base = base;
  elem->offset = offset;
  if (idx->kind == EK_CONST) {
    ConstInt total = offset + idx->cv.i * size;
    if (total >= 0 && total <= UINT32_MAX) {
      elem->offset = (u32)total;
    } else {
      Ref delta = il_const_int(&p->il, IT_I64, (u64)total);
      elem->base = il_emit(&p->il, OP_ADD, IT_I64, base, delta);
      elem->offset = 0;
    }
    return;
  }
  Ref delta = emit_extend_if_necessary(p, emit_to_ref(p, idx, idx->type, at), idx->type);
  if (size != 1) {
    delta = il_emit(&p->il, OP_MUL, IT_I64, delta, il_const_int(&p->il, IT_I64, size));
  }
  elem->base = il_emit(&p->il, OP_ADD, IT_I64, base, delta);
}

// '[' expr ']' (index) or '[' [expr] ':' [expr] ']' (slice). e is the
// expression being indexed, and is replaced by the result. Only indexing a
// pointer or a fixed-size array is typed so far. A constant index into an
// array is checked against its size.
static void index_or_slice(Parser* p, ExpDesc* e) {
  SrcLoc at = p->ls.pos.t.loc;
  bool is_pointer = type_is_pointer(p, e->type);
  bool is_array = type_is_array(p, e->type);
  // The pointer is loaded, or the array's address found, before the index is
  // evaluated, like the left operand of a binary operator.
  Ref base = 0;
  u32 offset = 0;
  bool emits = EMIT && (is_pointer || (is_array && (e->kind == EK_LOCAL || e->kind == EK_ADDR)));
  if (EMIT && is_pointer) {
    base = emit_to_ref(p, e, e->type, at);
  } else if (EMIT && is_array && e->kind == EK_LOCAL) {
    base = il_slot_addr(&p->il, e->slot);
  } else if (EMIT && is_array && e->kind == EK_ADDR) {
    base = e->base;
    offset = e->offset;
  }
  next(p);  // '['
  // The index, or the slice's start, which can be left out.
  ExpDesc first;
  SrcLoc first_at = p->ls.pos.t.loc;
  if (T(p) != TK_COLON) {
    subexpr(p, -1, &first);
  }
  bool is_slice = optional(p, TK_COLON);
  if (is_slice && T(p) != TK_RSQUARE) {
    expr(p);
  }
  check_match(p, TK_RSQUARE, TK_LSQUARE, at);
  TypeId result = ty_unknown;
  if (!is_slice && (is_pointer || is_array)) {
    if (first.type.u != TY_UNKNOWN && !type_is_integer(first.type)) {
      error_at(p, first_at, "Index must be an integer, not %s.", type_str(p, first.type));
    }
    check_const_conversion(p, ty_i64, &first, first_at);
    const TypeInfo* info = type_info(p, e->type);
    if (is_array && first.kind == EK_CONST && (first.cv.i < 0 || first.cv.i >= info->count)) {
      error_at(p, first_at, "Index %s is out of range for %s.", int_str(p, first.cv.i),
               type_str(p, e->type));
    }
    result = is_pointer ? info->pointee : info->elem;
  }
  if (is_slice) {
    e->shape = SH_VALUE;
  } else if (e->shape != SH_PARAM || !is_array) {
    e->shape = SH_LVALUE;
  }
  e->type = result;
  if (emits && !is_slice) {
    emit_index(p, e, base, offset, &first, first_at);
  } else {
    exp_unemitted(e);
  }
}

// postfix -> { '.' name | '.' TypeName | '[' index_or_slice ']' | '(' args ')'
//            | type_args }
//
// A '{' can't otherwise follow a name or type, because a dict literal can only
// be at the start of an expression. So after a type it opens type arguments
// (Pair{int, str}). Only structs have type parameters, so after a name it's an
// error.
static void postfix(Parser* p, ExpDesc* e) {
  for (;;) {
    switch (T(p)) {
      case TK_DOT: {
        // '.' auto-dereferences any number of pointers. That's never ambiguous,
        // because there are no methods on pointers.
        TypeId base = e->shape == SH_TYPE ? ty_unknown : e->type;
        while (type_is_pointer(p, base)) {
          base = type_info(p, base)->pointee;
        }
        next(p);
        if (T(p) == TK_TYPENAME) {
          // module.TypeName is handled by primary().
          error_at(p, p->ls.pos.t.loc, "Unexpected type name after '.'.");
        } else if (T(p) != TK_NAME) {
          error_expected(p, "field name");
        }
        NameId name = p->ls.pos.t.v.name;
        SrcLoc name_at = p->ls.pos.t.loc;
        next(p);
        const Field* f = type_is_struct(p, base) ? struct_field(p, base, name) : NULL;
        // Methods aren't typed yet, so a name that isn't a field is only an
        // error if it isn't called.
        if (type_is_struct(p, base) && !f && T(p) != TK_LPAREN) {
          field_error(p, base, name, name_at);
        }
        if (EMIT && f && type_is_pointer(p, e->type)) {
          // Each pointer but the last is loaded through to get the next one,
          // and the last one's value is the base.
          Ref ptr = emit_to_ref(p, e, e->type, name_at);
          for (TypeId t = type_info(p, e->type)->pointee; type_is_pointer(p, t);
               t = type_info(p, t)->pointee) {
            ptr = il_emit(&p->il, OP_LOAD, IT_I64, ptr, 0);
          }
          e->kind = EK_ADDR;
          e->base = ptr;
          e->offset = f->offset;
        } else if (EMIT && f && type_is_struct(p, e->type) && e->kind == EK_LOCAL) {
          // A field of a struct local is in the local's slot, so the slot's
          // address is the base.
          Ref slot_addr = il_slot_addr(&p->il, e->slot);
          e->kind = EK_ADDR;
          e->base = slot_addr;
          e->offset = f->offset;
        } else if (EMIT && f && type_is_struct(p, e->type) && e->kind == EK_ADDR) {
          // A field of a nested struct is in the same memory as the outer
          // struct, so only the offset changes.
          e->offset += f->offset;
        } else {
          exp_unemitted(e);
        }
        bool in_param = e->shape == SH_PARAM && type_is_struct(p, e->type);
        e->shape = in_param ? SH_PARAM : SH_LVALUE;
        e->type = f ? f->type : ty_unknown;
        break;
      }
      case TK_LSQUARE:
        if (e->shape == SH_TYPE) {
          error_at(p, p->ls.pos.t.loc, "A type can't be indexed.");
        }
        index_or_slice(p, e);
        break;
      case TK_LPAREN: {
        SrcLoc at = p->ls.pos.t.loc;
        if (e->shape == SH_TYPE) {
          struct_args(p, e);
          break;
        }
        if (EMIT && e->kind != EK_FUNC) {
          error_at(p, at, "This call isn't supported yet.");
        }
        TypeId callee = e->type;
        TypeId ret = type_is_function(p, callee) ? type_info(p, callee)->ret : ty_unknown;
        bool returns_agg = type_is_aggregate(p, ret) && type_has_layout(p, ret);
#if EMIT
        // One more for a RESULTAGG.
        Ref args[MAX_CALL_ARGS + 1];
#else
        Ref* args = NULL;
#endif
        Ref* arg_refs = args;
        Ref result = 0;
        if (EMIT && returns_agg) {
          // An aggregate result is written to a new temporary, and the
          // call's value is the temporary, as for a struct literal. The
          // RESULTAGG of its address goes before the arguments. It's emitted
          // before them, which doesn't matter because the callee only writes
          // to it when it returns.
          AggId agg = type_agg(p, ret);
          SlotId temp = il_slot_add_agg(&p->il, agg, (NameId){IL_SLOT_UNNAMED});
          result = il_slot_addr(&p->il, temp);
          args[0] = il_emit(&p->il, OP_RESULTAGG, IT_I64, result, agg.u);
          arg_refs = &args[1];
        }
        u32 num_args = call_args(p, callee, arg_refs);
        e->shape = SH_CALL;
        e->type = ret;
        if (EMIT && returns_agg) {
          Ref sym = il_const_sym(&p->il, e->sym);
          il_call(&p->il, IT_VOID, sym, args, num_args + 1);
          e->kind = EK_ADDR;
          e->base = result;
          e->offset = 0;
        } else if (EMIT) {
          Ref sym = il_const_sym(&p->il, e->sym);
          e->ref = il_call(&p->il, il_type_of(p, e->type, at), sym, args, num_args);
          e->kind = EK_REF;
        } else {
          exp_unemitted(e);
        }
        break;
      }
      case TK_LBRACE:
        if (e->shape == SH_NAME || e->shape == SH_LVALUE || e->shape == SH_PARAM) {
          error_at(p, p->ls.pos.t.loc, "Type arguments can only follow a type name.");
        }
        if (e->shape != SH_TYPE) {
          return;
        }
        type_args(p);
        e->type = ty_unknown;
        if (T(p) == TK_LBRACE) {
          error_at(p, p->ls.pos.t.loc, "Type arguments were given twice.");
        }
        break;
      default:
        return;
    }
  }
}

// Suffixed literals map directly to a FixedType, which is in the same order.
static_assert(TY_F64 - TY_I8 == SUF_F64 - SUF_I8, "NumSuffix and FixedType out of sync");
static_assert(TY_U8 - TY_I8 == SUF_U8 - SUF_I8, "NumSuffix and FixedType out of sync");

static TypeId literal_type(Parser* p) {
  NumSuffix suf = (NumSuffix)(p->ls.pos.t.flags & TF_SUFFIX_MASK);
  if (suf == SUF_NONE) {
    return T(p) == TK_INTLIT ? ty_untyped_int : ty_untyped_float;
  }
  return (TypeId){TY_I8 + (suf - SUF_I8)};
}

// len_call -> 'len' '(' expr ')'
//
// The length is an int. A fixed-size array's length is a constant, but its
// operand is still evaluated. len is a builtin rather than a call of
// __len__ because fixed-size arrays can't have methods, and their length
// has to be a constant so that it can be used as an array size.
static void len_call(Parser* p, ExpDesc* e) {
  SrcLoc at = p->ls.pos.t.loc;
  next(p);  // '('
  SrcLoc arg_at = p->ls.pos.t.loc;
  ExpDesc arg;
  subexpr(p, -1, &arg);
  check_match(p, TK_RPAREN, TK_LPAREN, at);
  TypeId t = arg.type;
  // A struct can have a __len__ method, but methods aren't typed yet.
  if (type_is_integer(t) || type_is_float(t) || t.u == TY_BOOL || t.u == TY_NULL ||
      type_is_pointer(p, t)) {
    error_at(p, arg_at, "len expects an array, str, or list, not %s.", type_str(p, t));
  }
  e->shape = SH_VALUE;
  e->type = ty_i64;
  if (type_is_array(p, t)) {
    e->kind = EK_CONST;
    e->cv.i = type_info(p, t)->count;
  } else {
    exp_unemitted(e);
  }
}

// primary -> literal | string | name | TypeName | '(' expr ')'
//          | list | dict | sizeof | alignof | offsetof | cast | len_call
//          followed by postfix
static void primary(Parser* p, ExpDesc* e) {
  SrcLoc at = p->ls.pos.t.loc;
  e->shape = SH_VALUE;
  e->type = ty_unknown;
  exp_unemitted(e);
  switch (T(p)) {
    case TK_INTLIT:
      e->type = literal_type(p);
      e->kind = EK_CONST;
      e->cv.i = p->ls.pos.t.v.i;
      if (!type_is_untyped(e->type)) {
        check_const_fits(p, e, e->type, at);
      }
      next(p);
      break;
    case TK_FLOATLIT:
      e->type = literal_type(p);
      e->kind = EK_CONST;
      e->cv.f = p->ls.pos.t.v.f;
      if (!type_is_untyped(e->type)) {
        check_const_fits(p, e, e->type, at);
      }
      next(p);
      break;
    case TK_STRING:
      e->type = ty_str;
      e->kind = EK_CONST_DATA;
      if (EMIT) {
        u32 len;
        const u8* s = lex_str(&p->ls, &p->ls.pos.t, &len);
        e->data = il_data_add(p->ls.ctx, s, len, 1);
      }
      next(p);
      break;
    case TK_TRUE:
    case TK_FALSE:
      e->type = ty_bool;
      e->kind = EK_CONST;
      e->cv.b = T(p) == TK_TRUE;
      next(p);
      break;
    case TK_NULL:
      next(p);
      e->type = ty_null;
      e->kind = EK_CONST;
      e->cv.i = 0;
      break;
    case TK_STRING_BEGIN:
      interpolated_string(p);
      e->type = ty_str;
      break;
    case TK_NAME: {
      NameId name = p->ls.pos.t.v.name;
      const Name* n = name_of(p, name);
      GlobalInfo info = n->binding ? GK_NONE : global_info(p, name);
      GlobalKind kind = (GlobalKind)(info & GLOBAL_KIND_MASK);
      const Module* m = kind == GK_MODULE ? imported_module(p, name) : NULL;
      if (m) {
        next(p);
        if (T(p) != TK_DOT) {
          error_at(p, p->ls.pos.t.loc, "Expecting '.' after module name.");
        }
        next(p);
        if (T(p) != TK_NAME && T(p) != TK_TYPENAME) {
          error_expected(p, "name after module name");
        }
        check_module_member(p, m, name);
        GlobalInfo member = module_global_info(m, p->ls.pos.t.v.name);
        GlobalKind member_kind = (GlobalKind)(member & GLOBAL_KIND_MASK);
        SrcLoc member_at = p->ls.pos.t.loc;
        e->shape = T(p) == TK_TYPENAME ? SH_TYPE : SH_LVALUE;
        next(p);
        if (member_kind == GK_TYPE) {
          e->type = global_type(member);
        } else if (member_kind == GK_CONST) {
          load_named_const(p, e, &p->global_consts.items[member >> GLOBAL_KIND_BITS]);
        } else if (member_kind == GK_FUNC) {
          e->type = p->global_funcs.items[member >> GLOBAL_KIND_BITS].type;
          check_function_called(p, e, member_at);
        }
        break;
      }
      bool defined = n->binding || kind != GK_NONE;
      next(p);
      if (kind == GK_BUILTIN && name.u == p->ls.ctx->name_len.u && T(p) == TK_LPAREN) {
        len_call(p, e);
        break;
      }
      // A name followed by '=' is an assignment target, which declares the name
      // if it's new. Similarly, a loop variable is declared after the
      // expression that's iterated.
      if (!defined && T(p) != TK_EQ && !is_loop_var(p, at)) {
        error_at(p, at, "Undefined reference to '%.*s'.", (int)n->len, (const char*)n->str);
      }
      if (n->binding) {
        const Local* l = (const Local*)n->binding;
        e->type = l->type;
        if (l->kind == LK_CONST) {
          load_named_const(p, e, &p->local_consts[l - p->locals]);
        }
        if (EMIT && l->kind == LK_VAR) {
          e->kind = EK_LOCAL;
          e->slot = l->slot;
        } else if (EMIT && l->kind == LK_PARAM && type_is_aggregate(p, l->type)) {
          e->kind = EK_ADDR;
          e->base = l->ref;
          e->offset = 0;
        } else if (EMIT && l->kind == LK_PARAM) {
          e->kind = EK_REF;
          e->ref = l->ref;
        }
      } else if (kind == GK_CONST) {
        load_named_const(p, e, &p->global_consts.items[info >> GLOBAL_KIND_BITS]);
      } else if (kind == GK_FUNC) {
        const GlobalFunc* gf = &p->global_funcs.items[info >> GLOBAL_KIND_BITS];
        e->type = gf->type;
        if (EMIT && gf->sym != FUNC_NO_SYM) {
          e->kind = EK_FUNC;
          e->sym = gf->sym;
        }
      }
      if (type_is_created(e->type)) {
        check_function_called(p, e, at);
      }
      const Local* l = (const Local*)n->binding;
      bool is_param = l && l->kind == LK_PARAM && l >= &p->locals[p->func_base];
      e->shape = is_param ? SH_PARAM : SH_NAME;
      break;
    }
    case TK_TYPEPARAM:
      check_type_param(p);
      next(p);
      e->shape = SH_TYPE;
      break;
    case TK_TYPENAME:
      e->type = check_type_name(p);
      next(p);
      e->shape = SH_TYPE;
      break;
    case TK_LPAREN:
      next(p);
      subexpr(p, -1, e);
      check_match(p, TK_RPAREN, TK_LPAREN, at);
      e->shape = SH_VALUE;
      break;
    case TK_LSQUARE:
      list_literal(p, e);
      break;
    case TK_LBRACE:
      dict_literal(p);
      break;
    case TK_SIZEOF:
    case TK_ALIGNOF: {
      bool is_size = T(p) == TK_SIZEOF;
      next(p);
      at = p->ls.pos.t.loc;
      check(p, TK_LPAREN);
      TypeId t = sizeof_operand(p);
      check_match(p, TK_RPAREN, TK_LPAREN, at);
      // A known size is an untyped constant, so that it can be used as any
      // integer type, such as a u64 size for C.
      if (type_has_layout(p, t)) {
        e->type = ty_untyped_int;
        e->kind = EK_CONST;
        e->cv.i = is_size ? type_size(p, t) : type_align(p, t);
      } else {
        e->type = ty_i64;
        exp_unemitted(e);
      }
      break;
    }
    case TK_OFFSETOF: {
      next(p);
      at = p->ls.pos.t.loc;
      check(p, TK_LPAREN);
      SrcLoc type_at = p->ls.pos.t.loc;
      TypeId t = type(p);
      check(p, TK_COMMA);
      NameId name = p->ls.pos.t.v.name;
      SrcLoc name_at = p->ls.pos.t.loc;
      check_name(p, "field name");
      e->type = ty_i64;
      if (type_is_struct(p, t)) {
        const Field* f = struct_field(p, t, name);
        if (!f) {
          field_error(p, t, name, name_at);
        }
        if (type_has_layout(p, t)) {
          e->type = ty_untyped_int;
          e->kind = EK_CONST;
          e->cv.i = f->offset;
        }
      } else if (t.u != TY_UNKNOWN) {
        error_at(p, type_at, "offsetof expects a struct, not %s.", type_str(p, t));
      }
      check_match(p, TK_RPAREN, TK_LPAREN, at);
      break;
    }
    case TK_CAST: {  // cast T(expr)
      next(p);
      TypeId t = type(p);
      at = p->ls.pos.t.loc;
      check(p, TK_LPAREN);
      SrcLoc value_at = p->ls.pos.t.loc;
      subexpr(p, -1, e);
      check_match(p, TK_RPAREN, TK_LPAREN, at);
      if (!castable(p, e->type, t)) {
        error_at(p, value_at, "Can't cast %s to %s.", type_str(p, e->type), type_str(p, t));
      }
      e->shape = SH_VALUE;
      // Pointers and u64 are all i64s, so casting between them doesn't change
      // the value.
      bool is_bitcast = (type_is_pointer(p, e->type) || e->type.u == TY_U64) &&
                        (type_is_pointer(p, t) || t.u == TY_U64);
      if (e->kind == EK_CONST && (type_is_integer(t) || type_is_float(t)) &&
          (type_is_integer(e->type) || type_is_float(e->type))) {
        fold_cast(p, e, t, value_at);
      } else if (!EMIT || !is_bitcast) {
        exp_unemitted(e);
      }
      e->type = t;
      break;
    }
    default:
      error_expected(p, "expression");
  }
  postfix(p, e);
}

// Statements

// Declares an ALL_CAPS name, whose initializer v has to be a constant, with
// type t. An untyped initializer keeps its untypedness.
static void declare_const(Parser* p,
                          NameId name,
                          SrcLoc at,
                          bool toplevel,
                          TypeId t,
                          const ExpDesc* v,
                          SrcLoc value_at) {
  if (!exp_is_const(v)) {
    error_at(p, value_at, "Expected a constant expression.");
  }
  if (EMIT && v->kind == EK_CONST_DATA) {
    error_at(p, value_at, "This constant isn't supported yet.");
  }
  NamedConst c = {t, v->cv};
  if (type_is_float(t) && type_is_integer(v->type)) {
    c.cv.f = (double)v->cv.i;
  } else if (t.u == TY_F32) {
    c.cv.f = (float)v->cv.f;
  }
  if (toplevel) {
    u32 idx = p->global_consts.num;
    *vec_push(p->ls.ctx, &p->global_consts) = c;
    declare_global(p, name, GK_CONST | (idx << GLOBAL_KIND_BITS));
  } else {
    Local* l = declare_local(p, name, at, LK_CONST);
    l->type = t;
    p->local_consts[l - p->locals] = c;
  }
}

// The rest of a declaration once its type is parsed: name ['=' expr].
static void decl_rest(Parser* p, bool toplevel, TypeId t) {
  SrcLoc at;
  NameId name = expect_name(p, "variable name", &at);
  if (toplevel) {
    check_not_global(p, name, T(p) == TK_EQ ? p->ls.pos.t.loc : at);
  } else if (lookup_in_func(p, name)) {
    const Name* n = name_of(p, name);
    error_at(p, at, "'%.*s' is already declared.", (int)n->len, (const char*)n->str);
  }
  bool is_const = name_of(p, name)->is_const;
  SrcLoc value_at = at;
  ExpDesc v;
  bool has_value = optional(p, TK_EQ);
  if (has_value) {
    value_at = p->ls.pos.t.loc;
    p->const_context = toplevel || is_const;
    expr_expect(p, t, &v);
    p->const_context = false;
    check_assign(p, t, &v, value_at);
    if (is_const) {
      declare_const(p, name, at, toplevel, t, &v, value_at);
      return;
    }
    if (toplevel && !exp_is_const(&v)) {
      error_at(p, value_at, "Expected a constant expression.");
    }
  } else if (is_const) {
    error_at(p, p->ls.pos.t.loc, "A constant needs a value.");
  }
  if (toplevel) {
    declare_global(p, name, GK_VAR);
  } else {
    declare_if_new(p, name, at, t);
    if (EMIT) {
      Local* l = lookup_in_func(p, name);
      l->slot = emit_local_slot(p, t, name, at);
      if (!has_value && type_is_aggregate(p, t)) {
        emit_agg_init(p, il_slot_addr(&p->il, l->slot), t);
        return;
      }
      if (!has_value) {
        v = (ExpDesc){.type = t, .kind = EK_CONST};
      }
      emit_assign_to_local(p, l, &v, value_at);
    }
  }
}

static bool is_augassign(TokenKind kind) {
  return kind >= TK_PLUSEQ && kind <= TK_SHREQ;
}

// Indexed by an augmented assignment token's offset from TK_PLUSEQ.
static const BinOpr augassign_opr[] = {
    OPR_ADD,   // +=
    OPR_SUB,   // -=
    OPR_MUL,   // *=
    OPR_DIV,   // /=
    OPR_MOD,   // %=
    OPR_BAND,  // &=
    OPR_BOR,   // |=
    OPR_BXOR,  // ^=
    OPR_SHL,   // <<=
    OPR_SHR,   // >>=
};
static_assert(COUNTOF(augassign_opr) == TK_SHREQ - TK_PLUSEQ + 1,
              "augassign_opr doesn't match tokens");

// A declaration, assignment, or call. The postfix chain is parsed first, and
// then its shape and the next token decide which it is.
//
//   decl   -> type name ['=' expr]
//   assign -> name_or_lvalue ('=' | augop) expr
//   call   -> postfix chain ending in a call
static void decl_or_expr_stat(Parser* p, bool toplevel) {
  TokenKind k = T(p);
  SrcLoc at = p->ls.pos.t.loc;
  if (k == TK_NAME || k == TK_TYPENAME || k == TK_TYPEPARAM) {
    NameId name = p->ls.pos.t.v.name;
    ExpDesc exp;
    primary(p, &exp);
    if (exp.shape == SH_TYPE) {
      decl_rest(p, toplevel, exp.type);
      return;
    }
    if (T(p) == TK_EQ || is_augassign(T(p))) {
      if (exp.shape == SH_PARAM) {
        error_at(p, p->ls.pos.t.loc, "Function parameters are immutable.");
      }
      if (exp.shape != SH_NAME && exp.shape != SH_LVALUE) {
        error_at(p, at, "Can't assign to this expression.");
      }
      if (toplevel && T(p) != TK_EQ) {
        error_at(p, p->ls.pos.t.loc, "Augmented assignment isn't allowed at top level.");
      }
      // The current function's local that's assigned to, if it already exists.
      Local* loc = NULL;
      if (exp.shape == SH_NAME) {
        if (toplevel) {
          check_not_global(p, name, p->ls.pos.t.loc);
        }
        loc = lookup(p, name);
        // An enclosing function's 'global' doesn't apply to this function, so
        // this assignment declares a new local.
        if (loc && loc < &p->locals[p->func_base] && loc->kind == LK_GLOBAL) {
          loc = NULL;
        }
        if (loc && loc < &p->locals[p->func_base]) {
          error_at(p, p->ls.pos.t.loc, "Upvalues are immutable.");
        }
        if (loc &&
            (loc->kind == LK_CONST || (loc->kind == LK_GLOBAL && name_of(p, name)->is_const))) {
          error_at(p, p->ls.pos.t.loc, "Cannot assign a new value to a constant.");
        }
      }
      bool not_augassign = T(p) == TK_EQ;
      bool declares = exp.shape == SH_NAME && not_augassign;
      bool stores_field = not_augassign && exp.kind == EK_ADDR;
      if (EMIT && !toplevel && !declares && !stores_field) {
        error_at(p, at, "This statement isn't supported yet.");
      }
      TokenKind op_kind = T(p);
      SrcLoc op_at = p->ls.pos.t.loc;
      next(p);
      SrcLoc value_at = p->ls.pos.t.loc;
      // A new local takes the value's type, even if it shadows a global
      // function.
      bool to_existing = not_augassign && (!declares || loc);
      ExpDesc v;
      p->const_context = toplevel || (declares && name_of(p, name)->is_const);
      expr_expect(p, to_existing ? exp.type : ty_unknown, &v);
      p->const_context = false;
      if (to_existing) {
        check_assign(p, exp.type, &v, value_at);
      } else if (!not_augassign) {
        // The result is always the target's type, so it doesn't need to be
        // checked again.
        BinOpr op = augassign_opr[op_kind - TK_PLUSEQ];
        type_for_arith(p, op, exp.type, v.type, op_at);
        if (v.kind == EK_CONST) {
          check_const_operands(p, op, exp.type, &exp, at, &v, value_at, op_at);
        }
      }
      // After the expression, so that it can't see the new local.
      if (declares && name_of(p, name)->is_const) {
        // A constant keeps the type of its initializer, which can be untyped.
        declare_const(p, name, at, toplevel, v.type, &v, value_at);
      } else if (declares) {
        check_not_void(p, &v, value_at);
        if (v.type.u == TY_NULL && (toplevel || !loc)) {
          error_at(p, value_at, "Can't infer a type from null.");
        }
        TypeId t = type_concrete(v.type);
        check_const_conversion(p, t, &v, value_at);
        if (toplevel && !exp_is_const(&v)) {
          error_at(p, value_at, "Expected a constant expression.");
        }
        if (toplevel) {
          declare_global(p, name, GK_VAR);
        } else {
          if (!loc) {
            loc = declare_local(p, name, at, LK_VAR);
            loc->type = t;
            if (EMIT) {
              loc->slot = emit_local_slot(p, t, name, at);
            }
          }
          if (EMIT) {
            emit_assign_to_local(p, loc, &v, value_at);
          }
        }
      } else if (EMIT && stores_field) {
        emit_assign_to_addr(p, &exp, &v, value_at);
      }
      return;
    }
    if (exp.shape == SH_CALL) {
      if (toplevel) {
        error_at(p, at, "Calls aren't allowed at top level.");
      }
      return;
    }
    error_at(p, at, "Expression can't be used as a statement.");
  }
  if (is_type_start(k)) {
    TypeId t = type(p);
    decl_rest(p, toplevel, t);
    return;
  }
  if (k == TK_INDENT) {
    error_at(p, at, "Unexpected indent.");
  }
  error_expected(p, "statement");
}

// simple_stat -> 'return' [expr] | 'break' | 'continue' | 'pass'
//              | 'print' expr | 'check' expr
//              | ('global' | 'nonlocal') name {',' name}
//              | decl_or_expr_stat
static void simple_stat(Parser* p) {
  if (EMIT) {
    il_source_pos(&p->il, p->ls.pos.t.loc);
    if (T(p) != TK_RETURN && T(p) != TK_PASS && T(p) != TK_PRINT && T(p) != TK_BREAK &&
        T(p) != TK_CONTINUE && !is_builtin_type(T(p)) && T(p) != TK_NAME && T(p) != TK_TYPENAME &&
        T(p) != TK_STAR && T(p) != TK_LSQUARE) {
      error_at(p, p->ls.pos.t.loc, "This statement isn't supported yet.");
    }
  }
  switch (T(p)) {
    case TK_RETURN: {
      SrcLoc return_at = p->ls.pos.t.loc;
      next(p);
      if (T(p) == TK_NEWLINE || block_follow(T(p))) {
        if (p->ret_type.u != TY_VOID && p->ret_type.u != TY_UNKNOWN) {
          error_at(p, return_at, "Missing return value in a function returning %s.",
                   type_str(p, p->ret_type));
        }
        if (EMIT) {
          il_emit(&p->il, OP_RET, IT_VOID, 0, 0);
        }
      } else {
        SrcLoc at = p->ls.pos.t.loc;
        ExpDesc e;
        expr_expect(p, p->ret_type, &e);
        if (p->ret_type.u == TY_VOID) {
          error_at(p, at, "Can't return a value from a function with no return type.");
        }
        if (!assignable(p, e.type, p->ret_type)) {
          error_at(p, at, "Can't return %s from a function returning %s.", type_str(p, e.type),
                   type_str(p, p->ret_type));
        }
        check_const_conversion(p, p->ret_type, &e, at);
        if (EMIT && p->il.ret == IT_AGG) {
          // The caller's RESULTAGG isn't visible here until the ABI step
          // makes it a hidden pointer or the result registers, so the RET
          // has the address of the value.
          il_emit(&p->il, OP_RET, IT_AGG, emit_agg_addr(p, &e, at), 0);
        } else if (EMIT) {
          il_emit(&p->il, OP_RET, p->il.ret, emit_to_ref(p, &e, p->ret_type, at), 0);
        }
      }
      return;
    }
    case TK_BREAK:
    case TK_CONTINUE:
      if (!p->loop) {
        error_at(p, p->ls.pos.t.loc, "'%s' isn't in a loop.", tok_kind_name(T(p)));
      }
      if (T(p) == TK_BREAK) {
        p->loop->has_break = true;
      }
      if (EMIT && T(p) == TK_BREAK) {
        il_jump(&p->il, p->loop->exit);
      } else if (EMIT) {
        il_jump(&p->il, p->loop->head);
      }
      next(p);
      return;
    case TK_PASS:
      next(p);
      return;
    case TK_PRINT: {
      next(p);
      if (EMIT && T(p) == TK_STRING_BEGIN) {
        print_interpolated(p);
        return;
      }
      SrcLoc at = p->ls.pos.t.loc;
      ExpDesc e;
      subexpr(p, -1, &e);
      check_not_void(p, &e, at);
      if (EMIT) {
        emit_print_value(p, &e, at);
        emit_print_end(p);
      }
      return;
    }
    case TK_CHECK: {
      next(p);
      SrcLoc at = p->ls.pos.t.loc;
      ExpDesc e;
      subexpr(p, -1, &e);
      expect_bool(p, &e, at, "'check'");
      return;
    }
    case TK_GLOBAL:
      next(p);
      for (;;) {
        SrcLoc at;
        NameId name = expect_name(p, "variable name", &at);
        if (global_kind(p, name) == GK_NONE) {
          const Name* n = name_of(p, name);
          error_at(p, at, "Undefined global '%.*s'.", (int)n->len, (const char*)n->str);
        }
        declare_local(p, name, at, LK_GLOBAL);
        if (!optional(p, TK_COMMA)) {
          break;
        }
      }
      return;
    case TK_NONLOCAL:
      next(p);
      for (;;) {
        check_name(p, "variable name");
        if (!optional(p, TK_COMMA)) {
          break;
        }
      }
      return;
    case TK_IF:
    case TK_FOR:
    case TK_WITH:
    case TK_DEF:
      error_at(p, p->ls.pos.t.loc, "'%s' can't follow ':' on the same line.", tok_kind_name(T(p)));
    default:
      decl_or_expr_stat(p, /*toplevel=*/false);
      return;
  }
}

static Flow simple_stat_flow(TokenKind k) {
  bool ends = k == TK_RETURN || k == TK_BREAK || k == TK_CONTINUE;
  return ends ? FLOW_ENDS : FLOW_FALLS_THROUGH;
}

// block -> INDENT stat {stat} DEDENT
static Flow block(Parser* p) {
  next(p);  // INDENT
  Flow flow;
  for (;;) {
    TokenKind k = T(p);
    flow = stat(p);
    if (block_follow(T(p))) {
      break;
    }
    if (simple_stat_flow(k) != FLOW_FALLS_THROUGH) {
      error_expected(p, "end of block");
    }
  }
  check(p, TK_DEDENT);
  return flow;
}

// The body after a ':'. Either an indented block, or a single simple
// statement on the same line (`if x: return 1`).
static Flow suite(Parser* p) {
  if (T(p) == TK_INDENT) {
    return block(p);
  }
  if (T(p) == TK_NEWLINE || block_follow(T(p))) {
    error_expected(p, "an indented block");
  }
  Flow flow = simple_stat_flow(T(p));
  simple_stat(p);
  end_of_stat(p);
  return flow;
}

// One branch of an 'if', `cond ':' suite`. A false condition goes to the
// next branch. A suite that falls through jumps to the block after the whole
// 'if', which is declared in *end when it's first needed, because there isn't
// one if every branch returns.
static Flow if_branch(Parser* p, BlockId* end) {
  if (EMIT) {
    il_source_pos(&p->il, p->ls.pos.t.loc);
  }
  Ref condition = cond(p);
  BlockId next_branch = {0};
  if (EMIT) {
    next_branch = il_block_declare(&p->il);
    il_branch(&p->il, condition, next_branch);
  }
  check(p, TK_COLON);
  Flow flow = suite(p);
  if (EMIT) {
    if (flow == FLOW_FALLS_THROUGH) {
      if (end->u == IL_BLOCK_NONE) {
        *end = il_block_declare(&p->il);
      }
      il_jump(&p->il, *end);
    }
    il_block_start(&p->il, next_branch);
  }
  return flow;
}

// funcsig -> [type] name '(' [param {',' param}] ')'
// param   -> type name | name    (bare name: a method's first parameter)
//
// The return type is optional, so after `def` a NAME is either the function
// name or the start of a module.Type return type. A following '.' means it's
// a type. A method's `self` and a module.Type parameter are distinguished the
// same way.
//
// A top-level function's name is a global, and a nested function's name is a
// local of the enclosing function. The new function's scope is opened after
// the name, and the return value is for func_close().
static u32 funcsig(Parser* p, bool is_method, NameId* func_name, SrcLoc* name_at) {
  NameId name = p->ls.pos.t.v.name;
  SrcLoc at = p->ls.pos.t.loc;
  TypeId ret = ty_void;
  if (T(p) == TK_NAME) {
    next(p);
    if (optional(p, TK_DOT)) {  // module.Type return type
      ret = qualified_type_rest(p, name, at);
      name = expect_name(p, "function name", &at);
    }
  } else {
    if (is_type_start(T(p))) {
      ret = type(p);
    }
    name = expect_name(p, "function name", &at);
  }
  if (name_of(p, name)->is_const) {
    error_at(p, at, "A constant can't be a function name.");
  }
  *func_name = name;
  *name_at = at;
  // The type is created before the name is declared, and the parameters are
  // added to it as they're parsed.
  TypeId ft = {p->types.num};
  *vec_push(p->ls.ctx, &p->types) =
      (TypeInfo){.kind = TYPE_FUNC, .ret = ret, .first_param = p->param_types.num};
  if (!is_method) {
    if (p->func_depth == 0) {
      u32 idx = p->global_funcs.num;
      u32 sym = EMIT && !p->in_foreign ? ctx_sym_add(p->ls.ctx, name) : FUNC_NO_SYM;
      *vec_push(p->ls.ctx, &p->global_funcs) = (GlobalFunc){ft, sym};
      declare_definition(p, name, at, GK_FUNC | (idx << GLOBAL_KIND_BITS));
    } else {
      declare_if_new(p, name, at, ft);
    }
  }
  if (T(p) == TK_LBRACE) {
    error_at(p, p->ls.pos.t.loc, "Functions can't have type parameters, only structs.");
  }
  u32 outer = func_open(p);
  p->ret_type = ret;

  SrcLoc lparen = p->ls.pos.t.loc;
  check(p, TK_LPAREN);
  if (T(p) != TK_RPAREN) {
    bool first = true;
    for (;;) {
      name = p->ls.pos.t.v.name;
      at = p->ls.pos.t.loc;
      TypeId t = ty_unknown;
      if (T(p) == TK_NAME) {
        next(p);
        if (optional(p, TK_DOT)) {  // module.Type param
          t = qualified_type_rest(p, name, at);
          name = expect_name(p, "parameter name", &at);
        } else if (!(is_method && first)) {
          error_at(p, p->ls.pos.t.loc, "Parameter needs a type.");
        }
      } else {
        t = type(p);
        name = expect_name(p, "parameter name", &at);
      }
      declare_local(p, name, at, LK_PARAM)->type = t;
      *vec_push(p->ls.ctx, &p->param_types) = t;
      ++type_info(p, ft)->num_params;
      first = false;
      if (!optional(p, TK_COMMA)) {
        break;
      }
    }
  }
  check_match(p, TK_RPAREN, TK_LPAREN, lparen);
  return outer;
}

// foreign_decl -> funcsig end   (after 'foreign')
static void foreign_decl(Parser* p, bool is_method) {
  p->in_foreign = true;
  NameId name;
  SrcLoc name_at;
  func_close(p, funcsig(p, is_method, &name, &name_at));
  p->in_foreign = false;
  end_of_stat(p);
}

// funcdef -> funcsig ':' suite   (after 'def')
static void funcdef(Parser* p, bool is_method) {
  TypeId outer_ret = p->ret_type;
  Loop* outer_loop = p->loop;
  p->loop = NULL;
  NameId name;
  SrcLoc name_at;
  u32 outer = funcsig(p, is_method, &name, &name_at);
  if (EMIT) {
    if (is_method) {
      error_at(p, name_at, "Methods aren't supported yet.");
    }
    if (p->func_depth > 1) {
      error_at(p, name_at, "Nested functions aren't supported yet.");
    }
    if (type_is_aggregate(p, p->ret_type) && type_has_layout(p, p->ret_type)) {
      il_reset(&p->il, name, IT_AGG);
      p->il.ret_agg = type_agg(p, p->ret_type);
    } else {
      il_reset(&p->il, name, il_type_of(p, p->ret_type, name_at));
    }
    il_block_start(&p->il, il_block_declare(&p->il));
    // Nothing in the body has been parsed yet, so the function's only locals
    // are its parameters.
    for (u32 i = p->func_base; i < p->num_locals; ++i) {
      Local* l = &p->locals[i];
      u32 index = i - p->func_base;
      if (type_is_aggregate(p, l->type) && type_has_layout(p, l->type)) {
        l->ref = il_emit(&p->il, OP_PARAMAGG, IT_I64, index, type_agg(p, l->type).u);
      } else {
        l->ref = il_emit(&p->il, OP_PARAM, il_type_of(p, l->type, name_at), index, 0);
      }
    }
  }
  check(p, TK_COLON);
  Flow flow = suite(p);
  if (flow == FLOW_FALLS_THROUGH && p->ret_type.u != TY_VOID) {
    error_at(p, name_at, "Missing return at the end of a function returning %s.",
             type_str(p, p->ret_type));
  }
  if (EMIT) {
    if (flow == FLOW_FALLS_THROUGH) {
      il_emit(&p->il, OP_RET, IT_VOID, 0, 0);
    }
    Ctx* ctx = p->ls.ctx;
    if (ctx->on_func) {
      ctx->on_func(ctx, &p->il, p->mod->src.buf);
    }
  }
  func_close(p, outer);
  p->ret_type = outer_ret;
  p->loop = outer_loop;
}

// for_header -> for_targets 'in' expr | expr
//
// A bare name followed by 'in' or ',' is a loop variable. Any other name
// starts a condition, which continues from the name. Returns the condition's
// value when emitting, and otherwise 0.
static Ref for_header(Parser* p) {
  SrcLoc at = p->ls.pos.t.loc;
  ExpDesc e;
  if (T(p) == TK_NAME) {
    NameId name = p->ls.pos.t.v.name;
    p->loop_var_at = at;
    primary(p, &e);
    p->loop_var_at = (SrcLoc){0};
    if ((e.shape == SH_NAME || e.shape == SH_PARAM) && (T(p) == TK_IN || T(p) == TK_COMMA)) {
      if (EMIT) {
        error_at(p, at, "Iterating isn't supported yet.");
      }
      Targets t;
      for_targets(p, &t, name, at);
      check(p, TK_IN);
      expr(p);
      for (u32 i = 0; i < t.num_targets; ++i) {
        declare_if_new(p, t.name[i], t.at[i], ty_unknown);
      }
      return 0;
    }
    binop_rest(p, -1, &e, at);
  } else {
    subexpr(p, -1, &e);
  }
  return cond_value(p, &e, at);
}

// stat -> 'if' expr ':' suite {'elif' expr ':' suite} ['else' ':' suite]
//       | 'for' [for_header] ':' suite
//       | 'with' expr ['as' name] ':' suite
//       | 'def' funcdef
//       | simple_stat end
//
//
// An 'if' only ends if it has an 'else' and all of its branches end. A
// 'for' with a header might not run its body, so it only ends if it's 'for:'
// with no break.
static Flow stat(Parser* p) {
  switch (T(p)) {
    case TK_IF: {
      next(p);
      BlockId end = {IL_BLOCK_NONE};
      // &= rather than &&, so that every branch is still parsed.
      bool ends = if_branch(p, &end) == FLOW_ENDS;
      while (optional(p, TK_ELIF)) {
        ends &= if_branch(p, &end) == FLOW_ENDS;
      }
      if (optional(p, TK_ELSE)) {
        check(p, TK_COLON);
        ends &= suite(p) == FLOW_ENDS;
      } else {
        ends = false;
      }
      // The else suite, or the last condition being false, continues into
      // the end block.
      if (EMIT && end.u != IL_BLOCK_NONE) {
        il_block_start(&p->il, end);
      }
      return ends ? FLOW_ENDS : FLOW_FALLS_THROUGH;
    }
    case TK_FOR: {
      SrcLoc at = p->ls.pos.t.loc;
      next(p);
      bool forever = T(p) == TK_COLON;
      Loop loop = {0};
      if (EMIT) {
        loop.head = il_block_declare(&p->il);
        loop.exit = il_block_declare(&p->il);
        il_block_start(&p->il, loop.head);
      }
      if (!forever) {
        if (EMIT) {
          il_source_pos(&p->il, at);
        }
        Ref condition = for_header(p);
        if (EMIT) {
          il_branch(&p->il, condition, loop.exit);
        }
      }
      check(p, TK_COLON);
      Loop* outer = p->loop;
      p->loop = &loop;
      Flow flow = suite(p);
      p->loop = outer;
      bool ends = forever && !loop.has_break;
      if (EMIT) {
        // The end of the body goes back to the head, unless the body always
        // ends in a return (or a break or continue, which jump themselves).
        if (flow == FLOW_FALLS_THROUGH) {
          il_jump(&p->il, loop.head);
        }
        // Nothing jumps to the exit of a 'for:' without a break, so it's
        // declared but never started.
        if (!ends) {
          il_block_start(&p->il, loop.exit);
        }
      }
      return ends ? FLOW_ENDS : FLOW_FALLS_THROUGH;
    }
    case TK_WITH:
      if (EMIT) {
        error_at(p, p->ls.pos.t.loc, "'with' isn't supported yet.");
      }
      next(p);
      expr(p);
      if (optional(p, TK_AS)) {
        SrcLoc at;
        NameId name = expect_name(p, "variable name", &at);
        if (name_of(p, name)->is_const) {
          error_at(p, at, "A constant can't be a 'with' variable.");
        }
        declare_if_new(p, name, at, ty_unknown);
      }
      check(p, TK_COLON);
      return suite(p);
    case TK_DEF:
      next(p);
      funcdef(p, /*is_method=*/false);
      return FLOW_FALLS_THROUGH;
    default: {
      Flow flow = simple_stat_flow(T(p));
      simple_stat(p);
      end_of_stat(p);
      return flow;
    }
  }
}

// Top level

// Grows p->image to size bytes with zeros.
static void image_extend(Parser* p, u32 size) {
  vec_reserve(p->ls.ctx, &p->image, size - p->image.num);
  memset(&p->image.items[p->image.num], 0, size - p->image.num);
  p->image.num = size;
}

// Writes defval, the default of a field of type t, at offset in p->image.
// Every target is little-endian.
static void image_add_const(Parser* p, TypeId t, const ExpDesc* defval, u32 offset, SrcLoc at) {
  if (t.u == TY_BOOL) {
    p->image.items[offset] = defval->cv.b;
  } else if (type_is_integer(t)) {
    u64 bits = (u64)defval->cv.i;
    for (u32 i = 0; i < type_size(p, t); ++i) {
      p->image.items[offset + i] = (u8)(bits >> (8 * i));
    }
  } else if (!type_is_pointer(p, t)) {
    error_at(p, at, "This field default isn't supported yet.");
  }
}

// Copies the images of any structs in a field of type t to offset in
// p->image. Returns whether there were any.
static bool image_add_field(Parser* p, TypeId t, u32 offset) {
  if (type_is_array(p, t)) {
    const TypeInfo* info = type_info(p, t);
    if (info->count == 0 || !image_add_field(p, info->elem, offset)) {
      return false;
    }
    u32 elem_size = type_size(p, info->elem);
    for (u32 i = 1; i < info->count; ++i) {
      memcpy(&p->image.items[offset + i * elem_size], &p->image.items[offset], elem_size);
    }
    return true;
  }
  if (!type_is_struct(p, t) || type_info(p, t)->image.u == DATA_NONE) {
    return false;
  }
  const IlData* d = &p->ls.ctx->data.items[type_info(p, t)->image.u];
  memcpy(&p->image.items[offset], &p->ls.ctx->databytes.items[d->off], d->len);
  return true;
}

// Returns the AggId of a struct or array type t. A struct's was added at the
// end of its definition. An array's is added the first time it's needed.
static AggId type_agg(Parser* p, TypeId t) {
  TypeInfo* info = type_info(p, t);
  if (info->agg.u != AGG_NONE) {
    return info->agg;
  }
  ASSERT(info->kind == TYPE_ARRAY && type_has_layout(p, t));
  u32 size = type_size(p, t);
  AggLeaf leaves[AGG_MAX_CLASSIFIED];
  u32 num_leaves = 0;
  if (size <= AGG_MAX_CLASSIFIED) {
    type_agg_leaves(p, t, 0, leaves, &num_leaves);
  }
  info->agg = il_agg_add(p->ls.ctx, size, type_align(p, t), leaves, num_leaves);
  p->image.num = 0;
  image_extend(p, size);
  if (image_add_field(p, t, 0)) {
    info->image = il_data_add(p->ls.ctx, p->image.items, size, type_align(p, t));
  }
  return info->agg;
}

// struct_body -> INDENT field {field} DEDENT
// field       -> type name ['=' expr] end
//
// Fields are laid out as in C. A field whose layout isn't known makes the
// whole struct's layout unknown. Defaults have to be constants, so that a
// struct's initial value can be a constant image.
static void struct_body(Parser* p, TypeId st) {
  if (T(p) != TK_INDENT) {
    error_expected(p, "an indented block of fields");
  }
  next(p);
  u32 first = p->fields.num;
  u32 size = 0;
  u32 align = 1;
  bool known = true;
  bool has_image = false;
  p->image.num = 0;
  for (;;) {
    SrcLoc type_at = p->ls.pos.t.loc;
    TypeId t = type(p);
    TypeId inner = t;
    while (type_is_array(p, inner)) {
      inner = type_info(p, inner)->elem;
    }
    if (inner.u == st.u) {
      const Name* n = name_of(p, type_info(p, st)->name);
      error_at(p, type_at, "'%.*s' can't contain itself.", (int)n->len, (const char*)n->str);
    }
    NameId name = p->ls.pos.t.v.name;
    SrcLoc name_at = p->ls.pos.t.loc;
    check_name(p, "field name");
    for (u32 i = first; i < p->fields.num; ++i) {
      if (p->fields.items[i].name.u == name.u) {
        const Name* n = name_of(p, name);
        error_at(p, name_at, "Duplicate struct field name '%.*s'.", (int)n->len,
                 (const char*)n->str);
      }
    }
    known = known && type_has_layout(p, t);
    u32 offset = 0;
    if (known) {
      u32 field_align = type_align(p, t);
      offset = align_up(size, field_align);
      size = offset + type_size(p, t);
      align = field_align > align ? field_align : align;
      if (EMIT) {
        image_extend(p, size);
        has_image |= image_add_field(p, t, offset);
      }
    }
    if (optional(p, TK_EQ)) {
      SrcLoc value_at = p->ls.pos.t.loc;
      ExpDesc v;
      p->const_context = true;
      expr_expect(p, t, &v);
      p->const_context = false;
      check_assign(p, t, &v, value_at);
      if (!exp_is_const(&v)) {
        error_at(p, value_at, "Expected a constant expression.");
      }
      if (EMIT && known) {
        image_add_const(p, t, &v, offset, value_at);
        has_image = true;
      }
    }
    *vec_push(p->ls.ctx, &p->fields) = (Field){name, t, offset};
    end_of_stat(p);
    if (block_follow(T(p))) {
      break;
    }
  }
  check(p, TK_DEDENT);
  TypeInfo* info = type_info(p, st);
  info->first_field = first;
  info->num_fields = p->fields.num - first;
  if (known) {
    size = align_up(size, align);
    AggLeaf leaves[AGG_MAX_CLASSIFIED];
    u32 num_leaves = 0;
    if (size <= AGG_MAX_CLASSIFIED) {
      type_agg_leaves(p, st, 0, leaves, &num_leaves);
    }
    info->agg = il_agg_add(p->ls.ctx, size, align, leaves, num_leaves);
    if (EMIT && has_image) {
      image_extend(p, size);
      info->image = il_data_add(p->ls.ctx, p->image.items, size, align);
    }
  }
}

// receiver -> TypeName [type_params] | builtin
//           | '[' ']' TypeName | '{' TypeName '}' TypeName
//
// Methods can only be defined in the receiver type's own module, so the
// receiver is never module.Type. There's no specialization, so the type
// arguments of a generic receiver are always new type parameters. There are
// no methods on pointers, because '.' auto-dereferences, and so p.f() would be
// ambiguous.
static void receiver_param(Parser* p) {
  if (is_type_start(T(p)) && T(p) != TK_TYPEPARAM) {
    error_at(p, p->ls.pos.t.loc, "Methods can't be defined for specific type arguments.");
  }
  type_param(p);
}

static void receiver(Parser* p) {
  SrcLoc at = p->ls.pos.t.loc;
  switch (T(p)) {
    case TK_TYPENAME:
      check_type_name(p);
      next(p);
      if (T(p) == TK_LBRACE) {
        SrcLoc brace = p->ls.pos.t.loc;
        next(p);
        for (;;) {
          receiver_param(p);
          if (!optional(p, TK_COMMA)) {
            break;
          }
        }
        check_match(p, TK_RBRACE, TK_LBRACE, brace);
      }
      return;
    case TK_LSQUARE:
      next(p);
      if (T(p) != TK_RSQUARE) {
        error_at(p, at, "Methods can't be defined on fixed-size arrays.");
      }
      next(p);
      receiver_param(p);
      return;
    case TK_LBRACE:
      next(p);
      receiver_param(p);
      check_match(p, TK_RBRACE, TK_LBRACE, at);
      receiver_param(p);
      return;
    case TK_STAR:
      error_at(p, at, "Methods can't be defined on pointer types.");
    case TK_NAME:
      error_at(p, at, "Methods can only be defined in the type's own module.");
    default:
      if (!is_builtin_type(T(p))) {
        error_expected(p, "type to define methods on");
      }
      next(p);
      return;
  }
}

// toplevel_stat -> 'def' funcdef
//                | 'on' receiver ('def' funcdef | 'foreign' funcsig end)
//                | 'struct' TypeName [type_params] ':' struct_body
//                | 'foreign' funcsig end
//                | decl_or_expr_stat end    (declarations and assignments)
static void toplevel_stat(Parser* p) {
  p->type_params = 0;
  switch (T(p)) {
    case TK_DEF:
      next(p);
      funcdef(p, /*is_method=*/false);
      return;
    case TK_ON:
      next(p);
      receiver(p);
      if (optional(p, TK_FOREIGN)) {
        foreign_decl(p, /*is_method=*/true);
      } else {
        check(p, TK_DEF);
        funcdef(p, /*is_method=*/true);
      }
      return;
    case TK_STRUCT: {
      next(p);
      // str is a keyword, so the prelude's definition of it fills in TY_STR's
      // entry, and isn't declared, since keywords are never looked up.
      bool is_str = T(p) == TK_STR && p->mod == p->ls.ctx->modules.items[MODULE_PRELUDE];
      if (T(p) != TK_TYPENAME && !is_str) {
        error_expected(p, "struct name (a TypeName)");
      }
      NameId name = is_str ? lex_intern_safe(p->ls.ctx, "str") : p->ls.pos.t.v.name;
      SrcLoc at = p->ls.pos.t.loc;
      next(p);
      if (T(p) == TK_LBRACE) {
        type_params(p);
      }
      // The layout isn't known until the body ends, so the struct can only
      // refer to itself through pointers.
      TypeInfo info = {.kind = TYPE_STRUCT, .name = name, .agg = {AGG_NONE}, .image = {DATA_NONE}};
      TypeId st;
      if (is_str) {
        st = ty_str;
        ASSERT(type_info(p, st)->kind == TYPE_NONE && !p->type_params);
        info.ptr = type_info(p, st)->ptr;
        *type_info(p, st) = info;
      } else {
        st = (TypeId){p->types.num};
        *vec_push(p->ls.ctx, &p->types) = info;
        TypeId bound = p->type_params ? ty_unknown : st;
        declare_definition(p, name, at, GK_TYPE | (bound.u << GLOBAL_KIND_BITS));
      }
      check(p, TK_COLON);
      struct_body(p, st);
      if (is_str) {
        p->str_data_offset = str_field_offset(p, "_data");
        p->str_len_offset = str_field_offset(p, "_len");
      }
      return;
    }
    case TK_FOREIGN:
      next(p);
      foreign_decl(p, /*is_method=*/false);
      return;
    case TK_IMPORT:
      error_at(p, p->ls.pos.t.loc, "Imports must come before other declarations.");
    default:
      decl_or_expr_stat(p, /*toplevel=*/true);
      end_of_stat(p);
      return;
  }
}

// Modules

// Returns len zero bytes followed by LEX_PADDING more, for a name that will be
// interned, because lex_intern_padded() reads past the end.
static u8* name_buf_new(Ctx* ctx, u32 len) {
  u8* buf = (u8*)calloc(len + LEX_PADDING, 1);
  if (!buf) {
    ctx_error(ctx, NULL, NULL, (SrcLoc){0}, "Out of memory.");
  }
  return buf;
}

// Adds a module to ctx, which takes ownership of name_buf. Its source isn't
// set yet.
static Module* module_new(Ctx* ctx, u8* name_buf, NameId name) {
  Module* m = (Module*)calloc(1, sizeof(Module));
  if (!m) {
    free(name_buf);
    ctx_error(ctx, NULL, NULL, (SrcLoc){0}, "Out of memory.");
  }
  *vec_push(ctx, &ctx->modules) = m;
  m->name = name;
  m->name_buf = name_buf;
  m->loading = true;
  if (!namemap_init(&m->imports, 16)) {
    ctx_error(ctx, NULL, NULL, (SrcLoc){0}, "Out of memory.");
  }
  namemap_set(ctx, &ctx->module_ids, name, ctx->modules.num - 1);
  return m;
}

// Returns len bytes for a filename, plus one for the terminating NUL.
static char* filename_new(Ctx* ctx, size_t len) {
  char* filename = (char*)malloc(len + 1);
  if (!filename) {
    ctx_error(ctx, NULL, NULL, (SrcLoc){0}, "Out of memory.");
  }
  return filename;
}

// Reads m's source from filename, which m takes ownership of. Returns false if
// it can't be read.
static bool module_read(Ctx* ctx, Module* m, char* filename) {
  m->src.filename = filename;
  size_t size;
  u8* buf = ctx->load_file(filename, &size);
  if (!buf) {
    return false;
  }
  m->src.buf = buf;
  m->src.end = buf + size;
  // Most of the cost of growing a large table is page faults, so it starts
  // with room for a global every 64 bytes of source. A large calloc() gets
  // pages that the OS has already zeroed, so the pages that are never used
  // aren't touched.
  u32 cap = MIN_GLOBALS;
  while (cap < size / 32) {
    cap *= 2;
  }
  if (!namemap_init(&m->globals, cap)) {
    ctx_error(ctx, filename, NULL, (SrcLoc){0}, "Out of memory.");
  }
  return true;
}

// Parses m, and then returns p to where it was. Imports are before anything
// else in a file, so p has no other state that could be seen or changed by
// parsing m.
static void parse_import(Parser* p, Module* m) {
  ASSERT(p->num_locals == 0 && p->func_depth == 0 && p->type_params == 0 &&
         p->ls.pos.interp_sp == 0);
  Module* mod = p->mod;
  FileBuf src = p->ls.src;
  LexPos pos = p->ls.pos;
  p->mod = m;
  lex_init(&p->ls, p->ls.ctx, &m->src);
  parse_module(p);
  m->loading = false;
  p->mod = mod;
  p->ls.src = src;
  p->ls.pos = pos;
}

// Reports an import of ctx->modules.items[idx], which is still loading.
NORETURN static void error_import_cycle(Parser* p, u32 idx, SrcLoc at) {
  // The modules that are loading are the chain of imports that led here, and
  // they're in Ctx.modules in the same order.
  Ctx* ctx = p->ls.ctx;
  char chain[1000];
  size_t len = 0;
  for (u32 i = idx; i < ctx->modules.num && len < sizeof(chain); ++i) {
    const Module* m = ctx->modules.items[i];
    if (m->loading) {
      const Name* n = lex_name(ctx, m->name);
      len += (size_t)snprintf(chain + len, sizeof(chain) - len, "%.*s -> ", (int)n->len,
                              (const char*)n->str);
    }
  }
  const Name* n = lex_name(ctx, ctx->modules.items[idx]->name);
  error_at(p, at, "Import cycle: %s%.*s.", chain, (int)n->len, (const char*)n->str);
}

// Returns the index in Ctx.modules of the module that path[0..len) names,
// relative to the current module's directory, and loads and parses it if it's
// new. at is the path's location, for errors.
static u32 import_module(Parser* p, const u8* path, u32 len, SrcLoc at) {
  Ctx* ctx = p->ls.ctx;
  const char* err = path_import_is_invalid(path, len);
  if (err) {
    error_at(p, at, "%s", err);
  }
  const Name* from = lex_name(ctx, p->mod->name);
  u32 dir = path_dir_len(from->str, from->len);
  u32 name_len = dir + len;
  u8* name_buf = name_buf_new(ctx, name_len);
  memcpy(name_buf, from->str, dir);
  memcpy(name_buf + dir, path, len);
  u32 joined_len = name_len;
  name_len = path_normalize(name_buf, name_len);
  memset(name_buf + name_len, 0, joined_len - name_len);
  NameId name = lex_intern_padded(ctx, name_buf, name_len);
  const u32* idx = namemap_find(&ctx->module_ids, name);
  if (idx) {
    // The module's name was already interned when it was added, so the Name
    // doesn't point into name_buf.
    free(name_buf);
    if (ctx->modules.items[*idx]->loading) {
      error_import_cycle(p, *idx, at);
    }
    return *idx;
  }
  Module* m = module_new(ctx, name_buf, name);
  // Modules that m imports are added after it.
  u32 m_idx = ctx->modules.num - 1;
  // Files are found relative to the root module's directory.
  const char* root = ctx->modules.items[MODULE_ROOT]->src.filename;
  u32 root_dir = path_dir_len((const u8*)root, (u32)strlen(root));
  char* filename = filename_new(ctx, root_dir + name_len + strlen(".luv"));
  memcpy(filename, root, root_dir);
  memcpy(filename + root_dir, name_buf, name_len);
  memcpy(filename + root_dir + name_len, ".luv", sizeof(".luv"));
  if (!module_read(ctx, m, filename)) {
    error_at(p, at, "Couldn't open import, looking for '%s'.", filename);
  }
  parse_import(p, m);
  return m_idx;
}

// module -> {import} {toplevel_stat} EOS
// import -> 'import' STRING ['as' name] end
static void parse_module(Parser* p) {
  next(p);
  while (optional(p, TK_IMPORT)) {
    // Escaped strings are decoded into Ctx.strbytes, which isn't padded for
    // lex_intern_padded(), and a module name has no reason to contain
    // escapes. This also means that a path can't contain '\', so it always
    // uses '/'.
    if (T(p) != TK_STRING || (p->ls.pos.t.flags & TF_ESCAPED)) {
      error_expected(p, "quoted module name");
    }
    u32 len;
    const u8* s = lex_str(&p->ls, &p->ls.pos.t, &len);
    u32 idx = import_module(p, s, len, p->ls.pos.t.loc);
    // The module is bound to the last component of its path.
    u32 base = path_dir_len(s, len);
    NameId name = lex_intern_padded(p->ls.ctx, s + base, len - base);
    next(p);
    if (optional(p, TK_AS)) {
      SrcLoc at;
      name = expect_name(p, "module alias", &at);
    }
    declare_global(p, name, GK_MODULE);
    namemap_set(p->ls.ctx, &p->mod->imports, name, idx);
    end_of_stat(p);
  }
  while (T(p) != TK_EOS) {
    toplevel_stat(p);
  }
}

// Parses the prelude as module 0, and fills in prelude_globals.
//
// NameIds are allocated in the order that names are first interned, and the
// prelude is the first source that's lexed. So every name that it defines is
// below prelude_name_end, and the prelude's globals can be found by indexing
// with the NameId instead of hashing. Names that the compiler interns before
// it, such as keywords, are also below prelude_name_end, but they're never
// globals, so their entries are GK_NONE.
static void prelude_load(Ctx* ctx, Parser* p) {
  ASSERT(ctx->modules.num == MODULE_PRELUDE);
  static const char* const builtins[] = {"len", "max", "range"};
  NameId builtin_names[COUNTOF(builtins)];
  for (u32 i = 0; i < COUNTOF(builtins); ++i) {
    builtin_names[i] = lex_intern_safe(ctx, builtins[i]);
  }
  Module* m = (Module*)calloc(1, sizeof(Module));
  if (!m) {
    ctx_error(ctx, NULL, NULL, (SrcLoc){0}, "Out of memory.");
  }
  *vec_push(ctx, &ctx->modules) = m;
  // It isn't in Ctx.module_ids, so it can't be imported.
  m->name = lex_intern_safe(ctx, "prelude");
  m->src = (FileBuf){"<prelude>", prelude_src, prelude_src + PRELUDE_LEN, /*is_static=*/true};
  m->loading = true;
  if (!namemap_init(&m->globals, MIN_GLOBALS) || !namemap_init(&m->imports, 16)) {
    ctx_error(ctx, NULL, NULL, (SrcLoc){0}, "Out of memory.");
  }
  p->mod = m;
  lex_init(&p->ls, ctx, &m->src);
  parse_module(p);
  m->loading = false;
  u32 end = ctx->names.num;
  p->prelude_globals = (GlobalInfo*)malloc(end * sizeof(GlobalInfo));
  if (!p->prelude_globals) {
    ctx_error(ctx, NULL, NULL, (SrcLoc){0}, "Out of memory.");
  }
  for (u32 i = 0; i < end; ++i) {
    p->prelude_globals[i] = module_global_info(m, (NameId){i});
  }
  for (u32 i = 0; i < COUNTOF(builtins); ++i) {
    ASSERT(p->prelude_globals[builtin_names[i].u] == GK_NONE);
    p->prelude_globals[builtin_names[i].u] = GK_BUILTIN;
  }
  p->prelude_name_end = end;
}

// Undoes the file's bindings and frees the Parser.
static void parse_end(Parser* p) {
  pop_locals(p, 0);
  if (EMIT) {
    il_free(&p->il);
  }
  free(p->prelude_globals);
  free(p->global_consts.items);
  free(p->global_funcs.items);
  free(p->types.items);
  free(p->param_types.items);
  free(p->fields.items);
  free(p->image.items);
  hashtable_free(&p->array_types);
  free(p);
}

void PARSE_FILE(Ctx* ctx, const char* path) {
  ASSERT(ctx->modules.num == 0);
  // The Parser isn't a local, so that it's still valid after a longjmp. All of
  // its fields start as zero, including those that are freed by parse_end().
  Parser* p = (Parser*)calloc(1, sizeof(Parser));
  if (!p) {
    ctx_error(ctx, path, NULL, (SrcLoc){0}, "Out of memory.");
  }
  // Names are shared by all files parsed with ctx, so this file's bindings
  // are undone at the end, including after an error.
  jmp_buf* outer = ctx->on_error;
  jmp_buf jb;
  ctx->on_error = &jb;
  if (setjmp(jb)) {
    parse_end(p);
    ctx->on_error = outer;
    longjmp(*outer, 1);
  }
  vec_reserve(ctx, &p->types, INITIAL_TYPES);
  memset(p->types.items, 0, TY_NUM_FIXED * sizeof(TypeInfo));
  p->types.num = TY_NUM_FIXED;
  vec_reserve(ctx, &p->param_types, INITIAL_TYPES);
  if (!hashtable_init(&p->array_types, 64)) {
    ctx_error(ctx, path, NULL, (SrcLoc){0}, "Out of memory.");
  }
  if (EMIT) {
    il_init(&p->il, ctx);
  }
  prelude_load(ctx, p);
  // The root module's canonical name is its file name, without the directory
  // or .luv.
  u32 len;
  const char* name = path_stem(path, &len);
  u8* name_buf = name_buf_new(ctx, len);
  memcpy(name_buf, name, len);
  p->mod = module_new(ctx, name_buf, lex_intern_padded(ctx, name_buf, len));
  size_t path_len = strlen(path);
  char* filename = filename_new(ctx, path_len);
  memcpy(filename, path, path_len + 1);
  if (!module_read(ctx, p->mod, filename)) {
    ctx_error(ctx, filename, NULL, (SrcLoc){0}, "Couldn't open '%s'.", filename);
  }
  lex_init(&p->ls, ctx, &p->mod->src);
  parse_module(p);
  p->mod->loading = false;
  parse_end(p);
  ctx->on_error = outer;
}
