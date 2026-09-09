# getattr/setattr/hasattr with names built at run time, repeated: the interned
# name is remembered per buffer, and a buffer reused for other bytes must not
# answer with the old name.


class O:
    pass


o = O()
names = ["attr" + str(i) for i in range(6)]
for i, n in enumerate(names):
    setattr(o, n, i)
for _ in range(3):
    for i, n in enumerate(names):
        print(n, hasattr(o, n), getattr(o, n))

# the same text in a different str object each time
for i in range(6):
    n = "att" + "r" + str(i)
    print(getattr(o, n), hasattr(o, n + "x"))

# a buffer whose bytes change under a name of the same length
buf = bytearray(b"attr0")
for i in range(6):
    buf[4] = ord("0") + i
    print(getattr(o, buf.decode()))

# names that did not exist before and are created by setattr
for i in range(6, 12):
    n = "new" + str(i)
    setattr(o, n, i * 2)
    print(n, getattr(o, n), getattr(o, "new" + str(i)))

# keyword fields in format look the name up the same way
f = "{a}-{b}"
for i in range(3):
    print(f.format(a=i, b=names[i]))
