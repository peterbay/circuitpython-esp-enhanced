# Helper for name_probe_cache.py: a module whose attributes are rebound,
# deleted and answered by a module-level __getattr__.
value = 1
table = [1, 2, 3]


def twice(x):
    return x * 2


def __getattr__(name):
    return "dynamic:" + name
