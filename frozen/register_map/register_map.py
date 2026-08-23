# SPDX-License-Identifier: MIT
"""
register_map
============

A named register map on top of the C ``registers`` module, aimed at making an
I2C (or SPI) sensor driver short and readable.

You describe the device's fields once, then read and write them by name — as
attributes::

    from register_map import RegisterMap
    from adafruit_bus_device.i2c_device import I2CDevice

    imu = RegisterMap.from_i2c(I2CDevice(i2c, 0x68), {
        "whoami":       (0x75, 0, 8, {"mode": "r"}),
        "reset":        (0x6B, 7, 1),
        "clock":        (0x6B, 0, 3),
        "gyro_range":   (0x1B, 3, 2, {"values": {"250dps": 0, "500dps": 1,
                                                 "1000dps": 2, "2000dps": 3}}),
        "temp":         (0x41, 0, 16, {"signed": True, "width": 2, "lsb_first": False,
                                       "scale": 1 / 340, "offset": 36.53, "mode": "r"}),
    })

    imu.reset = 1
    imu.gyro_range = "500dps"      # a name, not a magic number
    print(imu.gyro_range)          # -> "500dps"
    print(imu.temp)                # -> 23.7  (already in degrees C)

A field spec is ``(address, lowest_bit, num_bits)`` or the same with an options
dict:

* ``width``     register size in bytes (default 1)
* ``signed``    two's complement (default False)
* ``lsb_first`` byte order (default True)
* ``mode``      ``"rw"`` (default) or ``"r"`` for read-only
* ``values``    ``{name: int}`` — read gives the name, write takes name or int
* ``scale`` / ``offset``  read returns ``raw * scale + offset``, write inverts it
* ``cache``     ``"none"`` (default) or ``"full"`` — see below

The cache is opt-in per register because a shadow of a register the device
changes under you goes stale. Leave data and status registers at ``"none"`` (a
fresh bus read every time); mark host-owned config registers ``"full"`` to read
from the shadow and to batch writes. ``sync(name)`` / ``sync_all()`` re-read the
device when you know it changed a cached register.
"""

import registers

try:
    from typing import Dict, Optional, Tuple, Union
except ImportError:
    pass


class _I2CAccessor:
    def __init__(self, i2c_device, address_width=1, lsb_first=True):
        self._dev = i2c_device
        self._aw = address_width
        self._lsb_first = lsb_first
        self._addr = bytearray(address_width)
        self._out = bytearray(address_width + 8)

    def _pack(self, address):
        if self._lsb_first:
            for i in range(self._aw):
                self._addr[i] = (address >> (i * 8)) & 0xFF
        else:
            for i in range(self._aw):
                self._addr[i] = (address >> ((self._aw - 1 - i) * 8)) & 0xFF

    def read(self, address, buffer):
        self._pack(address)
        with self._dev as i2c:
            i2c.write_then_readinto(self._addr, buffer)

    def write(self, address, buffer):
        self._pack(address)
        n = self._aw + len(buffer)
        if n > len(self._out):
            self._out = bytearray(n)
        self._out[: self._aw] = self._addr
        self._out[self._aw : n] = buffer
        with self._dev as i2c:
            i2c.write(self._out, end=n)


class _SPIAccessor:
    # The R/W bit is the top bit of the first address byte: 1 to read, 0 to write.
    def __init__(self, spi_device, address_width=1, lsb_first=True):
        self._dev = spi_device
        self._aw = address_width
        self._lsb_first = lsb_first
        self._addr = bytearray(address_width)

    def _pack(self, address, read):
        if self._lsb_first:
            for i in range(self._aw):
                self._addr[i] = (address >> (i * 8)) & 0xFF
        else:
            for i in range(self._aw):
                self._addr[i] = (address >> ((self._aw - 1 - i) * 8)) & 0xFF
        if read:
            self._addr[0] |= 0x80
        else:
            self._addr[0] &= 0x7F

    def read(self, address, buffer):
        self._pack(address, True)
        with self._dev as spi:
            spi.write(self._addr)
            spi.readinto(buffer)

    def write(self, address, buffer):
        self._pack(address, False)
        with self._dev as spi:
            spi.write(self._addr)
            spi.write(buffer)


class _Field:
    __slots__ = ("address", "width", "mask", "shift", "signed", "lsb_first",
                 "cached", "writable", "by_int", "by_name", "scale", "offset")


class RegisterMap:
    """A named, typed register map. Access fields as attributes or by ``[name]``."""

    def __init__(self, accessor):
        # All internal state is underscore-prefixed so __setattr__ can tell it
        # apart from field assignments.
        object.__setattr__(self, "_acc", accessor)
        object.__setattr__(self, "_fields", {})
        object.__setattr__(self, "_widths", {})
        object.__setattr__(self, "_shadow", {})
        object.__setattr__(self, "_valid", set())
        object.__setattr__(self, "_batch", False)
        object.__setattr__(self, "_dirty", set())

    @classmethod
    def from_i2c(cls, i2c_device, fields, address_width=1, lsb_first=True):
        """Build over an ``adafruit_bus_device.I2CDevice``."""
        m = cls(_I2CAccessor(i2c_device, address_width, lsb_first))
        m._define(fields)
        return m

    @classmethod
    def from_spi(cls, spi_device, fields, address_width=1, lsb_first=True):
        """Build over an ``adafruit_bus_device.SPIDevice``."""
        m = cls(_SPIAccessor(spi_device, address_width, lsb_first))
        m._define(fields)
        return m

    def _define(self, fields):
        for name, spec in fields.items():
            address, lowest_bit, num_bits = spec[0], spec[1], spec[2]
            opts = spec[3] if len(spec) > 3 else {}
            f = _Field()
            f.address = address
            f.width = opts.get("width", 1)
            f.signed = opts.get("signed", False)
            f.lsb_first = opts.get("lsb_first", True)
            f.cached = opts.get("cache", "none") == "full"
            f.writable = opts.get("mode", "rw") != "r"
            f.mask = ((1 << num_bits) - 1) << lowest_bit
            f.shift = lowest_bit
            f.scale = opts.get("scale")
            f.offset = opts.get("offset", 0)
            values = opts.get("values")
            if values:
                f.by_name = values
                f.by_int = {v: k for k, v in values.items()}
            else:
                f.by_name = None
                f.by_int = None
            if f.mask >= (1 << (f.width * 8)):
                raise ValueError("field wider than register: " + name)
            self._fields[name] = f
            prev = self._widths.get(address)
            if prev is not None and prev != f.width:
                raise ValueError("register width mismatch at 0x%02x" % address)
            self._widths[address] = f.width
            if f.cached and address not in self._shadow:
                self._shadow[address] = bytearray(f.width)

    def _buffer(self, f):
        if f.cached:
            if f.address not in self._valid:
                self._acc.read(f.address, self._shadow[f.address])
                self._valid.add(f.address)
            return self._shadow[f.address]
        buf = bytearray(f.width)
        self._acc.read(f.address, buf)
        return buf

    def _get(self, f):
        raw = registers.extract(self._buffer(f), f.mask, f.shift, f.signed, f.lsb_first)
        if f.by_int is not None:
            return f.by_int.get(raw, raw)       # name, or the raw int if unknown
        if f.scale is not None:
            return raw * f.scale + f.offset      # physical units
        return raw

    def _set(self, f, name, value):
        if not f.writable:
            raise AttributeError("read-only field: " + name)
        if f.by_name is not None and isinstance(value, str):
            value = f.by_name[value]
        elif f.scale is not None and not isinstance(value, int):
            value = int(round((value - f.offset) / f.scale))
        buf = self._buffer(f)
        registers.insert(buf, f.mask, f.shift, value, f.lsb_first)
        if f.cached and self._batch:
            self._dirty.add(f.address)
        else:
            self._acc.write(f.address, buf)

    # --- dict-style access -------------------------------------------------
    def __getitem__(self, name):
        return self._get(self._fields[name])

    def __setitem__(self, name, value):
        self._set(self._fields[name], name, value)

    # --- attribute-style access -------------------------------------------
    def __getattr__(self, name):
        # Only reached when normal lookup fails, so methods are unaffected.
        fields = self.__dict__.get("_fields")
        if fields and name in fields:
            return self._get(fields[name])
        raise AttributeError(name)

    def __setattr__(self, name, value):
        fields = self.__dict__.get("_fields")
        if fields is not None and name in fields:
            self._set(fields[name], name, value)
        else:
            object.__setattr__(self, name, value)

    # --- cache control -----------------------------------------------------
    def sync(self, name):
        """Re-read the register behind a field from the device into the shadow.
        Use when the device changed a cached register under you."""
        f = self._fields[name]
        if f.cached:
            self._acc.read(f.address, self._shadow[f.address])
            self._valid.add(f.address)

    def sync_all(self):
        """Re-read every cached register from the device."""
        for address in self._shadow:
            self._acc.read(address, self._shadow[address])
            self._valid.add(address)

    def batch(self):
        """Context manager that defers writes to cached registers and flushes
        each touched register once on exit."""
        return _Batch(self)

    def fields(self):
        """The field names, for introspection."""
        return list(self._fields.keys())


class _Batch:
    def __init__(self, m):
        self._m = m

    def __enter__(self):
        object.__setattr__(self._m, "_batch", True)
        return self._m

    def __exit__(self, *exc):
        m = self._m
        object.__setattr__(m, "_batch", False)
        for address in m._dirty:
            m._acc.write(address, m._shadow[address])
        object.__setattr__(m, "_dirty", set())
        return False
