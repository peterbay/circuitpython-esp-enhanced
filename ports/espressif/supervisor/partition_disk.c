// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-License-Identifier: MIT

// CIRCUITPY-CHANGE: a native block device over an ESP-IDF flash partition, so a
// partition can hold a FAT filesystem that USB mass storage can also serve.
//
// It has to be native (C) and live outside the VM heap: MSC reads and writes
// blocks from the USB task, with no interpreter around, and the mount has to
// survive an auto-reload.
//
// Flash erases in 4096-byte sectors while FAT writes 512-byte blocks, so a write
// that does not cover a whole sector reads the sector, splices the blocks in,
// erases and writes it back. There is no wear levelling: this is meant for
// occasional writes of mostly-read data, such as logs or lookup tables.

#include <string.h>

#include "supervisor/partition_disk.h"

#if CIRCUITPY_PARTITION_DISK

#include "esp_partition.h"

#include "extmod/vfs_fat.h"
#include "py/mperrno.h"
#include "py/runtime.h"
#include "supervisor/filesystem.h"

#define SECTOR_SIZE (4096)
#define BLOCK_SIZE  (512)

// One slot is enough: the point is a single extra USB drive. The state is static
// so that it outlives the VM.
static const esp_partition_t *_partition;
static fs_user_mount_t _disk_usermount;
static mp_vfs_mount_t _disk_vfs;
static char _disk_path[PARTITION_DISK_PATH_MAX];
static uint8_t _sector_buf[SECTOR_SIZE];

static mp_uint_t partition_disk_read_blocks(mp_obj_t self_in, uint8_t *dest,
    uint32_t block_num, uint32_t num_blocks) {
    (void)self_in;
    const esp_partition_t *part = _partition;
    if (part == NULL) {
        return 1;
    }
    size_t offset = (size_t)block_num * BLOCK_SIZE;
    size_t length = (size_t)num_blocks * BLOCK_SIZE;
    if (offset + length > part->size) {
        return 1;
    }
    return esp_partition_read(part, offset, dest, length) == ESP_OK ? 0 : 1;
}

static mp_uint_t partition_disk_write_blocks(mp_obj_t self_in, const uint8_t *src,
    uint32_t block_num, uint32_t num_blocks) {
    (void)self_in;
    const esp_partition_t *part = _partition;
    if (part == NULL) {
        return 1;
    }
    size_t offset = (size_t)block_num * BLOCK_SIZE;
    size_t length = (size_t)num_blocks * BLOCK_SIZE;
    if (offset + length > part->size) {
        return 1;
    }

    size_t pos = 0;
    while (pos < length) {
        size_t address = offset + pos;
        size_t sector_start = (address / SECTOR_SIZE) * SECTOR_SIZE;
        size_t in_sector = address - sector_start;
        size_t chunk = SECTOR_SIZE - in_sector;
        if (chunk > length - pos) {
            chunk = length - pos;
        }
        if (in_sector == 0 && chunk == SECTOR_SIZE) {
            // A whole aligned sector: nothing to preserve.
            memcpy(_sector_buf, src + pos, SECTOR_SIZE);
        } else {
            if (esp_partition_read(part, sector_start, _sector_buf, SECTOR_SIZE) != ESP_OK) {
                return 1;
            }
            memcpy(_sector_buf + in_sector, src + pos, chunk);
        }
        if (esp_partition_erase_range(part, sector_start, SECTOR_SIZE) != ESP_OK) {
            return 1;
        }
        if (esp_partition_write(part, sector_start, _sector_buf, SECTOR_SIZE) != ESP_OK) {
            return 1;
        }
        pos += chunk;
    }
    return 0;
}

static bool partition_disk_ioctl(mp_obj_t self_in, size_t cmd, size_t arg, mp_int_t *out_value) {
    (void)self_in;
    (void)arg;
    const esp_partition_t *part = _partition;
    if (out_value != NULL) {
        *out_value = 0;
    }
    if (part == NULL) {
        return false;
    }
    switch (cmd) {
        case MP_BLOCKDEV_IOCTL_INIT:
        case MP_BLOCKDEV_IOCTL_DEINIT:
        case MP_BLOCKDEV_IOCTL_SYNC:
            // Writes reach flash immediately, so there is nothing to flush.
            break;
        case MP_BLOCKDEV_IOCTL_BLOCK_COUNT:
            *out_value = part->size / BLOCK_SIZE;
            break;
        case MP_BLOCKDEV_IOCTL_BLOCK_SIZE:
            *out_value = BLOCK_SIZE;
            break;
        default:
            return false;
    }
    return true;
}

fs_user_mount_t *partition_disk_get_usermount(void) {
    if (_partition == NULL) {
        return NULL;
    }
    return &_disk_usermount;
}

const char *partition_disk_get_path(void) {
    return _partition == NULL ? NULL : _disk_path;
}

bool partition_disk_mount(const char *label, const char *path, bool usb_writable,
    partition_disk_result_t *result) {
    *result = PARTITION_DISK_OK;

    if (_partition != NULL) {
        *result = PARTITION_DISK_ALREADY_MOUNTED;
        return false;
    }
    size_t path_len = strlen(path);
    if (path_len == 0 || path_len >= PARTITION_DISK_PATH_MAX || path[0] != '/') {
        *result = PARTITION_DISK_BAD_PATH;
        return false;
    }

    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_ANY,
        ESP_PARTITION_SUBTYPE_ANY, label);
    if (part == NULL) {
        *result = PARTITION_DISK_NO_PARTITION;
        return false;
    }

    fs_user_mount_t *vfs = &_disk_usermount;
    memset(vfs, 0, sizeof(fs_user_mount_t));
    vfs->base.type = &mp_fat_vfs_type;
    vfs->blockdev.flags = MP_BLOCKDEV_FLAG_NATIVE | MP_BLOCKDEV_FLAG_HAVE_IOCTL;
    vfs->blockdev.block_size = BLOCK_SIZE;
    vfs->fatfs.drv = vfs;
    // Whole-device filesystem, no partition table in front of it.
    vfs->fatfs.part = 0;
    vfs->blockdev.readblocks[0] = mp_const_none;
    vfs->blockdev.readblocks[1] = (mp_obj_t)&vfs->blockdev;
    vfs->blockdev.readblocks[2] = (mp_obj_t)partition_disk_read_blocks;
    vfs->blockdev.writeblocks[0] = mp_const_none;
    vfs->blockdev.writeblocks[1] = (mp_obj_t)&vfs->blockdev;
    vfs->blockdev.writeblocks[2] = (mp_obj_t)partition_disk_write_blocks;
    vfs->blockdev.u.ioctl[0] = mp_const_none;
    vfs->blockdev.u.ioctl[1] = (mp_obj_t)&vfs->blockdev;
    vfs->blockdev.u.ioctl[2] = (mp_obj_t)partition_disk_ioctl;

    _partition = part;

    FRESULT res = f_mount(&vfs->fatfs);
    if (res != FR_OK) {
        _partition = NULL;
        *result = PARTITION_DISK_NO_FILESYSTEM;
        return false;
    }

    // Only one side may write a FAT filesystem at a time.
    filesystem_set_concurrent_write_protection(vfs, true);
    filesystem_set_writable_by_usb(vfs, usb_writable);

    memcpy(_disk_path, path, path_len + 1);
    mp_vfs_mount_t *mount = &_disk_vfs;
    mount->str = _disk_path;
    mount->len = path_len;
    mount->obj = MP_OBJ_FROM_PTR(vfs);
    mount->next = MP_STATE_VM(vfs_mount_table);
    MP_STATE_VM(vfs_mount_table) = mount;
    return true;
}

bool partition_disk_format(const char *label, partition_disk_result_t *result) {
    *result = PARTITION_DISK_OK;
    if (_partition != NULL) {
        *result = PARTITION_DISK_ALREADY_MOUNTED;
        return false;
    }
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_ANY,
        ESP_PARTITION_SUBTYPE_ANY, label);
    if (part == NULL) {
        *result = PARTITION_DISK_NO_PARTITION;
        return false;
    }

    // Set the device up just far enough to run mkfs through it.
    fs_user_mount_t *vfs = &_disk_usermount;
    memset(vfs, 0, sizeof(fs_user_mount_t));
    vfs->base.type = &mp_fat_vfs_type;
    vfs->blockdev.flags = MP_BLOCKDEV_FLAG_NATIVE | MP_BLOCKDEV_FLAG_HAVE_IOCTL;
    vfs->blockdev.block_size = BLOCK_SIZE;
    vfs->fatfs.drv = vfs;
    vfs->fatfs.part = 0;
    vfs->blockdev.readblocks[0] = mp_const_none;
    vfs->blockdev.readblocks[1] = (mp_obj_t)&vfs->blockdev;
    vfs->blockdev.readblocks[2] = (mp_obj_t)partition_disk_read_blocks;
    vfs->blockdev.writeblocks[0] = mp_const_none;
    vfs->blockdev.writeblocks[1] = (mp_obj_t)&vfs->blockdev;
    vfs->blockdev.writeblocks[2] = (mp_obj_t)partition_disk_write_blocks;
    vfs->blockdev.u.ioctl[0] = mp_const_none;
    vfs->blockdev.u.ioctl[1] = (mp_obj_t)&vfs->blockdev;
    vfs->blockdev.u.ioctl[2] = (mp_obj_t)partition_disk_ioctl;

    _partition = part;
    // mkfs writes through the block device, so it must look writable.
    filesystem_set_concurrent_write_protection(vfs, false);

    uint8_t working_buf[FF_MAX_SS];
    FRESULT res = f_mkfs(&vfs->fatfs, FM_FAT | FM_SFD, 0, working_buf, sizeof(working_buf));
    _partition = NULL;
    if (res != FR_OK) {
        *result = PARTITION_DISK_FORMAT_FAILED;
        return false;
    }
    return true;
}

void partition_disk_reset(void) {
    // The mount table is rebuilt for each VM, so drop our entry with it. The
    // partition itself stays claimed only while mounted.
    _partition = NULL;
}

#endif // CIRCUITPY_PARTITION_DISK
