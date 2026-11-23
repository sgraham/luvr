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
    [IT_SYM] = "sym",    //
    [IT_DATA] = "data",  //
    [IT_AGG] = "agg",    //
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
  free(f->slots.v);
  free(f->source_pos.v);
  memset(f, 0, sizeof(*f));
}

void il_reset(IlFunc* f, NameId name, IlType ret) {
  f->name = name;
  f->ret = ret;
  f->ins.num = 0;
  f->consts.num = 0;
  f->const_types.num = 0;
  hashtable_clear(&f->const_map);
  f->block_starts.num = 0;
  f->slots.num = 0;
  f->source_pos.num = 0;
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

static Ref il_const(IlFunc* f, IlType ty, u64 v) {
  // Constants are interned so that each value and type has only one Ref. Then
  // operands can be compared by Ref alone, both when looking for common
  // subexpressions and when folding.
  u32 tag = hashtable_tag_of_key(v ^ ty);
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

Ref il_const_int(IlFunc* f, IlType ty, u64 v) {
  ASSERT(ty == IT_I32 || ty == IT_I64);
  if (ty == IT_I32) {
    v = (u64)(int32_t)v;
  }
  return il_const(f, ty, v);
}

Ref il_const_sym(IlFunc* f, u32 sym) {
  return il_const(f, IT_SYM, sym);
}

Ref il_const_data(IlFunc* f, u32 data) {
  return il_const(f, IT_DATA, data);
}

u32 il_data_add(Ctx* ctx, const u8* s, u32 n) {
  IlData* d = vec_push(ctx, &ctx->data);
  d->off = ctx->databytes.num;
  d->len = n;
  if (n != 0) {
    vec_reserve(ctx, &ctx->databytes, n);
    memcpy(ctx->databytes.v + ctx->databytes.num, s, n);
    ctx->databytes.num += n;
  }
  return ctx->data.num - 1;
}

Ref il_call(IlFunc* f, IlType ty, Ref callee, const Ref* args, u32 n) {
  Ref call = il_emit(f, OP_CALL, ty, callee, n);
  for (u32 i = 0; i < n; i += 2) {
    il_emit(f, OP_EXT, IT_VOID, args[i], i + 1 < n ? args[i + 1] : 0);
  }
  return call;
}

void il_copy(IlFunc* f, Ref dst, Ref src, AggId agg) {
  il_emit(f, OP_COPY, IT_VOID, dst, src);
  il_emit(f, OP_EXT, IT_VOID, agg.idx, 0);
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

SlotId il_slot_add(IlFunc* f, IlType ty, NameId name) {
  // Slot numbers are stored in an operand.
  if (f->slots.num >= (1u << 24)) {
    il_error_too_large(f);
  }
  IlSlot* s = vec_push(f->ctx, &f->slots);
  *s = (IlSlot){.name = name, .type = ty, .agg = {AGG_NONE}};
  return (SlotId){f->slots.num - 1};
}

SlotId il_slot_add_agg(IlFunc* f, AggId agg, NameId name) {
  ASSERT(agg.idx != AGG_NONE);
  SlotId slot = il_slot_add(f, IT_AGG, name);
  f->slots.v[slot.idx].agg = agg;
  return slot;
}

Ref il_slot_addr(IlFunc* f, SlotId slot) {
  return il_emit(f, OP_SLOT, IT_I64, slot.idx, 0);
}

void il_source_pos(IlFunc* f, u32 offset) {
  IlSourcePos* p = vec_push(f->ctx, &f->source_pos);
  p->ins = f->ins.num;
  p->offset = offset;
}

// Writes the line of src that contains offset.
static void il_dump_source_line(const u8* src, u32 offset, FILE* out) {
  const u8* start = src + offset;
  while (start > src && start[-1] != '\n') {
    --start;
  }
  const u8* end = src + offset;
  while (*end != '\n' && *end != 0) {
    ++end;
  }
  fprintf(out, "  ; %.*s\n", (int)(end - start), start);
}

static void il_dump_ref(const IlFunc* f, Ref r, FILE* out) {
  if (r & REF_INS) {
    fprintf(out, "%%%u", r & ~REF_INS);
    return;
  }
  u32 idx = r - 1;
  u64 v = f->consts.v[idx];
  if (f->const_types.v[idx] == IT_SYM) {
    const Name* name = lex_name(f->ctx, f->ctx->syms.v[v]);
    fprintf(out, "@%.*s", (int)name->len, name->str);
    return;
  }
  if (f->const_types.v[idx] == IT_DATA) {
    fprintf(out, "@.dat%" PRIu64, v);
    return;
  }
  fprintf(out, "%" PRId64, (int64_t)v);
}

void il_dump(const IlFunc* f, const u8* src, FILE* out) {
  fputs("func ", out);
  if (f->ret != IT_VOID) {
    fprintf(out, "%s ", il_type_names[f->ret]);
  }
  const Name* name = lex_name(f->ctx, f->name);
  fprintf(out, "@%.*s\n", (int)name->len, name->str);
  for (u32 i = 0; i < f->slots.num; ++i) {
    const IlSlot* s = &f->slots.v[i];
    if (s->type == IT_AGG) {
      fprintf(out, "  slot $%u #%u", i, s->agg.idx);
    } else {
      fprintf(out, "  slot $%u %s", i, il_type_names[s->type]);
    }
    if (s->name.idx != IL_SLOT_UNNAMED) {
      const Name* n = lex_name(f->ctx, s->name);
      fprintf(out, " %.*s", (int)n->len, n->str);
    }
    fputc('\n', out);
  }
  u32 pos = 0;
  for (u32 i = 0; i < f->ins.num; ++i) {
    for (; pos < f->source_pos.num && f->source_pos.v[pos].ins == i; ++pos) {
      il_dump_source_line(src, f->source_pos.v[pos].offset, out);
    }
    Ins ins = f->ins.v[i];
    IlOp op = ins_op(ins);
    IlType ty = ins_type(ins);
    if (op == OP_BLOCK) {
      fprintf(out, ":%u\n", ins_a(ins));
      continue;
    }
    // An EXT's operands are printed on its CALL's or COPY's line.
    if (op == OP_EXT) {
      continue;
    }
    fputs("  ", out);
    if (il_op_has_result[op] && ty != IT_VOID) {
      fprintf(out, "%%%u = ", i);
    }
    fputs(il_op_names[op], out);
    // A SLOT is always an i64 address, so its type would only be clutter.
    if (ty != IT_VOID && op != OP_SLOT) {
      fprintf(out, " %s", il_type_names[ty]);
    }
    if (op == OP_CALL) {
      fputc(' ', out);
      il_dump_ref(f, ins_a(ins), out);
      for (u32 j = 0; j < ins_b(ins); ++j) {
        Ins ext = f->ins.v[i + 1 + j / 2];
        fputs(", ", out);
        il_dump_ref(f, j % 2 ? ins_b(ext) : ins_a(ext), out);
      }
      fputc('\n', out);
      continue;
    }
    if (op == OP_COPY) {
      fputc(' ', out);
      il_dump_ref(f, ins_a(ins), out);
      fputs(", ", out);
      il_dump_ref(f, ins_b(ins), out);
      fprintf(out, ", #%u\n", ins_a(f->ins.v[i + 1]));
      continue;
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
      } else if (kind == OPND_SLOT) {
        fprintf(out, "$%u", operands[j]);
      } else if (kind == OPND_AGG) {
        fprintf(out, "#%u", operands[j]);
      } else if (kind == OPND_NUM) {
        fprintf(out, "%u", operands[j]);
      } else {
        il_dump_ref(f, operands[j], out);
      }
    }
    fputc('\n', out);
  }
  // Statements at the end that emitted nothing.
  for (; pos < f->source_pos.num; ++pos) {
    il_dump_source_line(src, f->source_pos.v[pos].offset, out);
  }
}

void il_dump_data(const Ctx* ctx, u32 i, FILE* out) {
  const IlData* d = &ctx->data.v[i];
  fprintf(out, "data @.dat%u \"", i);
  for (u32 j = 0; j < d->len; ++j) {
    u8 c = ctx->databytes.v[d->off + j];
    if (c == '"' || c == '\\') {
      fprintf(out, "\\%c", c);
    } else if (c == '\n') {
      fputs("\\n", out);
    } else if (c == '\t') {
      fputs("\\t", out);
    } else if (c >= ' ' && c <= '~') {
      fputc(c, out);
    } else {
      fprintf(out, "\\x%02x", c);
    }
  }
  fputs("\"\n", out);
}

void il_dump_agg(const Ctx* ctx, u32 i, FILE* out) {
  const AggInfo* agg = &ctx->aggs.v[i];
  fprintf(out, "agg #%u %u %u", i, agg->size, 1u << agg->align_log2);
  static const char* const abi_names[] = {"sse0", "sse1", "hfa_f32", "hfa_f64"};
  for (u32 bit = 0; bit < COUNTOF(abi_names); ++bit) {
    if (agg->abi & (1u << bit)) {
      fprintf(out, " %s", abi_names[bit]);
    }
  }
  fputc('\n', out);
}

static bool il_type_is_float(IlType ty) {
  return ty == IT_F32 || ty == IT_F64;
}

AggId il_agg_add(Ctx* ctx, u32 size, u32 align, const AggLeaf* leaves, u32 num_leaves) {
  ASSERT(align != 0 && (align & (align - 1)) == 0 && align <= 8);
  u8 abi = 0;
  if (size <= AGG_MAX_CLASSIFIED) {
    ASSERT(num_leaves <= size);
    bool has_scalar[2] = {false, false};
    bool has_int[2] = {false, false};
    bool same_type = true;
    for (u32 i = 0; i < num_leaves; ++i) {
      ASSERT(leaves[i].offset < size);
      same_type = same_type && leaves[i].type == leaves[0].type;
      u32 eightbyte = leaves[i].offset / 8;
      if (eightbyte < 2) {
        has_scalar[eightbyte] = true;
        has_int[eightbyte] = has_int[eightbyte] || !il_type_is_float(leaves[i].type);
      }
    }
    if (size <= 16) {
      if (has_scalar[0] && !has_int[0]) {
        abi |= AGG_SSE0;
      }
      if (has_scalar[1] && !has_int[1]) {
        abi |= AGG_SSE1;
      }
    }
    if (same_type && num_leaves >= 1 && num_leaves <= 4) {
      if (leaves[0].type == IT_F32) {
        abi |= AGG_HFA_F32;
      } else if (leaves[0].type == IT_F64) {
        abi |= AGG_HFA_F64;
      }
    }
  }
  u8 align_log2 = 0;
  while ((1u << align_log2) < align) {
    ++align_log2;
  }
  *vec_push(ctx, &ctx->aggs) = (AggInfo){.size = size, .align_log2 = align_log2, .abi = abi};
  AggId id = {ctx->aggs.num - 1};
  if (ctx->on_agg) {
    ctx->on_agg(ctx, id);
  }
  return id;
}
