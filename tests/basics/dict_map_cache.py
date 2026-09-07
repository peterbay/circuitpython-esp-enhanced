# The lookup cache is shared by all maps and stores only a candidate position.
# A hit must still validate the key in the current map, after any map mutation.
import gc


# Reusing the exact same key across maps necessarily uses the same cache entry.
# Different map sizes and unrelated contents must never leak another map's value.
shared = "map_cache_shared"
maps = []
for count in (0, 1, 4, 17, 128, 300):
    mapping = {i: -i for i in range(count)}
    mapping[shared] = count + 1
    maps.append(mapping)
cross_map_ok = True
for _ in range(8):
    for mapping, count in zip(maps, (0, 1, 4, 17, 128, 300)):
        cross_map_ok = cross_map_ok and mapping[shared] == count + 1
    for mapping, count in zip(reversed(maps), (300, 128, 17, 4, 1, 0)):
        cross_map_ok = cross_map_ok and mapping.get(shared) == count + 1
print("cross maps", cross_map_ok)


mapping = {"alpha": 1, "beta": 2, "gamma": 3}
print("warm", mapping["alpha"], mapping["beta"], mapping["gamma"])
mapping["beta"] = 20
print("replace", mapping["beta"], len(mapping))
print("pop", mapping.pop("beta"), mapping.get("beta", "missing"))
mapping["beta"] = 200
print("reinsert", mapping["beta"], len(mapping))
del mapping["alpha"]
print("delete", "alpha" in mapping, mapping.get("alpha", "missing"))
mapping.clear()
print("clear", len(mapping), mapping.get("beta", "missing"))
mapping["gamma"] = 30
print("after clear", mapping["gamma"], mapping.setdefault("beta", 40))
print("setdefault", mapping.setdefault("beta", 99), mapping["beta"])


# Cache positions are only a byte wide: exercise tables larger than 256 slots
# and deletion/reinsertion while forcing repeated resizes.
large = {}
for i in range(600):
    large[i] = i * 3
print("large warm", len(large), sum(large[i] for i in range(600)))
for i in range(0, 600, 3):
    assert large.pop(i) == i * 3
for i in range(0, 600, 3):
    large[i] = -i
print("large mutated", len(large), sum(large[i] for i in range(600)))


class Collision:
    def __init__(self, value):
        self.value = value

    def __hash__(self):
        return 7

    def __eq__(self, other):
        return isinstance(other, Collision) and self.value == other.value


keys = [Collision(i) for i in range(12)]
collisions = {key: key.value for key in keys}
print("hash collisions", [collisions[Collision(i)] for i in range(12)])
del collisions[keys[5]]
print("collision missing", collisions.get(Collision(5), "missing"))
collisions[Collision(5)] = 50
gc.collect()
print("collision restored", collisions[keys[5]], collisions[Collision(6)])


# Equal values may have different object tags/cache offsets. Equality fallback
# must preserve Python's common numeric key and distinguish unrelated key types.
numeric = {True: "boolean"}
print("numeric lookup", numeric[1], numeric[1.0])
numeric[1.0] = "float replacement"
print("numeric replace", len(numeric), numeric[True], numeric[1])
print("numeric pop", numeric.pop(1), len(numeric))
numeric[False] = "zero"
numeric[-0.0] = "negative zero"
print("zero", len(numeric), numeric[0], numeric[0.0])
mixed = {"1": "text", 1: "integer", None: "none", (1,): "tuple"}
# Keep bytes separate: a str/bytes probe collision intentionally emits a
# BytesWarning in this port, which is unrelated to the lookup cache behavior.
byte_keys = {b"1": "bytes"}
print("mixed", [mixed["1"], byte_keys[b"1"], mixed[True], mixed[1.0], mixed[None], mixed[(1,)]])


literal = "map_cache_dynamic_identifier"
dynamic = "".join(("map_cache_dynamic_", "identifier"))
strings = {literal: 10}
print("dynamic equality", literal == dynamic, strings[dynamic])
strings[dynamic] = 20
print("dynamic update", len(strings), strings[literal], strings[dynamic])
print("dynamic remove", strings.pop(dynamic), literal in strings)
strings[dynamic] = 30
print("dynamic insert", strings[literal])


# Dropping whole dictionaries frees their tables. Cache entries must not retain
# table pointers or become unsafe when a subsequent allocation reuses memory.
reuse_ok = True
for round_number in range(32):
    old = {shared: round_number, "discard": -1}
    assert old[shared] == round_number
    del old
    gc.collect()
    fresh = {"different": round_number + 1}
    reuse_ok = reuse_ok and fresh.get(shared, "missing") == "missing"
    fresh[shared] = round_number + 2
    reuse_ok = reuse_ok and fresh[shared] == round_number + 2
print("table reuse", reuse_ok)


class Base:
    map_cache_shared = 10


class Child(Base):
    pass


instance = Child()
print("attribute warm", instance.map_cache_shared, Child.map_cache_shared)
Base.map_cache_shared = 20
print("class update", instance.map_cache_shared, Child.map_cache_shared)
instance.map_cache_shared = 30
print("instance shadow", instance.map_cache_shared, Base.map_cache_shared)
del instance.map_cache_shared
print("unshadow", instance.map_cache_shared)
del Base.map_cache_shared
print("class delete", hasattr(instance, "map_cache_shared"))
Base.map_cache_shared = 40
print("class reinsert", instance.map_cache_shared)


# A builtin module exercises fixed ordered maps without modifying their values.
collect = gc.collect
print("module lookup", gc.collect is collect, gc.collect is getattr(gc, "collect"))
try:
    gc.map_cache_definitely_missing
except AttributeError:
    print("module missing", "AttributeError")


# This module's globals are another map, shared with the attribute-name cache.
map_cache_shared = 100


def read_global():
    return map_cache_shared


print("global warm", read_global())
globals()[shared] = 200
print("global update", read_global(), instance.map_cache_shared)
del globals()[shared]
try:
    read_global()
except NameError:
    print("global missing", "NameError")
globals()[shared] = 300
print("global reinsert", read_global())
