# Generates a luv file whose functions reference earlier globals (functions and
# global vars), to compare global table implementations.
#
# Usage: gen_globals.py NFUNCS LINES_PER_FUNC {random|near} > out.luv
#
# Each body line references two globals. 'random' picks them uniformly from all
# earlier globals, 'near' mostly from the last 64. The inputs used so far are:
#
#   gref_rand.luv   300000 4 random
#   gref_near.luv   300000 4 near
#   gref_small.luv  2000 800 random
import random, string, sys

nfuncs, nlines, mode = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
rnd = random.Random(1)


def name():
    return "".join(rnd.choice(string.ascii_lowercase) for _ in range(rnd.randint(6, 14)))


funcs, gvars = [], []


def pick(lst):
    if mode == "near" and rnd.random() < 0.9:
        return lst[rnd.randint(max(0, len(lst) - 64), len(lst) - 1)]
    return rnd.choice(lst)


out = []
for f in range(nfuncs):
    if f % 8 == 0:
        g = f"{name()}_{f}"
        out.append(f"int {g} = {rnd.randint(1, 99)}\n")
        gvars.append(g)
    fn = f"{name()}_{f}"
    out.append(f"def int {fn}(int a):\n    int x = a\n")
    for _ in range(nlines):
        if funcs:
            out.append(f"    x += {pick(funcs)}(a) * {pick(gvars)}\n")
        else:
            out.append(f"    x += {pick(gvars)}\n")
    out.append("    return x\n\n")
    funcs.append(fn)
sys.stdout.write("".join(out))
