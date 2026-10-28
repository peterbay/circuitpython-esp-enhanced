// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Petr Vavrin
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "extmod/vfs_fat.h"

#define PARTITION_DISK_PATH_MAX (16)

typedef enum {
    PARTITION_DISK_OK = 0,
    PARTITION_DISK_NO_PARTITION,
    PARTITION_DISK_NO_FILESYSTEM,
    PARTITION_DISK_ALREADY_MOUNTED,
    PARTITION_DISK_BAD_PATH,
    PARTITION_DISK_FORMAT_FAILED,
    // The firmware runs from it, or CIRCUITPY lives on it.
    PARTITION_DISK_PROTECTED,
} partition_disk_result_t;

// Mount a partition's FAT filesystem at path. usb_writable decides which side may
// write: true means the host writes and CircuitPython sees it read-only. Mounting
// the same partition at the same path again is a no-op, so code.py can call it on
// every reload.
bool partition_disk_mount(const char *label, const char *path, bool usb_writable,
    partition_disk_result_t *result);

// Format a partition as FAT. It must not be mounted.
bool partition_disk_format(const char *label, partition_disk_result_t *result);

// The mounted filesystem, or NULL. The USB mass storage LUN serves this.
fs_user_mount_t *partition_disk_get_usermount(void);
