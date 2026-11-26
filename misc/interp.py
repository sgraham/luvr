#!/usr/bin/env python3

# Runs the IL printed by `luv il`, to test emission before there's a back
# end.
#
#   luv il file.luv | misc/interp.py
#   misc/interp.py file.il
#
# The exit status is main's return value, as it would be for a native program.

import ast
import struct
import sys
from dataclasses import dataclass

TYPES = {"i8", "u8", "i16", "u16", "i32", "i64", "f32", "f64"}

# Sizes of the integer types in memory. Loads of the narrow ones give i32s.
INT_SIZES = {"i8": 1, "u8": 1, "i16": 2, "u16": 2, "i32": 4, "i64": 8}
SCALAR_SIZES = INT_SIZES | {"f32": 4, "f64": 8}

# The first character of an operand says what it is, except for integer
# constants.
OPERAND_KINDS = {"%": "ins", ":": "block", "$": "slot", "@": "sym", "#": "agg"}


class Error(Exception):
    pass


@dataclass
class Operand:
    kind: str  # "int", "float", or a value of OPERAND_KINDS
    # The n of %n, :n, $n or #n, a constant, or a symbol name.
    val: int | float | str


# All of the program's memory: data, then the frames of the calls that are
# running. Address 0 is never used, so that it can be null.
@dataclass
class ReadOnly:
    start: int
    end: int
    name: str  # the data's symbol name


@dataclass
class Memory:
    bytes: bytearray
    top: int  # the first unused address
    # The ro data, which is an error to write to, as it would be in a
    # read-only section.
    readonly: list


@dataclass
class Program:
    funcs: dict  # Funcs by symbol name
    data: dict  # Addresses in mem by symbol name
    aggs: dict  # Aggs by number
    mem: Memory


# The value of a RESULTAGG. It's an address like any other, but the CALL that
# it's the first argument of has to tell it apart from a pointer argument.
@dataclass
class ResultDst:
    addr: int


@dataclass
class Agg:
    size: int
    align: int


@dataclass
class Slot:
    size: int
    align: int
    name: str | None  # None for an unnamed temporary


class Ins:

    def __init__(self, result, op, ty, operands):
        self.result = result  # the n of %n, or None
        self.op = op
        self.ty = ty  # None for void
        self.operands = operands  # Operands


class Func:

    def __init__(self, name, ret):
        self.name = name
        self.ret = ret
        self.slots = []  # Slots, by slot number
        self.ins = []
        # Block number -> index in ins of the block's first instruction.
        self.blocks = {}


def parse_operand(s):
    if s[0] in OPERAND_KINDS:
        v = s[1:]
        return Operand(OPERAND_KINDS[s[0]], v if s[0] == "@" else int(v))
    # A float constant always has a '.' or an exponent, or is inf or nan.
    if any(c in s for c in ".en"):
        return Operand("float", float(s))
    if s[0].isdigit() or s[0] == "-":
        return Operand("int", int(s))
    raise Error(f"bad operand '{s}'")


def parse_ins(line):
    result = None
    if line.startswith("%"):
        lhs, line = line.split(" = ", 1)
        result = int(lhs[1:])
    words = line.split(None, 1)
    op = words[0]
    rest = words[1] if len(words) > 1 else ""
    ty = None
    first = rest.split(None, 1)
    if first and first[0] in TYPES:
        ty = first[0]
        rest = first[1] if len(first) > 1 else ""
    operands = []
    if rest:
        operands = [parse_operand(s.strip()) for s in rest.split(",")]
    return Ins(result, op, ty, operands)


def alloc(mem, size, align):
    addr = (mem.top + align - 1) & -align
    mem.top = addr + size
    if mem.top > len(mem.bytes):
        mem.bytes.extend(bytes(mem.top - len(mem.bytes)))
    return addr


# Floats are packed in the target's formats.
FLOAT_FORMATS = {"f32": "<f", "f64": "<d"}


def round_to_f32(v):
    return struct.unpack("<f", struct.pack("<f", v))[0]


def load(mem, addr, ty):
    if ty in FLOAT_FORMATS:
        fmt = FLOAT_FORMATS[ty]
        return struct.unpack(fmt, mem.bytes[addr:addr + struct.calcsize(fmt)])[0]
    if ty not in INT_SIZES:
        raise Error(f"LOAD {ty} isn't supported yet")
    n = INT_SIZES[ty]
    return int.from_bytes(mem.bytes[addr:addr + n], "little", signed=ty[0] == "i")


# Every write to memory goes through here.
def write(mem, addr, b):
    for r in mem.readonly:
        if addr < r.end and r.start < addr + len(b):
            raise Error(f"write to read-only data @{r.name}")
    mem.bytes[addr:addr + len(b)] = b


# Values are kept in the signed range of their type, except after ZEXT32, so
# only the low bits are stored.
def store(mem, addr, ty, v):
    if ty in FLOAT_FORMATS:
        write(mem, addr, struct.pack(FLOAT_FORMATS[ty], v))
        return
    if ty not in INT_SIZES:
        raise Error(f"STORE {ty} isn't supported yet")
    n = INT_SIZES[ty]
    write(mem, addr, (v & ((1 << (8 * n)) - 1)).to_bytes(n, "little"))


def parse(text):
    funcs = {}
    data = {}
    aggs = {}
    mem = Memory(bytearray(), 8, [])
    f = None
    for lineno, raw in enumerate(text.split("\n"), 1):
        line = raw.strip()
        if not line or line.startswith(";"):
            continue
        try:
            if line.startswith("data "):
                # Examples:
                #   data @.dat0 ro align 1 "bytes"
                #   data @.dat1 ro align 8 01 @.dat0,
                #       where @.dat0 is its 8-byte address
                #   data @.dat2 rw align 8 zero 16
                # Data is always dumped after the data it refers to.
                _, name, access, _, align, s = line.split(None, 5)
                align = int(align)
                b = bytearray()
                if s.startswith("zero "):
                    b += bytes(int(s.split()[1]))
                elif s.startswith('"'):
                    # The escapes are a subset of a Python bytes literal's.
                    b += ast.literal_eval("b" + s)
                else:
                    for tok in s.split():
                        if tok.startswith("@"):
                            if tok[1:] not in data:
                                raise Error(f"{tok} isn't defined before it's used")
                            b += data[tok[1:]].to_bytes(8, "little")
                        else:
                            b.append(int(tok, 16))
                addr = alloc(mem, len(b), align)
                mem.bytes[addr:addr + len(b)] = b
                data[name[1:]] = addr
                if access == "ro":
                    mem.readonly.append(ReadOnly(addr, addr + len(b), name[1:]))
            elif line.startswith("agg "):
                # agg #0 size 16 align 8 sse0, where the ABI bits are only
                # needed by a back end.
                _, name, _, size, _, align, *_ = line.split()
                aggs[int(name[1:])] = Agg(int(size), int(align))
            elif line.startswith("func "):
                # func i64 @main, func #1 @f when it returns agg #1, or func @f
                # when it returns nothing.
                words = line.split()
                ret = words[1] if len(words) == 3 else None
                f = Func(words[-1][1:], ret)
                funcs[f.name] = f
            elif f is None:
                raise Error("expected 'func'")
            elif line.startswith("slot "):
                # slot $0 i64 x, slot $1 i32 for an unnamed temporary, or
                # slot $2 #0 s for an aggregate.
                words = line.split()
                if words[2].startswith("#"):
                    agg = aggs[int(words[2][1:])]
                    size, align = agg.size, agg.align
                else:
                    size = align = SCALAR_SIZES[words[2]]
                f.slots.append(Slot(size, align, words[3] if len(words) > 3 else None))
            elif line.startswith(":"):
                f.blocks[int(line[1:])] = len(f.ins)
            else:
                f.ins.append(parse_ins(line))
        except (Error, ValueError, IndexError, SyntaxError) as e:
            raise Error(f"line {lineno}: {e}: {raw}")
    return Program(funcs, data, aggs, mem)


def value_of(values, prog, operand):
    if operand.kind in ("int", "float"):
        return operand.val
    if operand.kind == "ins":
        return values[operand.val]
    if operand.kind == "sym" and operand.val in prog.data:
        return prog.data[operand.val]
    raise Error(f"can't use a {operand.kind} as a value")


# Integers are kept as Python ints in the signed range of their type.
def wrap(ty, v):
    bits = {"i32": 32, "i64": 64}[ty]
    # The low bits are the two's complement result. If the top one of them is
    # set, it's negative.
    v &= (1 << bits) - 1
    if v >= 1 << (bits - 1):
        v -= 1 << bits
    return v


BITS = {"i32": 32, "i64": 64}


# Division truncates toward zero, unlike Python's //.
def div(a, b):
    if b == 0:
        raise Error("divide by zero")
    q = abs(a) // abs(b)
    return q if (a < 0) == (b < 0) else -q


# The remainder has the sign of the dividend.
def rem(a, b):
    return a - b * div(a, b)


# v reinterpreted as an unsigned integer of n bits.
def unsigned(n, v):
    return v & ((1 << n) - 1)


# The functions take the instruction's width in bits. A shift only uses the
# low bits of its count.
ARITH = {
    "ADD": lambda n, a, b: a + b,
    "SUB": lambda n, a, b: a - b,
    "MUL": lambda n, a, b: a * b,
    "DIV": lambda n, a, b: div(a, b),
    "UDIV": lambda n, a, b: div(unsigned(n, a), unsigned(n, b)),
    "REM": lambda n, a, b: rem(a, b),
    "UREM": lambda n, a, b: rem(unsigned(n, a), unsigned(n, b)),
    "AND": lambda n, a, b: a & b,
    "OR": lambda n, a, b: a | b,
    "XOR": lambda n, a, b: a ^ b,
    "SHL": lambda n, a, b: a << (b & (n - 1)),
    "SHR": lambda n, a, b: unsigned(n, a) >> (b & (n - 1)),
    "SAR": lambda n, a, b: a >> (b & (n - 1)),
}

# Extensions of the low bits of an i32 to all of it.
NARROW = {"SEXT8": (8, True), "ZEXT8": (8, False), "SEXT16": (16, True), "ZEXT16": (16, False)}

# Compares give 0 or 1. The unsigned ones reinterpret the operands at the
# instruction's width.
COMPARE = {
    "EQ": lambda a, b: a == b,
    "NE": lambda a, b: a != b,
    "LT": lambda a, b: a < b,
    "LE": lambda a, b: a <= b,
    "GT": lambda a, b: a > b,
    "GE": lambda a, b: a >= b,
}

UNSIGNED_COMPARE = {"ULT": "LT", "ULE": "LE", "UGT": "GT", "UGE": "GE"}


# s is the address of a str, which is {*u8 _data, int _len} as in the prelude.
def print_str(mem, s):
    data = load(mem, s, "i64")
    n = load(mem, s + 8, "i64")
    sys.stdout.flush()
    sys.stdout.buffer.write(mem.bytes[data:data + n])


# The runtime's functions, by symbol name. Each is passed the Memory first.
RUNTIME = {
    "luv_print_i64": lambda mem, v: sys.stdout.write(str(v)),
    "luv_print_u64": lambda mem, v: sys.stdout.write(str(v & ((1 << 64) - 1))),
    "luv_print_f64": lambda mem, v: sys.stdout.write("%f" % v),
    "luv_print_bool": lambda mem, v: sys.stdout.write("true" if v else "false"),
    "luv_print_str": print_str,
    "luv_print_end": lambda mem: sys.stdout.write("\n"),
}


# result_dst is the address that a function returning an aggregate copies its
# result to, from the caller's RESULTAGG.
def run(prog, f, args, result_dst):
    values = {}
    mem = prog.mem
    frame_base = mem.top
    slot_addrs = [alloc(mem, s.size, s.align) for s in f.slots]
    pc = 0
    while pc < len(f.ins):
        ins = f.ins[pc]
        pc += 1
        if ins.op in ("PARAM", "PARAMAGG"):
            values[ins.result] = args[ins.operands[0].val]
            continue
        if ins.op == "RESULTAGG":
            values[ins.result] = ResultDst(value_of(values, prog, ins.operands[0]))
            continue
        if ins.op == "ARGAGG":
            # The callee gets its own copy, made when the argument is
            # evaluated. It's in the caller's frame, which outlives the call.
            src = value_of(values, prog, ins.operands[0])
            agg = prog.aggs[ins.operands[1].val]
            addr = alloc(mem, agg.size, agg.align)
            write(mem, addr, mem.bytes[src:src + agg.size])
            values[ins.result] = addr
            continue
        if ins.op == "SLOT":
            values[ins.result] = slot_addrs[ins.operands[0].val]
            continue
        if ins.op == "LOAD":
            values[ins.result] = load(mem, value_of(values, prog, ins.operands[0]), ins.ty)
            continue
        if ins.op == "STORE":
            addr = value_of(values, prog, ins.operands[1])
            store(mem, addr, ins.ty, value_of(values, prog, ins.operands[0]))
            continue
        if ins.op == "ZERO":
            addr = value_of(values, prog, ins.operands[0])
            size = prog.aggs[ins.operands[1].val].size
            write(mem, addr, bytes(size))
            continue
        if ins.op == "COPY":
            dst, src = (value_of(values, prog, o) for o in ins.operands[:2])
            size = prog.aggs[ins.operands[2].val].size
            write(mem, dst, mem.bytes[src:src + size])
            continue
        if ins.op in ARITH:
            a, b = (value_of(values, prog, o) for o in ins.operands)
            values[ins.result] = wrap(ins.ty, ARITH[ins.op](BITS[ins.ty], a, b))
            continue
        if ins.op in COMPARE:
            a, b = (value_of(values, prog, o) for o in ins.operands)
            values[ins.result] = int(COMPARE[ins.op](a, b))
            continue
        if ins.op in UNSIGNED_COMPARE:
            a, b = (unsigned(BITS[ins.ty], value_of(values, prog, o)) for o in ins.operands)
            values[ins.result] = int(COMPARE[UNSIGNED_COMPARE[ins.op]](a, b))
            continue
        if ins.op == "NEG":
            values[ins.result] = wrap(ins.ty, -value_of(values, prog, ins.operands[0]))
            continue
        if ins.op in NARROW:
            bits, signed = NARROW[ins.op]
            v = unsigned(bits, value_of(values, prog, ins.operands[0]))
            if signed and v >= 1 << (bits - 1):
                v -= 1 << bits
            values[ins.result] = v
            continue
        if ins.op == "FEXT":
            # A constant operand is read as an f64, so it's rounded here.
            values[ins.result] = round_to_f32(value_of(values, prog, ins.operands[0]))
            continue
        if ins.op == "SEXT32":
            values[ins.result] = wrap("i32", value_of(values, prog, ins.operands[0]))
            continue
        if ins.op == "ZEXT32":
            values[ins.result] = value_of(values, prog, ins.operands[0]) & 0xFFFFFFFF
            continue
        if ins.op == "CALL":
            callee = ins.operands[0]
            args = [value_of(values, prog, o) for o in ins.operands[1:]]
            dst = None
            if args and isinstance(args[0], ResultDst):
                dst = args.pop(0).addr
            if callee.kind == "sym" and callee.val in prog.funcs:
                r = run(prog, prog.funcs[callee.val], args, dst)
            elif callee.kind == "sym" and callee.val in RUNTIME:
                r = RUNTIME[callee.val](mem, *args)
            else:
                raise Error(f"{f.name}: can't call {callee.val}")
            if ins.result is not None:
                values[ins.result] = r
            continue
        if ins.op == "JMP":
            pc = f.blocks[ins.operands[0].val]
            continue
        if ins.op == "BR":
            if value_of(values, prog, ins.operands[0]) == 0:
                pc = f.blocks[ins.operands[1].val]
            continue
        if ins.op == "RET" and f.ret is not None and f.ret.startswith("#"):
            # The callee doesn't know where the caller wants the result,
            # because RESULTAGG isn't one of its parameters. So the RET is of
            # wherever the callee built it, which is often a local or a
            # temporary in its own frame, and it's copied to the caller's
            # destination here, before that frame is freed.
            src = value_of(values, prog, ins.operands[0])
            size = prog.aggs[int(f.ret[1:])].size
            write(mem, result_dst, mem.bytes[src:src + size])
            mem.top = frame_base
            return None
        if ins.op == "RET":
            mem.top = frame_base
            return value_of(values, prog, ins.operands[0]) if ins.operands else None
        raise Error(f"{f.name}: {ins.op} isn't supported yet")
    raise Error(f"{f.name}: ran off the end")


def main():
    text = open(sys.argv[1]).read() if len(sys.argv) > 1 else sys.stdin.read()
    try:
        prog = parse(text)
        if "main" not in prog.funcs:
            raise Error("no main")
        ret = run(prog, prog.funcs["main"], [], None)
    except Error as e:
        sys.exit(f"interp: {e}")
    sys.exit((ret or 0) & 0xFF)


if __name__ == "__main__":
    main()
