# operators on ints that fit 64 bits but not a small int, against the ones
# that do not, on every shape the machine-arithmetic path takes or refuses

big = 2 ** 40
vals = [
    0, 1, -1, 7, -7,
    2 ** 30 - 1, -2 ** 30, 2 ** 30, -2 ** 30 - 1,
    2 ** 31 - 1, 2 ** 31, -2 ** 31, 0xFFFFFFFF,
    big, -big, big + 3, 10 ** 12, -10 ** 12,
    2 ** 62, -2 ** 62, 2 ** 63 - 1, -2 ** 63,
    2 ** 63, -2 ** 63 - 1, 2 ** 70, -2 ** 70,
]

for a in vals:
    for b in vals:
        print(a + b, a - b, a * b, a & b, a | b, a ^ b)
        print(a < b, a <= b, a == b, a >= b, a > b, a != b)
        if b != 0:
            print(a // b, a % b, divmod(a, b))
        if 0 <= b < 80:
            print(a << b, a >> b)

# mixed signs of floor division and modulo
for a in (10 ** 12 + 7, -(10 ** 12 + 7), 2 ** 40 + 1, -(2 ** 40 + 1)):
    for b in (3, -3, 1000000, -1000000, 2 ** 33, -2 ** 33):
        print(a // b, a % b, a // b * b + a % b == a)

# results that come back down to small ints work as indexes and keys
seq = [10, 20, 30, 40]
i = (big + 2) - big
print(seq[i], seq[(big + 3) // (big + 1) + 1])
d = {big - big: "zero", 2 ** 40: "big"}
print(d[0], d[(2 ** 41) // 2], d[big // 1])
print(hash(2 ** 40 - 2 ** 40 + 5) == hash(5), (2 ** 40 - 1) & 0x7FFFFFFF)

# what a driver does with 32-bit values
crc = 0xFFFFFFFF
for byte in b"CircuitPython on a 31-bit small int":
    crc ^= byte
    for _ in range(8):
        crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1))
print(hex(crc ^ 0xFFFFFFFF))
ns = 1234567890123
print(ns // 1000000, ns % 1000000, ns // 1000000000, (ns + 999999) // 1000000)
print(-ns // 1000000, -ns % 1000000)

# the overflow of small ints straight into 64 bits
for x in (46341, -46341, 65536, 2 ** 30 - 1, -2 ** 30):
    print(x * x, x * -x, x << 31, x << 32, x << 33, x * 3 * 5 * 7 * 11)
print(1 << 62, 1 << 63, -1 << 63, 3 << 61, -3 << 61, (2 ** 62) << 1, (2 ** 62) << 2)
print(2 ** 63 >> 1, -2 ** 63 >> 1, 2 ** 70 >> 8, -2 ** 70 >> 8, 2 ** 62 >> 70, -2 ** 62 >> 70)

# errors
for a, b in ((big, 0), (-big, 0), (2 ** 70, 0)):
    try:
        a // b
    except ZeroDivisionError:
        print("ZeroDivisionError //")
    try:
        a % b
    except ZeroDivisionError:
        print("ZeroDivisionError %")
for s in (-1, -2 ** 40):
    try:
        big << s
    except ValueError:
        print("ValueError <<")
    try:
        big >> s
    except ValueError:
        print("ValueError >>")

# the edges of the machine word
print(-2 ** 63 // -1, -2 ** 63 % -1, (-2 ** 63) * -1, -(-2 ** 63))
print(2 ** 63 - 1 + 1, -2 ** 63 - 1, (2 ** 63 - 1) * 2, (2 ** 32) * (2 ** 32))
print(2 ** 63 - 1 == 9223372036854775807, -2 ** 63 == -9223372036854775808)
print((2 ** 63 - 1) & -1, (2 ** 63 - 1) | -2 ** 63, (2 ** 63 - 1) ^ -1)
