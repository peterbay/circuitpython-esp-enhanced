# SPDX-License-Identifier: MIT
"""
partition_disk
==============

Turn an ``espidf.Partition`` into a block device you can format with
``storage.VfsFat`` and mount for normal file access.

    import espidf, storage
    from partition_disk import PartitionDisk

    disk = PartitionDisk(espidf.Partition("ota_1"))
    try:
        storage.getmount("/data")
    except OSError:
        # not mounted yet
        try:
            fs = storage.VfsFat(disk)
            storage.mount(fs, "/data")
        except Exception:
            storage.VfsFat.mkfs(disk)      # first time: format
            fs = storage.VfsFat(disk)
            storage.mount(fs, "/data")

    with open("/data/big.json", "w") as f:
        f.write(...)

Flash erases in 4096-byte sectors but FAT writes 512-byte blocks, so a write
reads the whole sector, splices the block in, erases and writes it back. That is
correct but heavy: every small write rewrites a 4 kB sector, so this is for
occasional writes of large read-mostly files, not a busy filesystem. There is no
wear levelling.
"""

SECTOR = 4096


class PartitionDisk:
    def __init__(self, partition, block_size=512):
        self._p = partition
        self._bs = block_size
        self._count = partition.size // block_size
        self._sbuf = bytearray(SECTOR)

    def readblocks(self, block_num, buf, offset=0):
        addr = block_num * self._bs + offset
        buf[:] = self._p.read(addr, len(buf))
        return 0

    def writeblocks(self, block_num, buf, offset=0):
        addr = block_num * self._bs + offset
        length = len(buf)
        pos = 0
        while pos < length:
            a = addr + pos
            sector_start = (a // SECTOR) * SECTOR
            in_sector = a - sector_start
            chunk = SECTOR - in_sector
            if chunk > length - pos:
                chunk = length - pos
            if in_sector == 0 and chunk == SECTOR:
                # A whole aligned sector: no need to preserve anything.
                self._sbuf[:] = buf[pos : pos + SECTOR]
            else:
                self._sbuf[:] = self._p.read(sector_start, SECTOR)
                self._sbuf[in_sector : in_sector + chunk] = buf[pos : pos + chunk]
            self._p.erase(sector_start, SECTOR)
            self._p.write(sector_start, self._sbuf)
            pos += chunk
        return 0

    def ioctl(self, op, arg):
        if op == 4:            # BLOCK_COUNT
            return self._count
        if op == 5:            # BLOCK_SIZE
            return self._bs
        if op == 6:            # BLOCK_ERASE (littlefs; FAT erases inside writeblocks)
            sector_start = ((arg * self._bs) // SECTOR) * SECTOR
            self._p.erase(sector_start, SECTOR)
            return 0
        # INIT (1), DEINIT (2), SYNC (3): nothing to do, flash writes are immediate.
        return 0
