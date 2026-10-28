# RawSample validates channel_count and sample_rate, and keeps the buffer object
# so that samples reached only through it survive a collection.
try:
    import audiocore, gc
except ImportError:
    print("SKIP")
    raise SystemExit

for kwargs in ({"channel_count": 0}, {"channel_count": 3}, {"channel_count": -1}, {"sample_rate": 0}):
    try:
        audiocore.RawSample(bytearray(8), **kwargs)
    except ValueError as e:
        print(kwargs, "ValueError:", e)


def make():
    source = bytearray(range(256))
    # A view starting past the first GC block points into the middle of the
    # allocation, which the collector does not treat as a reference to it.
    return audiocore.RawSample(memoryview(source)[64:], single_buffer=True)


sample = make()
gc.collect()
junk = [bytearray(b"\xff" * 256) for _ in range(64)]
audiocore.reset_buffer(sample)
result, buf = audiocore.get_buffer(sample)
print(result, len(buf), buf[:4] == bytes(range(64, 68)), buf[-1])
