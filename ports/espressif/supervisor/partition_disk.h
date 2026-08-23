// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "extmod/vfs_fat.h"

#ifndef CIRCUITPY_PARTITION_DISK
#define CIRCUITPY_PARTITION_DISK (0)
#endif

#define PARTITION_DISK_PATH_MAX (16)

struct esp_partition_t;

typedef enum {
    PARTITION_DISK_OK = 0,
    PARTITION_DISK_NO_PARTITION,
    PARTITION_DISK_NO_FILESYSTEM,
    PARTITION_DISK_ALREADY_MOUNTED,
    PARTITION_DISK_BAD_PATH,
    PARTITION_DISK_FORMAT_FAILED,
} partition_disk_result_t;

// Mount a partition's FAT filesystem at path. usb_writable decides which side
// may write: true means the host writes and CircuitPython sees it read-only.
bool partition_disk_mount(const char *label, const char *path, bool usb_writable,
    partition_disk_result_t *result);

// Format a partition as FAT. It must not be mounted.
bool partition_disk_format(const char *label, partition_disk_result_t *result);

// The mounted filesystem, or NULL. Used by the mass storage LUN.
fs_user_mount_t *partition_disk_get_usermount(void);

// Where it is mounted, or NULL.
const char *partition_disk_get_path(void);

void partition_disk_reset(void);
