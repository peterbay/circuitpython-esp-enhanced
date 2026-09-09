# MicroPython keeps the dict handed to type() as the class's own attribute
# table rather than copying it as CPython does, so changing that dict changes
# the class. The class lookup cache has to see every such change: a value
# replaced in place, a name added, a name removed. Expected output is
# MicroPython's, in the .exp file.
methods = {"f": lambda self: "f1", "value": 1}
T = type("T", (), methods)
t = T()
print(t.f(), t.value)
for _ in range(3):
    t.f()
methods["f"] = lambda self: "f2"
print(t.f())
methods["g"] = lambda self: "g"
print(t.g())
del methods["value"]
try:
    t.value
except AttributeError:
    print("AttributeError value")
methods["value"] = 2
print(t.value)
