# Positional defaults, called with every number of arguments the parameters
# admit, in the shapes an in-place frame builder has to get right: several
# defaults, defaults after required parameters, extra locals below the
# parameters, nested and recursive calls, methods, and the shapes that must
# fall back (varargs, keyword arguments, keyword-only, closures, many
# parameters).


def d0():
    return "d0"


def d1(a=1):
    return a


def d2(a, b=2):
    return (a, b)


def d5(a, b=2, c=3, d=4, e=5):
    return (a, b, c, d, e)


print(d0(), d1(), d1(9), d2(1), d2(1, 9))
for n in range(1, 6):
    args = tuple(range(10, 10 + n))
    print(n, d5(*args))

# the defaults must not be disturbed by a call that overrides them
for i in range(3):
    print(d5(0), d5(0, 1), d5(0, 1, 2), d5(0, 1, 2, 3), d5(0, 1, 2, 3, 4), d5(0))

# locals below the parameters must start out unbound
def locals_below(a, b=7):
    x = a + b
    y = x * 2
    z = [x, y]
    return (x, y, z)


print(locals_below(1), locals_below(1, 2))


def unbound_local(a=1):
    if a > 0:
        v = a
    try:
        return v
    except NameError:
        return "unbound"


print(unbound_local(1), unbound_local(0))

# a mutable default is created once and shared
def accumulate(x, into=[]):
    into.append(x)
    return into


print(accumulate(1), accumulate(2), accumulate(3))


def default_is_none(a, b=None):
    if b is None:
        b = []
    b.append(a)
    return b


print(default_is_none(1), default_is_none(2), default_is_none(3, [0]))

# defaults evaluated once, at definition
counter = [0]


def bump():
    counter[0] += 1
    return counter[0]


def evaluated_once(a=bump()):
    return a


print(evaluated_once(), evaluated_once(), evaluated_once(5), counter)

# recursion through the same shape
def fact(n, acc=1):
    if n <= 1:
        return acc
    return fact(n - 1, acc * n)


print(fact(1), fact(5), fact(10))


def fib(n, a=0, b=1):
    while n > 0:
        a, b = b, a + b
        n -= 1
    return a


print([fib(i) for i in range(10)], fib(10, 1, 1))

# methods and inheritance
class A:
    def __init__(self, x=10, y=20):
        self.x = x
        self.y = y

    def scale(self, k=2, off=0):
        return self.x * k + off


class B(A):
    def __init__(self, x=100):
        super().__init__(x)


for o in (A(), A(1), A(1, 2), B(), B(3)):
    print(o.x, o.y, o.scale(), o.scale(3), o.scale(3, 1))

# shapes that must not take a defaults fast path
def with_varargs(a, b=2, *rest):
    return (a, b, rest)


def with_kwargs(a, b=2, **kw):
    return (a, b, sorted(kw.items()))


def with_kwonly(a, b=2, *, c=3):
    return (a, b, c)


print(with_varargs(1), with_varargs(1, 2, 3, 4))
print(with_kwargs(1), with_kwargs(1, 2, z=3))
print(with_kwonly(1), with_kwonly(1, 5), with_kwonly(1, 5, c=6))
print(d5(1, c=30), d5(1, 2, e=50), d2(b=8, a=7))


def make_closure(m=3):
    def inner(x, k=m):
        return x * k

    return inner


f = make_closure()
g = make_closure(10)
print(f(2), f(2, 5), g(2), g(2, 5))

# a function with more parameters than a packed argument count can hold
def many(a0=0, a1=1, a2=2, a3=3, a4=4, a5=5, a6=6, a7=7, a8=8, a9=9,
         b0=10, b1=11, b2=12, b3=13, b4=14, b5=15, b6=16, b7=17, b8=18, b9=19,
         c0=20, c1=21, c2=22, c3=23, c4=24, c5=25, c6=26, c7=27, c8=28, c9=29,
         d0v=30, d1v=31, d2v=32, d3v=33):
    return a0 + a9 + b0 + b9 + c0 + c9 + d0v + d3v


print(many(), many(1), many(*range(34)))

# wrong argument counts still raise
for call in (lambda: d2(), lambda: d5(), lambda: d2(1, 2, 3), lambda: d0(1),
             lambda: d5(1, 2, 3, 4, 5, 6), lambda: d1(1, 2)):
    try:
        call()
        print("no error")
    except TypeError:
        print("TypeError")
