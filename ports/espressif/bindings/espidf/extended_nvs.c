// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Petr Vavrin
//
// SPDX-License-Identifier: MIT

// espidf.NVS; see extended.h.

#include <string.h>

#include "py/obj.h"
#include "py/objlist.h"
#include "py/runtime.h"

#include "bindings/espidf/__init__.h"
#include "bindings/espidf/extended.h"

#include "nvs.h"
#include "nvs_flash.h"

// named key/value storage, the way the ESP-IDF actually keeps
// it. microcontroller.nvm gives one flat 8 kB array addressed by offset; this is
// the layer under it, with typed values under names, wear levelling and atomic
// writes. Uses a namespace other than the "CPY" that nvm claims.
typedef struct {
    mp_obj_base_t base;
    nvs_handle_t handle;
    char ns_name[NVS_NS_NAME_MAX_SIZE];
    bool open;
} espidf_nvs_obj_t;

//| class NVS:
//|     """A namespace in the ESP-IDF non-volatile storage. Keys are strings up to
//|     15 characters. Values are integers, strings or bytes. Writes are committed
//|     immediately."""
//|
//|     def __init__(self, namespace: str) -> None:
//|         """Open, creating it if needed."""
//|         ...
static mp_obj_t espidf_nvs_make_new(const mp_obj_type_t *type, size_t n_args,
    size_t n_kw, const mp_obj_t *all_args) {
    mp_arg_check_num(n_args, n_kw, 1, 1, false);
    const char *ns = mp_obj_str_get_str(all_args[0]);

    // nvm's get_nvs_handle does this init dance; do it here too in case NVS was
    // never touched yet.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    CHECK_ESP_RESULT(err);

    if (strlen(ns) >= NVS_NS_NAME_MAX_SIZE) {
        mp_raise_ValueError(MP_ERROR_TEXT("namespace too long"));
    }
    // With a finaliser, so that a namespace nobody closed is closed when the
    // object is collected or the VM resets, instead of holding its handle for
    // the rest of the session.
    espidf_nvs_obj_t *self = mp_obj_malloc_with_finaliser(espidf_nvs_obj_t, &espidf_nvs_type);
    CHECK_ESP_RESULT(nvs_open(ns, NVS_READWRITE, &self->handle));
    strcpy(self->ns_name, ns);
    self->open = true;
    return MP_OBJ_FROM_PTR(self);
}

static nvs_handle_t espidf_nvs_get(espidf_nvs_obj_t *self) {
    if (!self->open) {
        mp_raise_ValueError(MP_ERROR_TEXT("NVS is deinitialized"));
    }
    return self->handle;
}

//|     def __getitem__(self, key: str) -> int | str | bytes:
//|         """Read a value. Raises KeyError if the key is not set. The returned type
//|         is whatever was stored."""
//|         ...
static mp_obj_t espidf_nvs_subscr(mp_obj_t self_in, mp_obj_t key_in, mp_obj_t value) {
    espidf_nvs_obj_t *self = MP_OBJ_TO_PTR(self_in);
    nvs_handle_t handle = espidf_nvs_get(self);
    const char *key = mp_obj_str_get_str(key_in);
    // Checked here rather than left to the ESP-IDF, whose answer is a bare
    // "ESP-IDF error 0x1109".
    if (strlen(key) >= NVS_KEY_NAME_MAX_SIZE) {
        mp_raise_ValueError_varg(MP_ERROR_TEXT("key longer than %d characters"), NVS_KEY_NAME_MAX_SIZE - 1);
    }

    if (value == MP_OBJ_SENTINEL) {
        // load: an integer wins over a blob under the same key, but this module
        // only ever writes one type per key, so a lookup of each is enough.
        int64_t i64;
        if (nvs_get_i64(handle, key, &i64) == ESP_OK) {
            return mp_obj_new_int_from_ll(i64);
        }
        size_t size = 0;
        if (nvs_get_str(handle, key, NULL, &size) == ESP_OK) {
            vstr_t vstr;
            vstr_init_len(&vstr, size - 1); // size counts the null
            nvs_get_str(handle, key, vstr.buf, &size);
            return mp_obj_new_str_from_vstr(&vstr);
        }
        if (nvs_get_blob(handle, key, NULL, &size) == ESP_OK) {
            vstr_t vstr;
            vstr_init_len(&vstr, size);
            nvs_get_blob(handle, key, vstr.buf, &size);
            return mp_obj_new_bytes_from_vstr(&vstr);
        }
        mp_raise_type_arg(&mp_type_KeyError, key_in);
    } else if (value == MP_OBJ_NULL) {
        // delete
        esp_err_t err = nvs_erase_key(handle, key);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            mp_raise_type_arg(&mp_type_KeyError, key_in);
        }
        CHECK_ESP_RESULT(err);
        CHECK_ESP_RESULT(nvs_commit(handle));
        return mp_const_none;
    } else {
        // store. Erase first, so switching the type of a key does not leave the
        // old value shadowing the new one.
        nvs_erase_key(handle, key);
        if (mp_obj_is_int(value)) {
            CHECK_ESP_RESULT(nvs_set_i64(handle, key, mp_obj_get_ll(value)));
        } else if (mp_obj_is_str(value)) {
            CHECK_ESP_RESULT(nvs_set_str(handle, key, mp_obj_str_get_str(value)));
        } else {
            mp_buffer_info_t bufinfo;
            mp_get_buffer_raise(value, &bufinfo, MP_BUFFER_READ);
            CHECK_ESP_RESULT(nvs_set_blob(handle, key, bufinfo.buf, bufinfo.len));
        }
        CHECK_ESP_RESULT(nvs_commit(handle));
        return mp_const_none;
    }
}

//|     def keys(self) -> List[str]:
//|         """Every key in this namespace."""
//|         ...
static mp_obj_t espidf_nvs_keys(mp_obj_t self_in) {
    espidf_nvs_obj_t *self = MP_OBJ_TO_PTR(self_in);
    espidf_nvs_get(self);
    mp_obj_t list = mp_obj_new_list(0, NULL);
    nvs_iterator_t it = NULL;
    esp_err_t res = nvs_entry_find(NVS_DEFAULT_PART_NAME, self->ns_name, NVS_TYPE_ANY, &it);
    while (res == ESP_OK) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        mp_obj_list_append(list, mp_obj_new_str(info.key, strlen(info.key)));
        res = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    return list;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_nvs_keys_obj, espidf_nvs_keys);

//|     def deinit(self) -> None:
//|         """Close the namespace."""
//|         ...
static mp_obj_t espidf_nvs_deinit(mp_obj_t self_in) {
    espidf_nvs_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->open) {
        nvs_close(self->handle);
        self->open = false;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_nvs_deinit_obj, espidf_nvs_deinit);

static const mp_rom_map_elem_t espidf_nvs_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_keys), MP_ROM_PTR(&espidf_nvs_keys_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&espidf_nvs_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&espidf_nvs_deinit_obj) },
};
static MP_DEFINE_CONST_DICT(espidf_nvs_locals_dict, espidf_nvs_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    espidf_nvs_type,
    MP_QSTR_NVS,
    MP_TYPE_FLAG_NONE,
    make_new, espidf_nvs_make_new,
    subscr, espidf_nvs_subscr,
    locals_dict, &espidf_nvs_locals_dict
    );
