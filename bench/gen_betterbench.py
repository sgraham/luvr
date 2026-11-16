# Generates a large luv file that looks more like real code than dumbbench.luv:
# structs with fields and methods, functions with typed parameters, calls,
# arithmetic across the integer and float types, casts, constants, strings,
# lists, dicts, comprehensions, and nested control flow.
#
# Usage: gen_betterbench.py NFUNCS [SEED] > out.luv
#
# The inputs used so far are:
#
#   betterbench.luv  150000 (195 MB, about 2.5 minutes to generate)
#
# The code is type correct: there are no implicit numeric conversions,
# constants are in range of their type, and there are no constant zero
# divisors. That includes checks the parser doesn't do yet, so that the file
# stays valid as checking is added. Constant subexpressions are evaluated here
# so that their folded values are known to fit.
#
# Most constants are small, most operands are names, and most expressions are
# shallow, as in real code. The rarer paths (large constants, typed literals,
# shifts, division) are present but uncommon.
import random, sys

nfuncs = int(sys.argv[1])
rnd = random.Random(int(sys.argv[2]) if len(sys.argv) > 2 else 1)

INT_RANGE = {
    "i8": (-(1 << 7), (1 << 7) - 1),
    "i16": (-(1 << 15), (1 << 15) - 1),
    "i32": (-(1 << 31), (1 << 31) - 1),
    "int": (-(1 << 63), (1 << 63) - 1),
    "u8": (0, (1 << 8) - 1),
    "u16": (0, (1 << 16) - 1),
    "u32": (0, (1 << 32) - 1),
    "u64": (0, (1 << 64) - 1),
}
INT_BITS = {"i8": 8, "i16": 16, "i32": 32, "int": 64, "u8": 8, "u16": 16, "u32": 32, "u64": 64}
SUFFIX = {"i8": "i8", "i16": "i16", "i32": "i32", "int": "i64", "u8": "u8", "u16": "u16",
          "u32": "u32", "u64": "u64", "f32": "f32", "f64": "f64"}
FLOATS = ("f32", "f64")
RESERVED = set(INT_BITS) | {"i64", "f32", "f64"}
NUMERIC = tuple(INT_RANGE) + FLOATS

# How often each type is chosen for variables, parameters and returns.
VAR_TYPES = [("int", 40), ("i32", 8), ("u8", 6), ("u32", 5), ("u64", 5), ("i16", 2),
             ("u16", 2), ("i8", 1), ("f64", 8), ("f32", 4), ("bool", 8), ("str", 8)]

WORDS = ("count total index offset size width height value result sum acc base "
         "limit start end pos step delta flags mask code kind level depth score cost "
         "weight rate ratio scale factor mid lo hi key slot idx num item node entry "
         "name label text path buf data head tail left right first last prev next cur "
         "tmp old new min max avg fill pad gap shift bits byte word id seq tick time").split()
VERBS = ("parse compute update check find build make get set read write load store "
         "scan merge split apply reset clamp hash encode decode emit visit walk sort "
         "count measure resolve lookup insert remove format render pack unpack").split()
NOUNS = ("header block token node entry record buffer table item value range frame "
         "packet chunk segment field row column cell page index score state config "
         "layout shape path name key slot queue stack tree graph edge").split()


def pick_weighted(choices):
    total = sum(w for _, w in choices)
    r = rnd.uniform(0, total)
    for c, w in choices:
        r -= w
        if r <= 0:
            return c
    return choices[-1][0]


def var_type():
    return pick_weighted(VAR_TYPES)


def is_int(t):
    return t in INT_RANGE


def is_num(t):
    return t in NUMERIC


def fits(v, t):
    if is_int(t):
        lo, hi = INT_RANGE[t]
        return isinstance(v, int) and lo <= v <= hi
    if t == "f32":
        if isinstance(v, int):
            return abs(v) < (1 << 24)
        return abs(v) < 3.4e38
    if t == "f64":
        return not isinstance(v, int) or abs(v) < (1 << 53)
    return True


def trunc_div(a, b):
    q = abs(a) // abs(b)
    return q if (a < 0) == (b < 0) else -q


def trunc_mod(a, b):
    return a - b * trunc_div(a, b)


# An expression: its text, type, priority (for parenthesizing), and its
# constant value, or None when it isn't constant. Untyped constants have type
# "untyped_int" or "untyped_float" until they meet a typed operand.
class E:
    def __init__(self, text, ty, prio=12, const=None):
        self.text, self.ty, self.prio, self.const = text, ty, prio, const


PRIO = {"or": 1, "and": 2, "==": 4, "!=": 4, "<": 4, "<=": 4, ">": 4, ">=": 4,
        "|": 5, "^": 6, "&": 7, "<<": 8, ">>": 8, "+": 9, "-": 9, "*": 10, "/": 10, "%": 10}


def paren(e, need):
    return f"({e.text})" if e.prio < need else e.text


class Struct:
    def __init__(self, name, fields, methods):
        self.name, self.fields, self.methods = name, fields, methods


class Func:
    def __init__(self, name, params, ret):
        self.name, self.params, self.ret = name, params, ret


structs = []
funcs = []
consts = []  # (name, value), all untyped ints from 1 to 100
globals_ = []  # (name, type), read but not written by functions


class Scope:
    def __init__(self, names=None):
        self.vars = list(names or [])  # (name, type)
        self.used = set(n for n, _ in self.vars)
        # Parameters, self and loop variables can't be assigned.
        self.ro = set(n for n, _ in self.vars)
        self.n = 0

    def fresh(self, base=None):
        while True:
            self.n += 1
            name = f"{base or rnd.choice(WORDS)}{self.n}"
            if name not in self.used and name not in RESERVED:
                self.used.add(name)
                return name

    def of_type(self, t):
        return [n for n, vt in self.vars if vt == t]


def small_int(t):
    # Mostly 0, 1, 2 and small values, sometimes masks and larger values.
    r = rnd.random()
    if r < 0.45:
        v = rnd.choice((0, 1, 1, 1, 2, 2, 3, 4))
    elif r < 0.85:
        v = rnd.randint(5, 100)
    elif r < 0.98:
        v = rnd.choice((0xff, 0xffff, 255, 256, 1000, 1024, 4096, 65535, 100000))
    else:
        v = rnd.choice((1 << 20, 1 << 31, 0x7fff_ffff, 10 ** 9, 1 << 40, 0xdead_beef))
    if is_int(t) and not fits(v, t):
        v = rnd.randint(0, min(100, INT_RANGE[t][1]))
    return v


def int_literal(v):
    if v >= 256 and rnd.random() < 0.4:
        h = f"{v:x}"
        if len(h) > 4 and rnd.random() < 0.5:
            parts = []
            while h:
                parts.insert(0, h[-4:])
                h = h[:-4]
            h = "_".join(parts)
        return "0x" + h
    return str(v)


def const_leaf(t):
    if is_int(t):
        v = small_int(t)
        if rnd.random() < 0.01:
            return E(f"{v}{SUFFIX[t]}", t, 12, v)
        return E(int_literal(v), "untyped_int", 12, v)
    if t in FLOATS:
        r = rnd.random()
        if r < 0.3:
            v = small_int(t)
            v = v if v < (1 << 24) else 2
            return E(str(v), "untyped_int", 12, v)
        v = rnd.choice((0.5, 1.0, 2.0, 0.25, 1.5, 3.14159, 0.001, 1e-6, 100.0, 1e3, 2.5e4))
        if rnd.random() < 0.01:
            return E(f"{v}{SUFFIX[t]}", t, 12, v)
        return E(repr(v), "untyped_float", 12, v)
    if t == "bool":
        v = rnd.random() < 0.5
        return E("true" if v else "false", "bool", 12, v)
    words = " ".join(rnd.choice(WORDS) for _ in range(rnd.randint(1, 4)))
    return E(f'"{words}"', "str", 12)


def pointer_arg(t, sc, depth):
    s = t[1:]
    locals_ = [n for n, vt in sc.vars if vt == s and n not in sc.ro]
    if locals_:
        return "&" + rnd.choice(locals_)
    # The address of a parameter can't be taken, but a struct literal's can.
    return f"&{s}()"


def call_args(params, sc, depth):
    return ", ".join(pointer_arg(t, sc, depth) if t.startswith("*") else
                     expr(t, sc, depth + 1).text for _, t in params)


def leaf(t, sc, depth):
    options = []
    names = sc.of_type(t)
    if names:
        options.append(("var", 10))
    options.append(("const", 4))
    rets = [f for f in funcs if f.ret == t]
    if rets and depth < 3:
        options.append(("call", 3))
    fields = [(n, s, f) for n, vt in sc.vars for s in structs
              if vt == s.name or vt == "*" + s.name for f, ft, _ in s.fields if ft == t]
    if fields:
        options.append(("field", 3))
    meths = [(n, s, m) for n, vt in sc.vars for s in structs if vt == s.name
             for m in s.methods if m.ret == t]
    if meths and depth < 3:
        options.append(("method", 1))
    if t == "int" and depth < 3:
        lists = [n for n, vt in sc.vars if vt.startswith("[]")]
        if lists:
            options.append(("len", 1))
    if t in ("int", "f64", "str"):
        lists = sc.of_type("[]" + t)
        if lists and depth < 3:
            options.append(("index", 2))
    if t == "int" and consts:
        options.append(("named", 1))
    if t == "int":
        options.append(("sizeof", 0.3))
    others = [n for n, vt in sc.vars if is_num(vt) and vt != t]
    if is_num(t) and others and depth < 3:
        options.append(("cast", 1))
    kind = pick_weighted(options)
    if kind == "var":
        return E(rnd.choice(names), t)
    if kind == "call":
        f = rnd.choice(rets)
        return E(f"{f.name}({call_args(f.params, sc, depth)})", t)
    if kind == "field":
        n, _, f = rnd.choice(fields)
        return E(f"{n}.{f}", t)
    if kind == "method":
        n, _, m = rnd.choice(meths)
        return E(f"{n}.{m.name}({call_args(m.params, sc, depth)})", t)
    if kind == "len":
        lists = [n for n, vt in sc.vars if vt.startswith("[]")]
        return E(f"len({rnd.choice(lists)})", t)
    if kind == "index":
        lst = rnd.choice(sc.of_type("[]" + t))
        return E(f"{lst}[{expr('int', sc, depth + 1).text}]", t)
    if kind == "named":
        name, v = rnd.choice(consts)
        return E(name, "untyped_int", 12, v)
    if kind == "sizeof":
        return E(f"sizeof({rnd.choice(NUMERIC)})", t)
    if kind == "cast":
        return E(f"cast {t}({rnd.choice(others)})", t)
    return const_leaf(t)


def result_type(a, b, t):
    if a.ty.startswith("untyped") and b.ty.startswith("untyped"):
        return "untyped_float" if "untyped_float" in (a.ty, b.ty) else "untyped_int"
    return t


# Folds constants with the same rules as the parser, and returns None if the
# result is an error, so the caller tries again.
def fold(op, a, b, rt, t):
    x, y = a.const, b.const
    if op in ("/", "%") and y == 0:
        return None
    if x is None or y is None:
        return "nonconst"
    if isinstance(x, float) or isinstance(y, float):
        v = {"+": x + y, "-": x - y, "*": x * y, "/": x / y if y else None}[op]
    elif op == "+":
        v = x + y
    elif op == "-":
        v = x - y
    elif op == "*":
        v = x * y
    elif op == "/":
        v = trunc_div(x, y)
    elif op == "%":
        v = trunc_mod(x, y)
    elif op == "&":
        v = x & y
    elif op == "|":
        v = x | y
    elif op == "^":
        v = x ^ y
    elif op == "<<":
        v = x << y
    else:
        v = x >> y
    if v is None or (isinstance(v, int) and abs(v) >= 1 << 120):
        return None
    if not fits(v, t):
        return None
    return v


def numeric_expr(t, sc, depth):
    if depth >= 3 or rnd.random() < 0.45 + depth * 0.2:
        e = leaf(t, sc, depth)
        if e.const is not None and not fits(e.const, t):
            return leaf(t, sc, 3) if sc.of_type(t) else const_leaf(t)
        return e
    r = rnd.random()
    if r < 0.06 and (t in FLOATS or t.startswith("i") or t == "int"):
        e = numeric_expr(t, sc, depth + 1)
        if e.const is not None and not fits(-e.const, t):
            return e
        inner = f"({e.text})" if e.prio < 12 or e.text.startswith("-") else e.text
        return E(f"-{inner}", e.ty, 11, None if e.const is None else -e.const)
    if r < 0.09 and is_int(t):
        e = numeric_expr(t, sc, depth + 1)
        if e.const is not None:
            return e
        inner = f"({e.text})" if e.prio < 12 else e.text
        return E(f"~{inner}", e.ty, 11)
    if r < 0.12:
        e = numeric_expr(t, sc, depth + 1)
        return E(f"({e.text})", e.ty, 12, e.const)
    if t in FLOATS:
        op = pick_weighted([("+", 5), ("-", 3), ("*", 5), ("/", 2)])
    else:
        op = pick_weighted([("+", 10), ("-", 6), ("*", 6), ("/", 2), ("%", 2), ("&", 2),
                            ("|", 1), ("^", 1), ("<<", 1), (">>", 1)])
    for _ in range(4):
        a = numeric_expr(t, sc, depth + 1)
        if op in ("<<", ">>"):
            # The count is a small constant or a variable of any integer type.
            ints = [n for n, vt in sc.vars if is_int(vt)]
            if ints and rnd.random() < 0.3:
                b = E(rnd.choice(ints), "int")
            else:
                c = rnd.randint(0, min(INT_BITS[t] - 1, 12))
                b = E(str(c), "untyped_int", 12, c)
        elif op in ("/", "%") and rnd.random() < 0.7:
            c = rnd.choice((2, 2, 4, 8, 10, 16, 100, 1000))
            if not fits(c, t):
                c = 2
            b = E(str(c), "untyped_int", 12, c)
        elif a.const is not None and sc.of_type(t) and rnd.random() < 0.9:
            # Constants are mostly combined with names, not other constants.
            b = E(rnd.choice(sc.of_type(t)), t)
        else:
            b = numeric_expr(t, sc, depth + 1)
        if op in ("/", "%") and b.const is None and not sc.of_type(t):
            continue
        if a.ty.startswith("untyped") and not b.ty.startswith("untyped") and op in ("<<", ">>"):
            # An untyped value shifted by a variable stays untyped.
            continue
        rt = result_type(a, b, t) if op not in ("<<", ">>") else a.ty
        if rt == "untyped_float" and is_int(t):
            continue
        v = fold(op, a, b, rt, t)
        if v is None:
            continue
        p = PRIO[op]
        left = paren(a, p)
        right = paren(b, p + 1)
        return E(f"{left} {op} {right}", rt, p, None if v == "nonconst" else v)
    return leaf(t, sc, 3) if sc.of_type(t) else const_leaf(t)


def bool_expr(sc, depth):
    r = rnd.random()
    if depth >= 3 or r < 0.2:
        names = sc.of_type("bool")
        if names and rnd.random() < 0.7:
            return E(rnd.choice(names), "bool")
        return leaf("bool", sc, depth)
    if r < 0.65:
        t = rnd.choice([vt for _, vt in sc.vars if is_num(vt) or vt == "str"] or ["int"])
        a = expr(t, sc, depth + 1)
        b = expr(t, sc, depth + 1)
        ops = ("==", "!=") if t == "str" else ("==", "!=", "<", "<=", ">", ">=")
        op = rnd.choice(ops)
        return E(f"{paren(a, 5)} {op} {paren(b, 5)}", "bool", 4)
    if r < 0.7:
        e = bool_expr(sc, depth + 1)
        return E(f"not {paren(e, 4)}", "bool", 3)
    lists = sc.of_type("[]int")
    if r < 0.75 and lists:
        e = expr("int", sc, depth + 1)
        op = rnd.choice(("in", "in", "not in"))
        return E(f"{paren(e, 5)} {op} {rnd.choice(lists)}", "bool", 4)
    op = rnd.choice(("and", "or"))
    a = bool_expr(sc, depth + 1)
    b = bool_expr(sc, depth + 1)
    p = PRIO[op]
    return E(f"{paren(a, p)} {op} {paren(b, p + 1)}", "bool", p)


def str_expr(sc, depth):
    r = rnd.random()
    if depth >= 2 or r < 0.5:
        return leaf("str", sc, depth)
    if r < 0.8:
        parts = []
        for _ in range(rnd.randint(1, 3)):
            parts.append(rnd.choice(WORDS))
            vs = [n for n, vt in sc.vars if is_num(vt) or vt in ("str", "bool")]
            if vs:
                parts.append(f"\\({rnd.choice(vs)})")
        return E('"' + " ".join(parts) + '"', "str")
    a = str_expr(sc, depth + 1)
    b = str_expr(sc, depth + 1)
    return E(f"{paren(a, 9)} + {paren(b, 10)}", "str", 9)


def expr(t, sc, depth=0):
    if t == "bool":
        return bool_expr(sc, depth)
    if t == "str":
        return str_expr(sc, depth)
    if is_num(t):
        return numeric_expr(t, sc, depth)
    for s in structs:
        if s.name == t:
            if sc.of_type(t) and rnd.random() < 0.5:
                return E(rnd.choice(sc.of_type(t)), t)
            inits = [f".{f}={expr(ft, sc, depth + 1).text}" for f, ft, _ in s.fields
                     if rnd.random() < 0.6]
            return E(f"{t}({', '.join(inits)})", t)
    raise ValueError(t)


def default_type(e, t):
    if e.ty == "untyped_int":
        return "int"
    if e.ty == "untyped_float":
        return "f64"
    return t


class Body:
    def __init__(self, sc, ret, indent):
        self.sc, self.ret, self.out = sc, ret, []
        self.indent = indent

    def line(self, s, ind):
        self.out.append("    " * ind + s + "\n")


def gen_block(b, ind, n, depth, in_loop):
    sc = b.sc
    saved = len(sc.vars)
    for i in range(n):
        last = i == n - 1
        gen_stat(b, ind, depth, in_loop, last)
    # Names declared in a block aren't used after it, even though locals are
    # function scoped, so that blocks can be skipped.
    del sc.vars[saved:]


def gen_stat(b, ind, depth, in_loop, last):
    sc = b.sc
    choices = [("decl", 10), ("infer", 6), ("assign", 4), ("aug", 6), ("call", 4)]
    if depth < 3:
        choices += [("if", 5), ("for", 3)]
        if any(vt.startswith("[]") for _, vt in sc.vars):
            choices.append(("forlist", 2))
        if any(vt.startswith("{") for _, vt in sc.vars):
            choices.append(("fordict", 1))
    choices += [("print", 1), ("list", 2), ("dict", 1), ("compr", 1), ("struct", 2),
                ("fieldset", 2), ("check", 0.3)]
    if depth == 0:
        choices.append(("nested", 0.3))
    if in_loop and last:
        choices.append(("break", 1))
    if last and depth > 0:
        choices.append(("pass", 0.2))
    kind = pick_weighted(choices)
    if kind == "decl":
        t = var_type()
        name = sc.fresh()
        if rnd.random() < 0.15 and t not in ("bool", "str"):
            b.line(f"{t} {name}", ind)
        else:
            b.line(f"{t} {name} = {expr(t, sc).text}", ind)
        sc.vars.append((name, t))
    elif kind == "infer":
        t = var_type()
        e = expr(t, sc)
        if e.ty == "untyped_int" and e.const is not None and not fits(e.const, "int"):
            e = const_leaf("int")
        name = sc.fresh()
        b.line(f"{name} = {e.text}", ind)
        sc.vars.append((name, default_type(e, t)))
    elif kind == "assign":
        targets = [(n, vt) for n, vt in sc.vars
                   if (is_num(vt) or vt in ("bool", "str")) and n not in sc.ro]
        if not targets:
            return gen_stat(b, ind, depth, in_loop, last)
        n, t = rnd.choice(targets)
        b.line(f"{n} = {expr(t, sc).text}", ind)
    elif kind == "aug":
        targets = [(n, vt) for n, vt in sc.vars if is_num(vt) and n not in sc.ro]
        if not targets:
            return gen_stat(b, ind, depth, in_loop, last)
        n, t = rnd.choice(targets)
        if t in FLOATS:
            op = rnd.choice(("+=", "+=", "-=", "*=", "/="))
        else:
            op = pick_weighted([("+=", 10), ("-=", 4), ("*=", 2), ("|=", 1), ("&=", 1),
                                ("^=", 0.5), ("<<=", 0.5), (">>=", 0.5)])
        if op in ("<<=", ">>="):
            rhs = str(rnd.randint(1, min(7, INT_BITS[t] - 1)))
        elif op == "/=":
            rhs = rnd.choice(("2.0", "10.0", "4")) if t in FLOATS else rnd.choice(("2", "10", "4"))
        else:
            e = expr(t, sc)
            if e.const is not None and op in ("*=",) and e.const == 0:
                e = const_leaf(t)
            rhs = e.text
        b.line(f"{n} {op} {rhs}", ind)
    elif kind == "call":
        f = rnd.choice(funcs) if funcs else None
        if f is None or f.ret is not None:
            voids = [f for f in funcs if f.ret is None]
            if not voids:
                return gen_stat(b, ind, depth, in_loop, last)
            f = rnd.choice(voids)
        b.line(f"{f.name}({call_args(f.params, sc, 0)})", ind)
    elif kind == "if":
        b.line(f"if {bool_expr(sc, 0).text}:", ind)
        gen_block(b, ind + 1, rnd.randint(1, 4), depth + 1, in_loop)
        for _ in range(rnd.choice((0, 0, 0, 1, 1, 2))):
            b.line(f"elif {bool_expr(sc, 0).text}:", ind)
            gen_block(b, ind + 1, rnd.randint(1, 3), depth + 1, in_loop)
        if rnd.random() < 0.4:
            b.line("else:", ind)
            gen_block(b, ind + 1, rnd.randint(1, 3), depth + 1, in_loop)
    elif kind == "for":
        v = sc.fresh("i")
        n = rnd.choice(("10", "100", str(rnd.randint(2, 64))))
        ints = sc.of_type("int")
        if ints and rnd.random() < 0.5:
            n = rnd.choice(ints)
        b.line(f"for {v} in range({n}):", ind)
        sc.vars.append((v, "int"))
        sc.ro.add(v)
        gen_block(b, ind + 1, rnd.randint(1, 4), depth + 1, True)
        sc.vars.remove((v, "int"))
    elif kind == "forlist":
        lst, lt = rnd.choice([(n, vt) for n, vt in sc.vars if vt.startswith("[]")])
        v = sc.fresh("it")
        b.line(f"for {v} in {lst}:", ind)
        sc.vars.append((v, lt[2:]))
        sc.ro.add(v)
        gen_block(b, ind + 1, rnd.randint(1, 3), depth + 1, True)
        sc.vars.remove((v, lt[2:]))
    elif kind == "fordict":
        d, dt = rnd.choice([(n, vt) for n, vt in sc.vars if vt.startswith("{")])
        kt, vt = dt[1:].split("}")
        k, v = sc.fresh("k"), sc.fresh("v")
        b.line(f"for {k}, {v} in {d}:", ind)
        sc.vars += [(k, kt), (v, vt)]
        sc.ro |= {k, v}
        gen_block(b, ind + 1, rnd.randint(1, 3), depth + 1, True)
        sc.vars.remove((k, kt))
        sc.vars.remove((v, vt))
    elif kind == "print":
        b.line(f"print {str_expr(sc, 0).text}", ind)
    elif kind == "list" and sc.of_type("[]int") and rnd.random() < 0.2:
        name = sc.fresh("part")
        lo = rnd.choice(("1", "", "2"))
        hi = rnd.choice(("", "-1", "4"))
        b.line(f"{name} = {rnd.choice(sc.of_type('[]int'))}[{lo}:{hi}]", ind)
        sc.vars.append((name, "[]int"))
    elif kind == "list":
        t = rnd.choice(("int", "int", "f64", "str"))
        name = sc.fresh("items")
        elems = ", ".join(expr(t, sc, 1).text for _ in range(rnd.randint(0, 6)))
        b.line(f"[]{t} {name} = [{elems}]", ind)
        sc.vars.append((name, "[]" + t))
    elif kind == "dict":
        vt = rnd.choice(("int", "str", "f64"))
        name = sc.fresh("table")
        elems = ", ".join(f'"{rnd.choice(WORDS)}{i}": {expr(vt, sc, 1).text}'
                          for i in range(rnd.randint(1, 4)))
        b.line(f"{{str}}{vt} {name} = {{{elems}}}", ind)
        sc.vars.append((name, "{str}" + vt))
    elif kind == "compr":
        lists = sc.of_type("[]int")
        v = sc.fresh("x")
        name = sc.fresh("picked")
        src = rnd.choice(lists) if lists else f"range({rnd.randint(2, 50)})"
        inner = Scope(sc.vars + [(v, "int")])
        inner.used, inner.n = sc.used, sc.n
        body = expr("int", inner, 1).text
        cond = f" if {bool_expr(inner, 1).text}" if rnd.random() < 0.5 else ""
        sc.n = inner.n
        b.line(f"{name} = [{body} for {v} in {src}{cond}]", ind)
        sc.vars.append((name, "[]int"))
    elif kind == "struct":
        if not structs:
            return gen_stat(b, ind, depth, in_loop, last)
        s = rnd.choice(structs[-8:])
        name = sc.fresh(s.name.lower()[:6])
        if rnd.random() < 0.3:
            b.line(f"{s.name} {name}", ind)
        else:
            b.line(f"{name} = {expr(s.name, sc).text}", ind)
        sc.vars.append((name, s.name))
    elif kind == "fieldset":
        sv = [(n, s) for n, vt in sc.vars for s in structs if vt == s.name and n not in sc.ro]
        if not sv:
            return gen_stat(b, ind, depth, in_loop, last)
        n, s = rnd.choice(sv)
        f, ft, _ = rnd.choice(s.fields)
        b.line(f"{n}.{f} = {expr(ft, sc).text}", ind)
    elif kind == "check":
        b.line(f"check {bool_expr(sc, 0).text}", ind)
    elif kind == "nested":
        captured = [(n, vt) for n, vt in sc.vars if vt == "int"]
        name = sc.fresh("helper")
        b.line(f"def int {name}(int z):", ind)
        inner = Scope(captured + [("z", "int")])
        b.line(f"return {expr('int', inner, 1).text}", ind + 1)
        b.line(f"{sc.fresh('r')} = {name}({expr('int', sc, 1).text})", ind)
    elif kind == "pass":
        b.line("pass", ind)
    elif kind == "break":
        b.line(rnd.choice(("break", "continue")), ind)


def gen_params(sc):
    params = []
    for _ in range(pick_weighted([(0, 2), (1, 4), (2, 4), (3, 2), (4, 1)])):
        t = var_type()
        if structs and rnd.random() < 0.1:
            t = rnd.choice(structs[-8:]).name
            if rnd.random() < 0.5:
                t = "*" + t
        params.append((sc.fresh(), t))
    return params


def ret_type():
    r = rnd.random()
    if r < 0.15:
        return None
    if r < 0.2 and structs:
        return rnd.choice(structs[-8:]).name
    return var_type()


def gen_func(name, params, ret, sc, prefix, receiver=None):
    out = []
    sig = ", ".join(([receiver] if receiver else []) + [f"{t} {n}" for n, t in params])
    out.append(f"{prefix}def {ret + ' ' if ret else ''}{name}({sig}):\n")
    b = Body(sc, ret, 1)
    gen_block(b, 1, rnd.randint(2, 10), 0, False)
    out += b.out
    if ret:
        out.append(f"    return {expr(ret, sc).text}\n")
    out.append("\n")
    return out


def gen_struct(idx):
    name = rnd.choice(NOUNS).capitalize() + str(idx)
    fields = []
    used = set()
    for _ in range(rnd.randint(2, 6)):
        f = rnd.choice(WORDS)
        while f in used:
            f = rnd.choice(WORDS) + str(len(used))
        used.add(f)
        t = var_type()
        default = None
        if rnd.random() < 0.25:
            default = const_leaf(t).text
        fields.append((f, t, default))
    out = [f"struct {name}:\n"]
    for f, t, d in fields:
        out.append(f"    {t} {f}" + (f" = {d}" if d else "") + "\n")
    out.append("\n")
    s = Struct(name, fields, [])
    structs.append(s)
    for _ in range(rnd.randint(0, 3)):
        sc = Scope()
        sc.used.add("self")
        params = gen_params(sc)
        ret = ret_type()
        mname = f"{rnd.choice(VERBS)}_{rnd.choice(WORDS)}"
        while any(m.name == mname for m in s.methods):
            mname += "2"
        sc.vars = [(n, t) for n, t in params] + [("self", name)]
        sc.ro = set(n for n, _ in sc.vars)
        out += gen_func(mname, params, ret, sc, f"on {name} ", receiver="self")
        s.methods.append(Func(mname, params, ret))
    return out


out = []
for i in range(nfuncs):
    if i % 12 == 0:
        out += gen_struct(i)
    if i % 30 == 0:
        cname = f"{rnd.choice(WORDS).upper()}_{rnd.choice(NOUNS).upper()}_{i}"
        v = rnd.choice((4, 8, 16, 32, 64, 100))
        out.append(f"{cname} = {v}\n\n")
        consts.append((cname, v))
    if i % 25 == 0:
        t = rnd.choice(("int", "int", "u32", "f64", "bool", "str"))
        gname = f"{rnd.choice(WORDS)}_{rnd.choice(NOUNS)}_{i}"
        out.append(f"{t} {gname} = {const_leaf(t).text}\n\n")
        globals_.append((gname, t))
    sc = Scope()
    params = gen_params(sc)
    sc.vars = list(params) + globals_
    sc.ro = set(n for n, _ in sc.vars)
    ret = ret_type()
    name = f"{rnd.choice(VERBS)}_{rnd.choice(NOUNS)}_{i}"
    out += gen_func(name, params, ret, sc, "")
    funcs.append(Func(name, params, ret))
    # Calls go to recent functions, as they mostly would in a real file.
    if len(funcs) > 64:
        funcs.pop(0)
    if len(structs) > 16:
        structs.pop(0)
    if len(consts) > 16:
        consts.pop(0)
    if len(globals_) > 8:
        globals_.pop(0)
sys.stdout.write("".join(out))
