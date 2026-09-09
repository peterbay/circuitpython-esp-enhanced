# The class lookup cache remembers where a name was found in a class and its
# bases, or that it was not. Every case here changes the answer after it has
# been cached at least once: a class gaining or losing a property, a
# __setattr__ or a __getattr__, a base method overridden and restored, a class
# attribute rebound or shadowed, a type made and mutated through its dict.
# Printed so that CPython produces the same lines.


class P:
    limit = 10

    def __init__(self):
        self.v = 1

    @property
    def p(self):
        return "p:%d" % self.v

    def m(self):
        return "m:%d" % self.v


print("--- stores on a class with a property, then a stored name becomes a property")
o = P()
for i in range(5):
    o.v = i
    o.w = i
print(o.v, o.w, o.p, o.m())
del o.w
P.w = property(lambda self: "prop-w", lambda self, x: setattr(self, "_w", x * 2))
o.w = 7
print(o.w, o._w)
del P.w
o.w = 8
print(o.w, o.p)

print("--- __setattr__ arriving later and leaving again")


class Q:
    @property
    def ro(self):
        return 1


q = Q()
for i in range(3):
    q.a = i
print(q.a)
log = []
Q.__setattr__ = lambda self, name, value: log.append((name, value))
q.a = 99
q.b = 100
print(log, q.a, hasattr(q, "b"))
del Q.__setattr__
q.a = 5
print(q.a, log)

print("--- __getattr__ arriving later")


class R:
    @property
    def ro(self):
        return 1


r = R()
r.x = 1
for i in range(3):
    try:
        r.missing
    except AttributeError:
        print("AttributeError", i)
R.__getattr__ = lambda self, name: "dyn:" + name
print(r.missing, r.x, r.ro)
del R.__getattr__
try:
    r.missing
except AttributeError:
    print("AttributeError again")

print("--- inherited methods overridden and restored, with a property in the chain")


class Base:
    @property
    def kind(self):
        return "base"

    def who(self):
        return "base.who"

    def only_base(self):
        return "only_base"


class Child(Base):
    def who(self):
        return "child.who"


c = Child()
print([c.who() for _ in range(3)], c.only_base(), c.kind)
Child.only_base = lambda self: "child.only_base"
print(c.only_base())
del Child.only_base
print(c.only_base())
Base.only_base = lambda self: "base.only_base2"
print(c.only_base())
del Child.who
print(c.who())

print("--- class attributes read through instances, rebound and shadowed")
o = P()
print([o.limit for _ in range(3)])
P.limit = 20
print(o.limit)
o.limit = 30
print(o.limit, P.limit)
del o.limit
print(o.limit)
del P.limit
try:
    o.limit
except AttributeError:
    print("AttributeError limit")
P.limit = "back"
print(o.limit)

print("--- descriptors, as register libraries use them")


class Field:
    def __init__(self, name):
        self.name = name

    def __get__(self, obj, objtype=None):
        return obj._regs.get(self.name, 0)

    def __set__(self, obj, value):
        obj._regs[self.name] = value


class Device:
    temp = Field("temp")
    mode = Field("mode")

    def __init__(self):
        self._regs = {}
        self.plain = "plain"


d = Device()
for i in range(3):
    d.temp = i
    d.mode = i * 2
    d.plain = i
print(d.temp, d.mode, d.plain, sorted(d._regs.items()))
Device.later = Field("later")
d.later = 4
print(d.later, sorted(d._regs.items()), "later" in d.__dict__)

print("--- multiple inheritance")


class Mixin:
    def hello(self):
        return "mixin"

    def both(self):
        return "mixin.both"


class Other:
    def both(self):
        return "other.both"

    def only_other(self):
        return "only_other"


class Combined(Mixin, Other):
    pass


cb = Combined()
print([cb.hello() for _ in range(2)], cb.both(), cb.only_other())
Combined.hello = lambda self: "combined"
print(cb.hello())
del Combined.hello
print(cb.hello())

print("--- subclasses of native types")


class MyList(list):
    def total(self):
        return sum(self)


ml = MyList([1, 2, 3])
ml.append(4)
ml.tag = "t"
print(ml.total(), len(ml), ml.tag, ml[-1])


class MyError(Exception):
    def describe(self):
        return "described:" + str(self.args)


try:
    raise MyError("boom", 2)
except MyError as e:
    print(e.describe(), e.args)

print("--- special methods rebound at run time")


class V:
    def __init__(self, n):
        self.n = n

    def __eq__(self, other):
        return self.n == other

    def __getitem__(self, i):
        return self.n + i

    def __len__(self):
        return self.n

    def __call__(self, x):
        return self.n * x


v = V(3)
print(v == 3, v == 4, v[1], len(v), v(2))
V.__getitem__ = lambda self, i: "new"
V.__len__ = lambda self: 99
print(v[1], len(v), v(2))

print("--- static and class methods through instances and classes")


class S:
    @staticmethod
    def s(x):
        return "s" + str(x)

    @classmethod
    def c(cls, x):
        return cls.__name__ + str(x)


class S2(S):
    pass


s = S2()
print(s.s(1), s.c(2), S2.s(3), S2.c(4), S.c(5))
S.c = classmethod(lambda cls, x: "new" + cls.__name__)
print(s.c(0), S.c(0))

print("--- builtins stay builtins while objects are made")


class Q2:
    pass


total = 0
for i in range(50):
    q2 = Q2()
    q2.x = i
    total += len("ab")
print(total)
len = lambda x: 7
total = 0
for i in range(5):
    q2 = Q2()
    q2.y = i
    total += len("ab")
print(total)
del len
print(len("abc"))

print("--- exec with a dict that already holds a builtin's name")
for i in range(3):
    g = {"len": i}
    exec("result = len", g)
    print(g["result"])
g = {}
exec("result = len('four')", g)
print(g["result"])
print("hotovo")
