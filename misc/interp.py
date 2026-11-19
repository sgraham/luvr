#!/usr/bin/env python3

# Runs the IL printed by `luv il`, to test emission before there's a back
# end.
#
#   luv il file.luv | misc/interp.py
#   misc/interp.py file.il
#
# The exit status is main's return value, as it would be for a native program.

import sys

TYPES = {"i8", "u8", "i16", "u16", "i32", "i64", "f32", "f64"}

# The first character of an operand says what it is, except for integer
# constants.
OPERAND_KINDS = {"%": "ins", ":": "block", "$": "slot", "@": "sym"}


class Error(Exception):
    pass


class Ins:

    def __init__(self, result, op, ty, operands):
        self.result = result  # the n of %n, or None
        self.op = op
        self.ty = ty  # None for void
        self.operands = operands  # (kind, value) pairs


class Func:

    def __init__(self, name, ret):
        self.name = name
        self.ret = ret
        self.slots = []  # (type, name or None) by slot number
        self.ins = []
        # Block number -> index in ins of the block's first instruction.
        self.blocks = {}


def parse_operand(s):
    if s[0] in OPERAND_KINDS:
        v = s[1:]
        return (OPERAND_KINDS[s[0]], v if s[0] == "@" else int(v))
    if s[0].isdigit() or s[0] == "-":
        return ("int", int(s))
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


def parse(text):
    funcs = {}
    f = None
    for lineno, raw in enumerate(text.split("\n"), 1):
        line = raw.strip()
        if not line or line.startswith(";"):
            continue
        try:
            if line.startswith("func "):
                # func i64 @main, or func @f when it returns nothing.
                words = line.split()
                ret = words[1] if len(words) == 3 else None
                f = Func(words[-1][1:], ret)
                funcs[f.name] = f
            elif f is None:
                raise Error("expected 'func'")
            elif line.startswith("slot "):
                # slot $0 i64 x, or slot $1 i32 for an unnamed temporary.
                words = line.split()
                f.slots.append((words[2], words[3] if len(words) > 3 else None))
            elif line.startswith(":"):
                f.blocks[int(line[1:])] = len(f.ins)
            else:
                f.ins.append(parse_ins(line))
        except (Error, ValueError, IndexError) as e:
            raise Error(f"line {lineno}: {e}: {raw}")
    return funcs


def value_of(values, operand):
    kind, v = operand
    if kind == "int":
        return v
    if kind == "ins":
        return values[v]
    raise Error(f"can't use a {kind} as a value")


def run(f):
    values = {}
    pc = 0
    while pc < len(f.ins):
        ins = f.ins[pc]
        pc += 1
        if ins.op == "RET":
            return value_of(values, ins.operands[0]) if ins.operands else None
        raise Error(f"{f.name}: {ins.op} isn't supported yet")
    raise Error(f"{f.name}: ran off the end")


def main():
    text = open(sys.argv[1]).read() if len(sys.argv) > 1 else sys.stdin.read()
    try:
        funcs = parse(text)
        if "main" not in funcs:
            raise Error("no main")
        ret = run(funcs["main"])
    except Error as e:
        sys.exit(f"interp: {e}")
    sys.exit((ret or 0) & 0xFF)


if __name__ == "__main__":
    main()
