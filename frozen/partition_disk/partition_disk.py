# SPDX-FileCopyrightText: Copyright (c) 2026 Petr Vavrin
#
# SPDX-License-Identifier: MIT
"""
partition_disk
==============

Turn an ``espidf.Partition`` into a block device you can format with
``storage.VfsFat`` and mount for normal file access::

    import espidf, os, storage
    from partition_disk import PartitionDisk

    disk = PartitionDisk(espidf.Partition("ota_1"))
    # The mount point has to exist as a directory on CIRCUITPY.
    try:
        storage.mount(storage.VfsFat(disk), "/data")
    except OSError:
        storage.VfsFat.mkfs(disk)          # no filesystem yet: format once
        storage.mount(storage.VfsFat(disk), "/data")

    with open("/data/big.json", "w") as f:
        f.write(...)

Unlike CIRCUITPY, it is writable from Python while USB is connected: the host
does not see this filesystem at all. (``espidf.expose_partition()`` is the
native variant that does make it a USB drive, on boards with USB OTG.)

Flash erases in 4096-byte sectors but FAT writes 512-byte blocks, so a write
reads the whole sector, splices the block in, erases and writes it back. That is
correct but heavy: every small write rewrites a 4 kB sector, so this is for
occasional writes of large read-mostly files, not a busy filesystem. There is no
wear levelling.
"""

SECTOR = 4096


class PartitionDisk:
    """Block device over an ``espidf.Partition``, ``block_size`` bytes a block."""

    def __init__(self, partition, block_size=512):
        self._p = partition
        self._bs = block_size
        self._count = partition.size // block_size
        self._sbuf = bytearray(SECTOR)

    def readblocks(self, block_num, buf, offset=0):
        buf[:] = self._p.read(block_num * self._bs + offset, len(buf))
        return 0

    def writeblocks(self, block_num, buf, offset=0):
        addr = block_num * self._bs + offset
        length = len(buf)
        pos = 0
        while pos < length:
            a = addr + pos
            sector_start = (a // SECTOR) * SECTOR
            in_sector = a - sector_start
            chunk = min(SECTOR - in_sector, length - pos)
            if in_sector == 0 and chunk == SECTOR:
                # A whole aligned sector: nothing to preserve.
                self._sbuf[:] = buf[pos : pos + SECTOR]
            else:
                self._sbuf[:] = self._p.read(sector_start, SECTOR)
                self._sbuf[in_sector : in_sector + chunk] = buf[pos : pos + chunk]
            self._p.erase(sector_start, SECTOR)
            self._p.write(sector_start, self._sbuf)
            pos += chunk
        return 0

    def ioctl(self, op, arg):
        if op == 4:  # BLOCK_COUNT
            return self._count
        if op == 5:  # BLOCK_SIZE
            return self._bs
        if op == 6:  # BLOCK_ERASE (littlefs; FAT erases inside writeblocks)
            self._p.erase(((arg * self._bs) // SECTOR) * SECTOR, SECTOR)
            return 0
        # INIT (1), DEINIT (2), SYNC (3): writes reach flash immediately.
        return 0
