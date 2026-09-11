# Indexing, slicing and len() of strings built through every path there is,
# ASCII and not, so that a string wrongly believed to be all ASCII would show
# up as a wrong character, a wrong length or a wrong slice.

ASCII = "hello, world"
WIDE = "héllo, wörld"
MIXED = "aébüc"


def probe(s, label):
    out = [label, str(len(s))]
    for i in range(len(s)):
        out.append(s[i])
    for i in range(1, len(s) + 1):
        out.append(s[-i])
    out.append(s[:])
    out.append(s[1:])
    out.append(s[:-1])
    out.append(s[2:5])
    out.append(s[-3:])
    out.append(s[:-3])
    out.append(s[3:3])
    out.append(s[5:2])
    out.append(s[-100:100])
    print(" ".join(out))


# literals
probe(ASCII, "lit-ascii")
probe(WIDE, "lit-wide")
probe(MIXED, "lit-mixed")

# concatenation
probe(ASCII + "!", "cat-ascii")
probe(ASCII + "é", "cat-becomes-wide")
probe(WIDE + "!", "cat-wide")
probe("" + ASCII, "cat-empty")

# slicing a string, then indexing the result
probe(ASCII[2:], "slice-ascii")
probe(WIDE[2:], "slice-wide")
probe(MIXED[1:4], "slice-mixed")
probe(WIDE[0:1], "slice-one-wide")

# formatting
probe("%s-%d" % (ASCII, 7), "modulo-ascii")
probe("%s-%d" % (WIDE, 7), "modulo-wide")
probe("{}+{}".format(ASCII, 1), "format-ascii")
probe("{}+{}".format(WIDE, 1), "format-wide")
probe(str(12345), "str-int")

# methods that build new strings
probe(ASCII.upper(), "upper-ascii")
probe(ASCII.lower(), "lower-ascii")
probe(ASCII.replace("l", "L"), "replace-ascii")
probe(WIDE.replace("l", "L"), "replace-wide")
probe(ASCII.strip("hd"), "strip-ascii")
probe(WIDE.strip("hd"), "strip-wide")
probe("-".join([ASCII, "x"]), "join-ascii")
probe("-".join([WIDE, "x"]), "join-wide")
for part in ASCII.split(","):
    probe(part, "split-ascii")
for part in WIDE.split(","):
    probe(part, "split-wide")

# from bytes
probe(bytes(ASCII, "utf-8").decode(), "decode-ascii")
probe(bytes(WIDE, "utf-8").decode(), "decode-wide")
probe(str(b"plain bytes", "utf-8"), "strbytes-ascii")

# characters obtained by indexing, then indexed again
for s in (ASCII, WIDE, MIXED):
    for i in range(len(s)):
        c = s[i]
        print(c, len(c), c[0], c[-1], c[0:1], c[1:], ord(c))

# chr() across the boundary
for cp in (0, 1, 65, 127, 128, 169, 255, 256, 1000, 0x20AC):
    c = chr(cp)
    print(cp, len(c), c == c[0], c[0:1] == c, ord(c[0]))

# iteration must agree with indexing
for s in (ASCII, WIDE, MIXED, WIDE[3:], ASCII + WIDE):
    print([c for c in s] == [s[i] for i in range(len(s))], len([c for c in s]), len(s))

# equality and dict keys must not be confused by the flag
d = {}
for s in (ASCII, WIDE, MIXED, ASCII[:], WIDE[:], "hello, world", "héllo, wörld"):
    d[s] = d.get(s, 0) + 1
print(sorted(d.values()), len(d))
print(ASCII == "hello, world", WIDE == "héllo, wörld", ASCII == WIDE)
print(hash(ASCII) == hash("hello, world"), hash(WIDE) == hash("héllo, wörld"))
print(ASCII in d, WIDE in d, (ASCII + "") in d, "nope" in d)

# find/index/startswith with explicit ranges, which use the walking path
for s in (ASCII, WIDE, MIXED):
    print(s.find("l"), s.find("l", 3), s.find("l", 3, 6), s.rfind("l"),
          s.startswith("h"), s.endswith("d"), s.count("l"))
for s in (ASCII, WIDE):
    print(s.index("o"), s.index("o", 3), s.rindex("o"), s[s.index("o"):])

# errors
for s in (ASCII, WIDE):
    try:
        s[len(s)]
    except IndexError:
        print("IndexError")
    try:
        s[-len(s) - 1]
    except IndexError:
        print("IndexError")
    try:
        s["x"]
    except TypeError:
        print("TypeError")
