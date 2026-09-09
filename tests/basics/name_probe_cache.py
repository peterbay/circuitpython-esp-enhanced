# Names at module level, in a class body and in exec, and attributes of
# modules and of builtin types, all answered from the interpreter's lookup
# caches. Each case rebinds, deletes, shadows or recreates something after the
# cache has seen it. Printed so that CPython produces the same lines.
import sys

print("--- module-level names rebound in a loop, then deleted and recreated")
total = 0
for i in range(5):
    total = total + i
print(total)
del total
try:
    total
except NameError:
    print("NameError")
total = "new"
print(total)

print("--- a builtin shadowed and unshadowed at module level")
out = []
for i in range(3):
    out.append(len("abc"))
len = lambda x: 42
for i in range(2):
    out.append(len("abc"))
del len
out.append(len("abcd"))
print(out)

print("--- a function rebinding a module name through global while the module reads it")
counter = 0


def bump():
    global counter
    counter += 10


for i in range(3):
    counter = counter + 1
    bump()
print(counter)


def make_new():
    global fresh
    fresh = "made in a function"


try:
    fresh
except NameError:
    print("NameError before")
make_new()
print(fresh)

print("--- names in a class body: locals of the body, module globals, builtins")
scale = 3


class Table:
    scale = scale * 2
    size = len("four")
    rows = []
    for k in range(3):
        rows.append(k * scale)
    scale = scale + 1


print(Table.scale, Table.size, Table.rows, scale)

print("--- exec with its own locals and globals")
g = {"base": 100}
l = {}
exec("x = base + 1\nx = x + 1\nbase = 5\ny = base", g, l)
print(sorted(g.keys()) == ["__builtins__", "base"] or sorted(g.keys()) == ["base"], l["x"], l["y"], g["base"])
exec("z = len('ab')\nz = z * 10", g)
print(g["z"])
for i in range(3):
    d = {"n": i}
    exec("n = n * 2\nm = n + 1", d)
    print(d["n"], d["m"], end=" ")
print()

print("--- module attributes: read, rebound, deleted, and module __getattr__")
import name_probe_cache_mod as mod

for i in range(3):
    print(mod.value, mod.twice(i), mod.table[1], end=" ")
    mod.value = mod.value + 1
print()
setattr(mod, "value", "set")
print(mod.value)
del mod.value
try:
    mod.value
except AttributeError:
    print("AttributeError")
mod.value = "back"
print(mod.value, mod.not_there, mod.twice(21))
print(type(mod).__name__, mod.__class__ is type(sys), sys.__class__ is type(mod))
print(sys.maxsize > 0, "value" in mod.__dict__)

print("--- methods of builtin types and of literals, called through the cache")
items = []
for i in range(4):
    items.append(i)
    "a,b".split(",")
    "x{}y".format(i)
print(items, ", ".join(["a", "b"]), "a,b,c".split(","), {"k": 1}.get("k"), {"k": 1}.get("z", 9))
s = "".join(["he", "ap"])
print(s.upper(), s.startswith("he"), b"xyz".find(b"y"), bytearray(b"ab").upper())
d = {"a": 1}
for i in range(3):
    d.update({"b": i})
print(sorted(d.items()))
print([].__class__ is list, "".__class__ is str)
print("hotovo")
