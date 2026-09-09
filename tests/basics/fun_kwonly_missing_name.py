# Which keyword-only argument is named when one is missing.
#
# fun_kwonly_binding.py compares only the type of the error, which cannot see a
# wrong name: the binder used to index the bytecode stream instead of the qstr
# it had already decoded, so a call missing 'bravo' reported '__dir__'.
#
# The wording differs between implementations, so only the name is compared.
# Every case leaves exactly one argument missing, because implementations differ
# in which one they report when several are.


def name_of(call):
    try:
        call()
    except TypeError as e:
        text = str(e)
        parts = text.split("'")
        return parts[-2] if len(parts) >= 2 else "no name in: " + text
    return "no error"


def two(*, alpha, bravo):
    return alpha, bravo


def three(*, charlie, delta, echo):
    return charlie, delta, echo


def after_positional(first, second, *, foxtrot, golf):
    return first, second, foxtrot, golf


def defaulted(*, hotel=1, india):
    return hotel, india


def after_varargs(first, *rest, juliett, kilo=2):
    return first, rest, juliett, kilo


def with_varkw(*, lima, mike=3, **rest):
    return lima, mike, rest


def many(*, a1, b2, c3, d4, e5, f6, g7, h8, i9):
    return a1, b2, c3, d4, e5, f6, g7, h8, i9


def long_name(*, an_unusually_long_keyword_only_parameter_name, november=1):
    return an_unusually_long_keyword_only_parameter_name


class Holder:
    def method(self, first, *, oscar, papa=4):
        return first, oscar, papa

    @staticmethod
    def static(*, quebec, tango=5):
        return quebec, tango

    @classmethod
    def klass(cls, *, romeo, uniform=6):
        return romeo, uniform


holder = Holder()

print("last of two", name_of(lambda: two(alpha=1)))
print("first of two", name_of(lambda: two(bravo=1)))
print("middle of three", name_of(lambda: three(charlie=1, echo=3)))
print("last of three", name_of(lambda: three(charlie=1, delta=2)))
print("first of three", name_of(lambda: three(delta=2, echo=3)))
print("after positional", name_of(lambda: after_positional(1, 2, foxtrot=3)))
print("sibling has a default", name_of(lambda: defaulted(hotel=9)))
# No keyword argument is passed here at all. The call still takes the branch
# that decodes the names, because the function has keyword-only defaults.
print("no keywords passed", name_of(lambda: defaulted()))
print("after varargs", name_of(lambda: after_varargs(1, 2, 3, kilo=9)))
print("beside varkw", name_of(lambda: with_varkw(mike=1, extra=2)))
print("ninth of nine", name_of(lambda: many(a1=1, b2=2, c3=3, d4=4, e5=5, f6=6, g7=7, h8=8)))
print("fifth of nine", name_of(lambda: many(a1=1, b2=2, c3=3, d4=4, f6=6, g7=7, h8=8, i9=9)))
print("first of nine", name_of(lambda: many(b2=2, c3=3, d4=4, e5=5, f6=6, g7=7, h8=8, i9=9)))
print("long name", name_of(lambda: long_name(november=2)))
print("bound method", name_of(lambda: holder.method(1, papa=2)))
print("static method", name_of(lambda: Holder.static(tango=1)))
print("class method", name_of(lambda: Holder.klass(uniform=1)))
print("lambda", name_of(lambda: (lambda *, sierra, victor=1: sierra)(victor=2)))

# With no keyword arguments and no keyword-only defaults, a different branch
# runs, which reports that something is missing without saying what. Only the
# type is compared there.
for label, call in (
    ("no keywords, no defaults", lambda: two()),
    ("no keywords, method", lambda: holder.method(1)),
    ("no keywords, lambda", lambda: (lambda *, sierra: sierra)()),
):
    try:
        call()
        print(label, "no error")
    except TypeError:
        print(label, "TypeError")

print("still works", two(alpha=1, bravo=2), three(charlie=1, delta=2, echo=3))
print("still works", after_positional(1, 2, foxtrot=3, golf=4), defaulted(india=5))
print("still works", after_varargs(1, 2, 3, juliett=4), with_varkw(lima=1, november=2))
print("still works", many(a1=1, b2=2, c3=3, d4=4, e5=5, f6=6, g7=7, h8=8, i9=9))
print("still works", long_name(an_unusually_long_keyword_only_parameter_name=8))
print("still works", holder.method(1, oscar=2), Holder.static(quebec=3), Holder.klass(romeo=4))
