# SPDX-FileCopyrightText: Copyright (c) 2026 CircuitPython contributors
# SPDX-License-Identifier: MIT
# Exercise constructor argument-array boundaries with collection and nested
# constructors while the outer constructor's temporary arguments remain live.

import gc


class Nested:
    def __init__(self, a, b=0):
        gc.collect()
        self.value = a + b


class Both:
    def __new__(cls, *args, **kwargs):
        gc.collect()
        assert Nested(10, 20).value == 30
        instance = object.__new__(cls)
        instance.new_values = (args, sorted(kwargs.items()))
        return instance

    def __init__(self, *args, **kwargs):
        gc.collect()
        assert Nested(a=7).value == 7
        self.init_values = (args, sorted(kwargs.items()))


count = 0
for positional_count in range(9):
    for keyword_count in range(9):
        arguments = tuple([n, n + 1] for n in range(positional_count))
        keywords = {"k" + str(n): [n, n + 2] for n in range(keyword_count)}
        instance = Both(*arguments, **keywords)
        expected = (arguments, sorted(keywords.items()))
        assert instance.new_values == expected
        assert instance.init_values == expected
        count += 1
print("constructor GC boundaries", count)
