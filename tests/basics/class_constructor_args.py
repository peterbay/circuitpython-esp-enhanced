# SPDX-FileCopyrightText: Copyright (c) 2026 CircuitPython contributors
# SPDX-License-Identifier: MIT
# Temporary argument arrays for Python __new__ and __init__ must remain live
# through collection, nested calls and exception unwinding.

import gc

try:
    object.__new__
except AttributeError:
    print("SKIP")
    raise SystemExit


class Plain:
    pass


class InitOnly:
    def __init__(self, a=0, b=1, *, c=2):
        self.values = (a, b, c)


class Both:
    def __new__(cls, *args, **kwargs):
        gc.collect()
        instance = object.__new__(cls)
        instance.new_values = (args, sorted(kwargs.items()))
        return instance

    def __init__(self, *args, **kwargs):
        gc.collect()
        self.init_values = (args, sorted(kwargs.items()))


print("plain", isinstance(Plain(), Plain))
print("init", InitOnly().values, InitOnly(3).values, InitOnly(c=4).values)
print("mixed", InitOnly(3, 4, c=5).values, InitOnly(b=7, a=6).values)
for count in (0, 1, 2, 7, 8, 15, 16, 31, 64):
    positional = tuple([n, n + 1] for n in range(count))
    keywords = {"k" + str(n): [n + 2] for n in range(count)}
    instance = Both(*positional, **keywords)
    gc.collect()
    expected = (positional, sorted(keywords.items()))
    print("many", count, instance.new_values == expected, instance.init_values == expected)


class Derived(Both):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.derived = True


derived = Derived([1, 2], c=[3])
print("inherited-new", isinstance(derived, Derived), derived.derived)
print("inherited-values", derived.new_values, derived.init_values)


class InheritedInit(InitOnly):
    pass


print("inherited-init", InheritedInit(8, c=9).values)


class NativeList(list):
    def __init__(self, first, second, *, marker=0):
        gc.collect()
        super().__init__([first, second])
        self.marker = marker


native = NativeList(3, 4, marker=5)
print("native", list(native), native.marker)

foreign_value = {"existing": 7}


class Foreign:
    def __new__(cls, *args, **kwargs):
        gc.collect()
        return foreign_value

    def __init__(self, *args, **kwargs):
        raise RuntimeError("must not initialize a foreign object")


print("foreign", Foreign() is foreign_value, Foreign(1, a=2) is foreign_value)


class Recursive:
    def __new__(cls, depth, *, value):
        instance = object.__new__(cls)
        if depth:
            instance.new_child = cls(depth - 1, value=value + 1)
        else:
            instance.new_child = None
        gc.collect()
        return instance

    def __init__(self, depth, *, value):
        gc.collect()
        self.value = value


node = Recursive(8, value=10)
values = []
while node is not None:
    values.append(node.value)
    node = node.new_child
print("recursive", values)


class FailNew:
    def __new__(cls, *args, **kwargs):
        gc.collect()
        raise ValueError("new")


class FailInit:
    def __init__(self, *args, **kwargs):
        gc.collect()
        raise ValueError("init")


class BadInit:
    def __init__(self, a):
        return a


class Required:
    def __init__(self, a, b):
        pass


failures = 0
for iteration in range(40):
    for failing in (FailNew, FailInit):
        try:
            failing(*range(16), a=[1, 2], b=[3, 4])
        except ValueError:
            failures += 1
    try:
        BadInit(1)
    except TypeError:
        failures += 1
    try:
        Required(1)
    except TypeError:
        failures += 1
    try:
        InitOnly(1, a=2)
    except TypeError:
        failures += 1
    # Repeat valid calls immediately after exceptions, not only after a GC.
    assert InitOnly(3, c=4).values == (3, 1, 4)
    assert Both(5, a=6).init_values == ((5,), [("a", 6)])
print("exceptions", failures)

gc.collect()
print("after-errors", InitOnly().values, InitOnly(1, c=2).values)
print("after-errors-both", Both().init_values, Both([1], a=[2]).init_values)
print("after-errors-foreign", Foreign(3, a=4) is foreign_value)
