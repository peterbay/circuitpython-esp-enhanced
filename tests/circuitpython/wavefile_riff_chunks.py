# "fmt " need only come before "data": other chunks may precede it, odd-length
# chunks carry a pad byte, and malformed format chunks are rejected.
try:
    import os, audiocore

    os.VfsFat
except (ImportError, AttributeError):
    print("SKIP")
    raise SystemExit


class RAMFS:
    SEC_SIZE = 512

    def __init__(self, blocks):
        self.data = bytearray(blocks * self.SEC_SIZE)

    def readblocks(self, n, buf):
        for i in range(len(buf)):
            buf[i] = self.data[n * self.SEC_SIZE + i]
        return 0

    def writeblocks(self, n, buf):
        for i in range(len(buf)):
            self.data[n * self.SEC_SIZE + i] = buf[i]
        return 0

    def ioctl(self, op, arg):
        if op == 4:  # MP_BLOCKDEV_IOCTL_BLOCK_COUNT
            return len(self.data) // self.SEC_SIZE
        if op == 5:  # MP_BLOCKDEV_IOCTL_BLOCK_SIZE
            return self.SEC_SIZE


def le(value, size):
    return value.to_bytes(size, "little")


def chunk(tag, body):
    return tag + le(len(body), 4) + body + (b"\0" if len(body) & 1 else b"")


def fmt(channels=1, rate=16000, bits=8):
    align = channels * bits // 8
    return le(1, 2) + le(channels, 2) + le(rate, 4) + le(rate * align, 4) + le(align, 2) + le(bits, 2)


def wav(*chunks):
    body = b"WAVE" + b"".join(chunks)
    return b"RIFF" + le(len(body), 4) + body


bdev = RAMFS(50)
os.VfsFat.mkfs(bdev)
fs = os.VfsFat(bdev)

data = chunk(b"data", bytes(range(32, 40)))
cases = (
    ("plain", wav(chunk(b"fmt ", fmt()), data)),
    ("LIST before fmt", wav(chunk(b"LIST", b"INFOsome"), chunk(b"fmt ", fmt()), data)),
    ("odd JUNK before fmt", wav(chunk(b"JUNK", b"abc"), chunk(b"fmt ", fmt()), data)),
    ("odd chunk after fmt", wav(chunk(b"fmt ", fmt()), chunk(b"note", b"x"), data)),
    ("data before fmt", wav(data, chunk(b"fmt ", fmt()))),
    ("short fmt", wav(chunk(b"fmt ", fmt()[:14]), data)),
    ("no channels", wav(chunk(b"fmt ", fmt(channels=0)), data)),
    ("no bits", wav(chunk(b"fmt ", fmt(bits=0)), data)),
    ("12 bits", wav(chunk(b"fmt ", fmt(bits=12)), data)),
    ("rate 0", wav(chunk(b"fmt ", fmt(rate=0)), data)),
)

for name, contents in cases:
    with fs.open("t.wav", "wb") as f:
        f.write(contents)
    with fs.open("t.wav", "rb") as f:
        try:
            sample = audiocore.WaveFile(f)
        except ValueError as e:
            print(name, "ValueError:", e)
            continue
        audiocore.reset_buffer(sample)
        result, buf = audiocore.get_buffer(sample)
        print(name, sample.sample_rate, result, list(buf))

# The silent pad after the last 16-bit samples is written bytewise, so a caller
# buffer that is not halfword aligned works.
with fs.open("t.wav", "wb") as f:
    f.write(wav(chunk(b"fmt ", fmt(bits=16)), chunk(b"data", b"\x11\x22\x33\x44\x55\x66")))
with fs.open("t.wav", "rb") as f:
    sample = audiocore.WaveFile(f, memoryview(bytearray(17))[1:])
    audiocore.reset_buffer(sample)
    result, buf = audiocore.get_buffer(sample)
    print("16-bit pad", result, list(memoryview(buf).cast("B")))
