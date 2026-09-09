# Exercise the keyword-only validation boundary in the bytecode argument binder.
# Error types are compared; error wording is implementation-specific.
import gc


def plain(a, b=20, c=30):
    return a, b, c


print("plain", plain(1), plain(a=1), plain(c=3, a=1), plain(1, **{"b": 2}))


def defaults(a=1, b=2, c=3, d=4, e=5, f=6, g=7, h=8):
    return a, b, c, d, e, f, g, h


print("defaults", defaults(), defaults(a=9), defaults(h=9), defaults(h=9, a=0))


def positional_rest(a, b=2, *rest):
    return a, b, rest


print("star", positional_rest(a=1), positional_rest(1, 2, 3, 4))
print("star kwargs", positional_rest(1, b=9), positional_rest(*(1, 2, 3)))


def keyword_rest(a=1, **rest):
    return a, sorted(rest.items())


print("dict", keyword_rest(), keyword_rest(a=2, z=3), keyword_rest(z=3, y=4))


def both_rest(a, *rest, **kwargs):
    return a, rest, sorted(kwargs.items())


print("both", both_rest(a=1), both_rest(1, 2, 3, z=4, y=5))


def only_keywords(*, first, second=20):
    return first, second


print("only", only_keywords(first=1), only_keywords(second=2, first=1))


def mixed(a, b=2, *rest, required, default=5, **kwargs):
    return a, b, rest, required, default, sorted(kwargs.items())


print("mixed", mixed(1, required=4))
print("mixed full", mixed(1, 2, 3, 4, default=6, required=5, z=7))
print("mixed expanded", mixed(**{"required": 4, "a": 1, "b": 3}))


def default_keywords(a=1, *, first=2, second=3):
    return a, first, second


print("kw defaults", default_keywords(), default_keywords(a=4))
print("kw overrides", default_keywords(second=5), default_keywords(6, first=7))


class Methods:
    def ordinary(self, a=1, b=2):
        return a, b

    def keywords(self, a=1, *, required, default=3):
        return a, required, default


instance = Methods()
bound = instance.ordinary
bound_keywords = instance.keywords
print("method", instance.ordinary(a=4), bound(b=5), Methods.ordinary(instance, a=6))
print("method kw", instance.keywords(required=2), bound_keywords(default=7, required=8))


def make_closures(seed):
    def ordinary(a=1, b=2):
        gc.collect()
        return seed, a, b

    def keywords(a=1, *, required, default=3):
        gc.collect()
        return seed, a, required, default

    return ordinary, keywords


closure, closure_kw = make_closures([9])
print("closure", closure(b=4), closure_kw(required=5))


def generator(a=1, b=2):
    gc.collect()
    yield a
    yield b


def generator_kw(a=1, *, required, default=3):
    gc.collect()
    yield a
    yield required
    yield default


pending = generator(b=[4])
gc.collect()
print("generator", list(pending), list(generator_kw(default=5, required=4)))


shared_default = []


def mutate_default(a, values=shared_default):
    gc.collect()
    values.append(a)
    return values


first = mutate_default(a=1)
second = mutate_default(a=2)
print("shared default", first is second, second is shared_default, second)


def make_cell(a, b=2):
    def inner():
        return a, b

    return inner


cell = make_cell(b=[4], a=[3])
gc.collect()
print("argument cells", cell())


def expect_type_error(label, call):
    try:
        call()
    except TypeError:
        print(label, "TypeError")
    else:
        print(label, "NO ERROR")


expect_type_error("plain missing", lambda: plain(b=2))
expect_type_error("plain unknown", lambda: plain(1, unknown=2))
expect_type_error("plain duplicate", lambda: plain(1, a=2))
expect_type_error("plain too many", lambda: plain(1, 2, 3, 4))
expect_type_error("star duplicate", lambda: positional_rest(1, a=2))
expect_type_error("star unknown", lambda: positional_rest(1, unknown=2))
expect_type_error("dict duplicate", lambda: keyword_rest(1, a=2))
expect_type_error("non-string keyword", lambda: keyword_rest(**{1: 2}))
expect_type_error("kw missing no keywords", lambda: only_keywords())
expect_type_error("kw missing with keyword", lambda: only_keywords(second=2))
expect_type_error("kw extra positional", lambda: only_keywords(1, first=2))
expect_type_error("kw unknown", lambda: only_keywords(first=1, unknown=2))
expect_type_error("mixed missing", lambda: mixed(a=1))
expect_type_error("method kw missing", lambda: instance.keywords(a=1))
expect_type_error("closure kw missing", lambda: closure_kw(a=1))
# Iterating also observes CPython's generator-body errors, if any, while the
# argument binding itself must reject the call before the body can execute.
expect_type_error("generator kw missing", lambda: list(generator_kw(a=1)))


entered = 0


def counted(a=1, *, required):
    global entered
    entered += 1
    return a, required


for _ in range(25):
    try:
        counted(a=2)
    except TypeError:
        pass
    else:
        raise AssertionError("missing keyword-only argument accepted")
    gc.collect()
print("failed calls", entered)
print("after failures", counted(required=3), entered)
