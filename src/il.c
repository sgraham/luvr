#include "luvr.h"

static const char* const il_op_names[] = {
#define IL_NAME_(name, max_operands, has_result) #name,
    IL_OPS(IL_NAME_)
#undef IL_NAME_
};

static const u8 il_op_max_operands[] = {
#define IL_MAX_OPERANDS_(name, max_operands, has_result) max_operands,
    IL_OPS(IL_MAX_OPERANDS_)
#undef IL_MAX_OPERANDS_
};

static const bool il_op_has_result[] = {
#define IL_HAS_RESULT_(name, max_operands, has_result) has_result,
    IL_OPS(IL_HAS_RESULT_)
#undef IL_HAS_RESULT_
};

static const char* const il_type_names[] = {
    [IT_VOID] = "void",  //
    [IT_I8] = "i8",      //
    [IT_U8] = "u8",      //
    [IT_I16] = "i16",    //
    [IT_U16] = "u16",    //
    [IT_I32] = "i32",    //
    [IT_I64] = "i64",    //
    [IT_F32] = "f32",    //
    [IT_F64] = "f64",    //
};

void il_init(IlFunc* f, Ctx* ctx) {
  memset(f, 0, sizeof(*f));
  f->ctx = ctx;
  if (!hashtable_init(&f->const_map, 64)) {
    ctx_error(ctx, NULL, NULL, 0, "Out of memory.");
  }
}

void il_free(IlFunc* f) {
  free(f->ins.v);
  free(f->consts.v);
  free(f->const_types.v);
  hashtable_free(&f->const_map);
  memset(f, 0, sizeof(*f));
}

void il_reset(IlFunc* f) {
  f->ins.num = 0;
  f->consts.num = 0;
  f->const_types.num = 0;
  hashtable_clear(&f->const_map);
}

static NORETURN void il_error_too_large(IlFunc* f) {
  ctx_error(f->ctx, NULL, NULL, 0, "Function too large.");
}

Ref il_emit(IlFunc* f, IlOp op, IlType ty, Ref a, Ref b) {
  if (f->ins.num >= REF_INS) {
    il_error_too_large(f);
  }
  *vec_push(f->ctx, &f->ins) = ins_make(op, ty, a, b);
  return REF_INS | (f->ins.num - 1);
}

Ref il_const_int(IlFunc* f, IlType ty, u64 v) {
  ASSERT(ty == IT_I32 || ty == IT_I64);
  if (ty == IT_I32) {
    v = (u64)(int32_t)v;
  }
  // Constants are interned so that each value and type has only one Ref. Then
  // operands can be compared by Ref alone, both when looking for common
  // subexpressions and when folding.
  u32 tag = hashtable_tag((v ^ ty) * 0x9e3779b97f4a7c15ull);
  HashSlot* s = hashtable_first(&f->const_map, tag);
  for (; s->tag != 0; s = hashtable_next(&f->const_map, s, tag)) {
    if (f->consts.v[s->val] == v && f->const_types.v[s->val] == ty) {
      return s->val + 1;
    }
  }
  if (f->consts.num >= REF_INS - 1) {
    il_error_too_large(f);
  }
  u32 idx = f->consts.num;
  *vec_push(f->ctx, &f->consts) = v;
  *vec_push(f->ctx, &f->const_types) = (u8)ty;
  hashtable_insert(f->ctx, &f->const_map, s, tag, idx);
  return idx + 1;
}

static void il_dump_ref(const IlFunc* f, Ref r, FILE* out) {
  if (r & REF_INS) {
    fprintf(out, "%%%u", r & ~REF_INS);
    return;
  }
  fprintf(out, "%" PRId64, (int64_t)f->consts.v[r - 1]);
}

void il_dump(const IlFunc* f, FILE* out) {
  for (u32 i = 0; i < f->ins.num; ++i) {
    Ins ins = f->ins.v[i];
    IlOp op = ins_op(ins);
    IlType ty = ins_type(ins);
    fputs("  ", out);
    if (il_op_has_result[op]) {
      fprintf(out, "%%%u = ", i);
    }
    fputs(il_op_names[op], out);
    if (ty != IT_VOID) {
      fprintf(out, " %s", il_type_names[ty]);
    }
    Ref operands[2] = {ins_a(ins), ins_b(ins)};
    for (u32 j = 0; j < il_op_max_operands[op] && operands[j]; ++j) {
      fputs(j ? ", " : " ", out);
      il_dump_ref(f, operands[j], out);
    }
    fputc('\n', out);
  }
}
