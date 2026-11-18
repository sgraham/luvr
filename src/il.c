#include "luvr.h"

static const char* const il_op_names[] = {
#define IL_NAME_(name, a, b, has_result) #name,
    IL_OPS(IL_NAME_)
#undef IL_NAME_
};

static const u8 il_op_operands[][2] = {
#define IL_OPERANDS_(name, a, b, has_result) {OPND_##a, OPND_##b},
    IL_OPS(IL_OPERANDS_)
#undef IL_OPERANDS_
};

static const bool il_op_has_result[] = {
#define IL_HAS_RESULT_(name, a, b, has_result) has_result,
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
  free(f->block_starts.v);
  memset(f, 0, sizeof(*f));
}

void il_reset(IlFunc* f) {
  f->ins.num = 0;
  f->consts.num = 0;
  f->const_types.num = 0;
  hashtable_clear(&f->const_map);
  f->block_starts.num = 0;
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

u32 il_block_declare(IlFunc* f) {
  // Block numbers are stored in an operand.
  if (f->block_starts.num >= (1u << 24)) {
    il_error_too_large(f);
  }
  *vec_push(f->ctx, &f->block_starts) = IL_BLOCK_UNSTARTED;
  return f->block_starts.num - 1;
}

void il_block_start(IlFunc* f, u32 b) {
  ASSERT(f->block_starts.v[b] == IL_BLOCK_UNSTARTED);
  f->block_starts.v[b] = f->ins.num;
  il_emit(f, OP_BLOCK, IT_VOID, b, 0);
}

void il_jump(IlFunc* f, u32 b) {
  il_emit(f, OP_JMP, IT_VOID, b, 0);
}

void il_branch(IlFunc* f, Ref cond, u32 b) {
  il_emit(f, OP_BR, IT_VOID, cond, b);
  il_block_start(f, il_block_declare(f));
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
    if (op == OP_BLOCK) {
      fprintf(out, ":%u\n", ins_a(ins));
      continue;
    }
    fputs("  ", out);
    if (il_op_has_result[op]) {
      fprintf(out, "%%%u = ", i);
    }
    fputs(il_op_names[op], out);
    if (ty != IT_VOID) {
      fprintf(out, " %s", il_type_names[ty]);
    }
    u32 operands[2] = {ins_a(ins), ins_b(ins)};
    for (u32 j = 0; j < 2; ++j) {
      IlOperand kind = (IlOperand)il_op_operands[op][j];
      if (kind == OPND_NONE || (kind == OPND_REF && operands[j] == 0)) {
        break;
      }
      fputs(j ? ", " : " ", out);
      if (kind == OPND_BLOCK) {
        fprintf(out, ":%u", operands[j]);
      } else {
        il_dump_ref(f, operands[j], out);
      }
    }
    fputc('\n', out);
  }
}
