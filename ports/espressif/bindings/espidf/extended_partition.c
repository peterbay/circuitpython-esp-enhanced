// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Petr Vavrin
//
// SPDX-License-Identifier: MIT

// espidf.Partition, partitions() and running_partition(); see extended.h.

#include <string.h>

#include "py/obj.h"
#include "py/objlist.h"
#include "py/objproperty.h"
#include "py/runtime.h"

#include "bindings/espidf/__init__.h"
#include "bindings/espidf/extended.h"

#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "spi_flash_mmap.h"

#if CIRCUITPY_PARTITION_DISK
#include "py/mperrno.h"
#include "supervisor/partition_disk.h"
#endif

// raw access to the flash partitions.
//
// The point of this is mmap(): it maps the partition into the data address space
// and hands back a memoryview over it, so a few hundred kilobytes of tables can be
// read without a single byte on the heap.
//
// Writing is guarded only against the partition the firmware is running from.
// Everything else is fair game and it is possible to destroy the filesystem with
// it, so know which partition you asked for.
//
// A mapping holds MMU pages, which run out, and it outlives the VM: one left
// mapped by a program that never called deinit() would still be there on the
// next run, and the one after. So every mapping is also tracked here and soft
// reset releases them. There is no finaliser doing the same, because the
// memoryview mmap() returns keeps no reference to the Partition: collecting the
// object would unmap memory the view still points at.
#define ESPIDF_MAX_MAPPINGS (4)
static esp_partition_mmap_handle_t espidf_mappings[ESPIDF_MAX_MAPPINGS];
static bool espidf_mapping_used[ESPIDF_MAX_MAPPINGS];

void espidf_partition_reset(void) {
    for (size_t i = 0; i < ESPIDF_MAX_MAPPINGS; i++) {
        if (espidf_mapping_used[i]) {
            esp_partition_munmap(espidf_mappings[i]);
            espidf_mapping_used[i] = false;
        }
    }
}

static void espidf_mapping_forget(esp_partition_mmap_handle_t handle) {
    for (size_t i = 0; i < ESPIDF_MAX_MAPPINGS; i++) {
        if (espidf_mapping_used[i] && espidf_mappings[i] == handle) {
            espidf_mapping_used[i] = false;
            return;
        }
    }
}

typedef struct {
    mp_obj_base_t base;
    const esp_partition_t *part;
    esp_partition_mmap_handle_t map_handle;
    bool mapped;
} espidf_partition_obj_t;

static const esp_partition_t *espidf_partition_check(espidf_partition_obj_t *self) {
    if (self->part == NULL) {
        mp_raise_ValueError(MP_ERROR_TEXT("Partition is deinitialized"));
    }
    return self->part;
}

static void espidf_partition_check_writable(const esp_partition_t *part) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running != NULL && part->address == running->address) {
        mp_raise_ValueError(MP_ERROR_TEXT("cannot write the running partition"));
    }
}

//| class Partition:
//|     """One entry of the flash partition table.
//|
//|     The second app partition, ``ota_1``, is unused unless something writes a
//|     firmware update into it (``dualbank`` does), which makes it a place to keep
//|     large read-only data. `partitions` lists the table and `running_partition`
//|     says which one to leave alone."""
//|
//|     def __init__(self, label: str) -> None:
//|         """Look the partition up by its label. Raises ValueError if there is no
//|         such partition."""
//|         ...
static mp_obj_t espidf_partition_make_new(const mp_obj_type_t *type, size_t n_args,
    size_t n_kw, const mp_obj_t *all_args) {
    mp_arg_check_num(n_args, n_kw, 1, 1, false);
    const char *label = mp_obj_str_get_str(all_args[0]);

    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_ANY,
        ESP_PARTITION_SUBTYPE_ANY, label);
    if (part == NULL) {
        mp_raise_ValueError(MP_ERROR_TEXT("no such partition"));
    }

    espidf_partition_obj_t *self = mp_obj_malloc(espidf_partition_obj_t, &espidf_partition_type);
    self->part = part;
    self->mapped = false;
    return MP_OBJ_FROM_PTR(self);
}

//|     def read(self, offset: int, length: int) -> bytes:
//|         """Read from the partition."""
//|         ...
static mp_obj_t espidf_partition_read(mp_obj_t self_in, mp_obj_t offset_in, mp_obj_t length_in) {
    espidf_partition_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const esp_partition_t *part = espidf_partition_check(self);
    size_t offset = mp_obj_get_int(offset_in);
    size_t length = mp_obj_get_int(length_in);
    if (offset > part->size || length > part->size - offset) {
        mp_raise_ValueError(MP_ERROR_TEXT("read past the end of the partition"));
    }
    vstr_t vstr;
    vstr_init_len(&vstr, length);
    CHECK_ESP_RESULT(esp_partition_read(part, offset, vstr.buf, length));
    return mp_obj_new_bytes_from_vstr(&vstr);
}
static MP_DEFINE_CONST_FUN_OBJ_3(espidf_partition_read_obj, espidf_partition_read);

//|     def write(self, offset: int, data: ReadableBuffer) -> None:
//|         """Write to the partition. The area has to be erased first: flash can only
//|         turn ones into zeros."""
//|         ...
static mp_obj_t espidf_partition_write(mp_obj_t self_in, mp_obj_t offset_in, mp_obj_t data_in) {
    espidf_partition_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const esp_partition_t *part = espidf_partition_check(self);
    espidf_partition_check_writable(part);
    size_t offset = mp_obj_get_int(offset_in);
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(data_in, &bufinfo, MP_BUFFER_READ);
    if (offset > part->size || bufinfo.len > part->size - offset) {
        mp_raise_ValueError(MP_ERROR_TEXT("write past the end of the partition"));
    }
    CHECK_ESP_RESULT(esp_partition_write(part, offset, bufinfo.buf, bufinfo.len));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(espidf_partition_write_obj, espidf_partition_write);

//|     def erase(self, offset: int = 0, length: int | None = None) -> None:
//|         """Erase part of the partition, whole 4096 byte sectors only. Erases all
//|         of it when called without arguments."""
//|         ...
static mp_obj_t espidf_partition_erase(size_t n_args, const mp_obj_t *args) {
    espidf_partition_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    const esp_partition_t *part = espidf_partition_check(self);
    espidf_partition_check_writable(part);
    size_t offset = n_args > 1 ? (size_t)mp_obj_get_int(args[1]) : 0;
    size_t length = n_args > 2 ? (size_t)mp_obj_get_int(args[2]) : part->size - offset;
    if ((offset % SPI_FLASH_SEC_SIZE) != 0 || (length % SPI_FLASH_SEC_SIZE) != 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("offset and length must be a multiple of 4096"));
    }
    if (offset > part->size || length > part->size - offset) {
        mp_raise_ValueError(MP_ERROR_TEXT("erase past the end of the partition"));
    }
    CHECK_ESP_RESULT(esp_partition_erase_range(part, offset, length));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(espidf_partition_erase_obj, 1, 3, espidf_partition_erase);

//|     def mmap(self) -> memoryview:
//|         """Map the partition into the address space and return a read only
//|         memoryview over it. Nothing is copied and nothing lands on the heap, so
//|         this is the way to read tables that would not fit there.
//|
//|         The mapping is released by `deinit`."""
//|         ...
static mp_obj_t espidf_partition_mmap(mp_obj_t self_in) {
    espidf_partition_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const esp_partition_t *part = espidf_partition_check(self);
    if (self->mapped) {
        mp_raise_ValueError(MP_ERROR_TEXT("already mapped"));
    }
    size_t slot = 0;
    while (slot < ESPIDF_MAX_MAPPINGS && espidf_mapping_used[slot]) {
        slot++;
    }
    if (slot == ESPIDF_MAX_MAPPINGS) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("too many mapped partitions"));
    }
    const void *ptr = NULL;
    CHECK_ESP_RESULT(esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA,
        &ptr, &self->map_handle));
    espidf_mappings[slot] = self->map_handle;
    espidf_mapping_used[slot] = true;
    self->mapped = true;
    // Without MP_OBJ_ARRAY_TYPECODE_FLAG_RW the memoryview refuses to be written,
    // which is what we want: the mapping really is read only.
    return mp_obj_new_memoryview('B', part->size, (void *)ptr);
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_partition_mmap_obj, espidf_partition_mmap);

//|     def deinit(self) -> None:
//|         """Release the mapping, if there is one. Any memoryview `mmap` returned
//|         must not be used afterwards."""
//|         ...
static mp_obj_t espidf_partition_deinit(mp_obj_t self_in) {
    espidf_partition_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->mapped) {
        esp_partition_munmap(self->map_handle);
        espidf_mapping_forget(self->map_handle);
        self->mapped = false;
    }
    self->part = NULL;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_partition_deinit_obj, espidf_partition_deinit);

static mp_obj_t espidf_partition_get_label(mp_obj_t self_in) {
    espidf_partition_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const esp_partition_t *part = espidf_partition_check(self);
    return mp_obj_new_str(part->label, strlen(part->label));
}
MP_DEFINE_CONST_FUN_OBJ_1(espidf_partition_get_label_obj, espidf_partition_get_label);
MP_PROPERTY_GETTER(espidf_partition_label_obj, (mp_obj_t)&espidf_partition_get_label_obj);

static mp_obj_t espidf_partition_get_size(mp_obj_t self_in) {
    espidf_partition_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_int_from_uint(espidf_partition_check(self)->size);
}
MP_DEFINE_CONST_FUN_OBJ_1(espidf_partition_get_size_obj, espidf_partition_get_size);
MP_PROPERTY_GETTER(espidf_partition_size_obj, (mp_obj_t)&espidf_partition_get_size_obj);

static mp_obj_t espidf_partition_get_address(mp_obj_t self_in) {
    espidf_partition_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_int_from_uint(espidf_partition_check(self)->address);
}
MP_DEFINE_CONST_FUN_OBJ_1(espidf_partition_get_address_obj, espidf_partition_get_address);
MP_PROPERTY_GETTER(espidf_partition_address_obj, (mp_obj_t)&espidf_partition_get_address_obj);

static const mp_rom_map_elem_t espidf_partition_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&espidf_partition_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&espidf_partition_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_erase), MP_ROM_PTR(&espidf_partition_erase_obj) },
    { MP_ROM_QSTR(MP_QSTR_mmap), MP_ROM_PTR(&espidf_partition_mmap_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&espidf_partition_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR_label), MP_ROM_PTR(&espidf_partition_label_obj) },
    { MP_ROM_QSTR(MP_QSTR_size), MP_ROM_PTR(&espidf_partition_size_obj) },
    { MP_ROM_QSTR(MP_QSTR_address), MP_ROM_PTR(&espidf_partition_address_obj) },
};
static MP_DEFINE_CONST_DICT(espidf_partition_locals_dict, espidf_partition_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    espidf_partition_type,
    MP_QSTR_Partition,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, espidf_partition_make_new,
    locals_dict, &espidf_partition_locals_dict
    );

//| def partitions() -> List[Tuple[str, int, int, int, int]]:
//|     """Every entry of the partition table as
//|     ``(label, type, subtype, address, size)``."""
//|     ...
//|
//|
static mp_obj_t espidf_partitions(void) {
    mp_obj_t list = mp_obj_new_list(0, NULL);
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY,
        ESP_PARTITION_SUBTYPE_ANY, NULL);
    while (it != NULL) {
        const esp_partition_t *part = esp_partition_get(it);
        mp_obj_t entry[5] = {
            mp_obj_new_str(part->label, strlen(part->label)),
            MP_OBJ_NEW_SMALL_INT(part->type),
            MP_OBJ_NEW_SMALL_INT(part->subtype),
            mp_obj_new_int_from_uint(part->address),
            mp_obj_new_int_from_uint(part->size),
        };
        mp_obj_list_append(list, mp_obj_new_tuple(5, entry));
        it = esp_partition_next(it);
    }
    esp_partition_iterator_release(it);
    return list;
}
MP_DEFINE_CONST_FUN_OBJ_0(espidf_partitions_obj, espidf_partitions);

//| def running_partition() -> str:
//|     """The label of the partition the firmware is running from. Writing to it
//|     is refused, so this is the one to avoid when picking a partition for data.
//|     Detect it with this, never by trying to erase."""
//|     ...
//|
//|
static mp_obj_t espidf_running_partition(void) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL) {
        return mp_const_none;
    }
    return mp_obj_new_str(running->label, strlen(running->label));
}
MP_DEFINE_CONST_FUN_OBJ_0(espidf_running_partition_obj, espidf_running_partition);

#if CIRCUITPY_PARTITION_DISK
//| def expose_partition(
//|     label: str, path: str = "/data", *, usb_writable: bool = False, format: bool = False
//| ) -> None:
//|     """Mount the FAT filesystem on a partition at ``path`` and make it a second USB
//|     drive.
//|
//|     Call it from ``boot.py``: CircuitPython presents its USB drives to the host
//|     after boot.py runs, so a drive added later is mounted for Python but never
//|     seen by the host. The mount stays for code.py and across reloads, and calling
//|     it again with the same arguments does nothing.
//|
//|     ``usb_writable`` picks the side that writes, because only one may write a FAT
//|     filesystem at a time. False (the default) lets CircuitPython write -- for
//|     logging -- and the host sees the drive read-only. True lets the host write and
//|     CircuitPython sees it read-only.
//|
//|     ``format`` formats the partition first, destroying what was on it. Do it once.
//|
//|     Raises ValueError for an unknown partition, for the one the firmware runs
//|     from and for the one holding CIRCUITPY, and OSError (ENODEV) when the
//|     partition holds no filesystem yet."""
//|     ...
//|
//|
static mp_obj_t espidf_expose_partition(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_label, ARG_path, ARG_usb_writable, ARG_format };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_label, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_path, MP_ARG_OBJ, { .u_obj = MP_ROM_NONE } },
        { MP_QSTR_usb_writable, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = false } },
        { MP_QSTR_format, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = false } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    const char *label = mp_obj_str_get_str(args[ARG_label].u_obj);
    const char *path = args[ARG_path].u_obj == MP_ROM_NONE
        ? "/data" : mp_obj_str_get_str(args[ARG_path].u_obj);

    partition_disk_result_t result = PARTITION_DISK_OK;
    if (!args[ARG_format].u_bool || partition_disk_format(label, &result)) {
        partition_disk_mount(label, path, args[ARG_usb_writable].u_bool, &result);
    }
    switch (result) {
        case PARTITION_DISK_OK:
            return mp_const_none;
        case PARTITION_DISK_NO_PARTITION:
            mp_raise_ValueError(MP_ERROR_TEXT("no such partition"));
        case PARTITION_DISK_PROTECTED:
            mp_raise_ValueError(MP_ERROR_TEXT("partition in use by the firmware or CIRCUITPY"));
        case PARTITION_DISK_BAD_PATH:
            mp_raise_ValueError(MP_ERROR_TEXT("path must be absolute and short"));
        case PARTITION_DISK_ALREADY_MOUNTED:
            mp_raise_ValueError(MP_ERROR_TEXT("another partition is already exposed"));
        case PARTITION_DISK_NO_FILESYSTEM:
            mp_raise_OSError(MP_ENODEV);
        case PARTITION_DISK_FORMAT_FAILED:
        default:
            mp_raise_OSError(MP_EIO);
    }
}
MP_DEFINE_CONST_FUN_OBJ_KW(espidf_expose_partition_obj, 1, espidf_expose_partition);
#endif
