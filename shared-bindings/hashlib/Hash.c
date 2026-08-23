// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2022 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include "shared-bindings/hashlib/Hash.h"

#include "py/obj.h"
#include "py/objproperty.h"
#include "py/objstr.h"
#include "py/runtime.h"

//| class Hash:
//|     """In progress hash algorithm. This object is always created by a `hashlib.new()`. It has no
//|     user-visible constructor."""
//|

//|     digest_size: int
//|     """Digest size in bytes"""
//|
static mp_obj_t hashlib_hash_digest_size_get(mp_obj_t self_in) {
    mp_check_self(mp_obj_is_type(self_in, &hashlib_hash_type));
    hashlib_hash_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return MP_OBJ_NEW_SMALL_INT(common_hal_hashlib_hash_get_digest_size(self));
}
MP_DEFINE_CONST_FUN_OBJ_1(hashlib_hash_digest_size_get_obj, hashlib_hash_digest_size_get);
MP_PROPERTY_GETTER(hashlib_hash_digest_size_obj, (mp_obj_t)&hashlib_hash_digest_size_get_obj);

//|     def update(self, data: ReadableBuffer) -> None:
//|         """Update the hash with the given bytes.
//|
//|         :param ~circuitpython_typing.ReadableBuffer data: Update the hash from data in this buffer
//|         """
//|         ...
//|
mp_obj_t hashlib_hash_update(mp_obj_t self_in, mp_obj_t buf_in) {
    mp_check_self(mp_obj_is_type(self_in, &hashlib_hash_type));
    hashlib_hash_obj_t *self = MP_OBJ_TO_PTR(self_in);

    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(buf_in, &bufinfo, MP_BUFFER_READ);

    common_hal_hashlib_hash_update(self, bufinfo.buf, bufinfo.len);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(hashlib_hash_update_obj, hashlib_hash_update);

//|     def digest(self) -> bytes:
//|         """Returns the current digest as bytes() with a length of `hashlib.Hash.digest_size`."""
//|         ...
//|
//|
static mp_obj_t hashlib_hash_digest(mp_obj_t self_in) {
    mp_check_self(mp_obj_is_type(self_in, &hashlib_hash_type));
    hashlib_hash_obj_t *self = MP_OBJ_TO_PTR(self_in);

    size_t size = common_hal_hashlib_hash_get_digest_size(self);
    mp_obj_t obj = mp_obj_new_bytes_of_zeros(size);
    mp_obj_str_t *o = MP_OBJ_TO_PTR(obj);

    common_hal_hashlib_hash_digest(self, (uint8_t *)o->data, size);
    return obj;
}
static MP_DEFINE_CONST_FUN_OBJ_1(hashlib_hash_digest_obj, hashlib_hash_digest);

// CIRCUITPY-CHANGE: block_size, name, hexdigest() and copy() are what CPython's
// hashlib objects carry and what HMAC implementations reach for. Without them a
// library that checks for them falls back to hashing in Python, which is three
// orders of magnitude slower here.

//|     block_size: int
//|     """Block size the algorithm pads to, in bytes. 64 for md5, sha1, sha224
//|     and sha256; 128 for sha384 and sha512."""
static mp_obj_t hashlib_hash_block_size_get(mp_obj_t self_in) {
    mp_check_self(mp_obj_is_type(self_in, &hashlib_hash_type));
    hashlib_hash_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return MP_OBJ_NEW_SMALL_INT(common_hal_hashlib_hash_get_block_size(self));
}
MP_DEFINE_CONST_FUN_OBJ_1(hashlib_hash_block_size_get_obj, hashlib_hash_block_size_get);
MP_PROPERTY_GETTER(hashlib_hash_block_size_obj, (mp_obj_t)&hashlib_hash_block_size_get_obj);

//|     name: str
//|     """Canonical name of the algorithm, such as ``"sha256"``."""
static mp_obj_t hashlib_hash_name_get(mp_obj_t self_in) {
    mp_check_self(mp_obj_is_type(self_in, &hashlib_hash_type));
    hashlib_hash_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const char *name = common_hal_hashlib_hash_get_name(self);
    return mp_obj_new_str(name, strlen(name));
}
MP_DEFINE_CONST_FUN_OBJ_1(hashlib_hash_name_get_obj, hashlib_hash_name_get);
MP_PROPERTY_GETTER(hashlib_hash_name_obj, (mp_obj_t)&hashlib_hash_name_get_obj);

//|     def hexdigest(self) -> str:
//|         """Returns the digest as a string of hexadecimal digits."""
//|         ...
static mp_obj_t hashlib_hash_hexdigest(mp_obj_t self_in) {
    mp_check_self(mp_obj_is_type(self_in, &hashlib_hash_type));
    hashlib_hash_obj_t *self = MP_OBJ_TO_PTR(self_in);

    size_t size = common_hal_hashlib_hash_get_digest_size(self);
    uint8_t digest[64];
    if (size > sizeof(digest)) {
        mp_raise_ValueError(MP_ERROR_TEXT("Unsupported hash algorithm"));
    }
    common_hal_hashlib_hash_digest(self, digest, size);

    vstr_t vstr;
    vstr_init_len(&vstr, size * 2);
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < size; i++) {
        vstr.buf[i * 2] = hex[digest[i] >> 4];
        vstr.buf[i * 2 + 1] = hex[digest[i] & 0xf];
    }
    return mp_obj_new_str_from_vstr(&vstr);
}
static MP_DEFINE_CONST_FUN_OBJ_1(hashlib_hash_hexdigest_obj, hashlib_hash_hexdigest);

//|     def copy(self) -> Hash:
//|         """Returns a separate hash carrying the same state, so the two can be
//|         fed differently from here on."""
//|         ...
static mp_obj_t hashlib_hash_copy(mp_obj_t self_in) {
    mp_check_self(mp_obj_is_type(self_in, &hashlib_hash_type));
    hashlib_hash_obj_t *self = MP_OBJ_TO_PTR(self_in);
    hashlib_hash_obj_t *other = mp_obj_malloc(hashlib_hash_obj_t, &hashlib_hash_type);
    common_hal_hashlib_hash_copy(self, other);
    return MP_OBJ_FROM_PTR(other);
}
static MP_DEFINE_CONST_FUN_OBJ_1(hashlib_hash_copy_obj, hashlib_hash_copy);

static const mp_rom_map_elem_t hashlib_hash_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_digest_size), MP_ROM_PTR(&hashlib_hash_digest_size_obj) },
    { MP_ROM_QSTR(MP_QSTR_block_size), MP_ROM_PTR(&hashlib_hash_block_size_obj) },
    { MP_ROM_QSTR(MP_QSTR_name), MP_ROM_PTR(&hashlib_hash_name_obj) },
    { MP_ROM_QSTR(MP_QSTR_update), MP_ROM_PTR(&hashlib_hash_update_obj) },
    { MP_ROM_QSTR(MP_QSTR_digest), MP_ROM_PTR(&hashlib_hash_digest_obj) },
    { MP_ROM_QSTR(MP_QSTR_hexdigest), MP_ROM_PTR(&hashlib_hash_hexdigest_obj) },
    { MP_ROM_QSTR(MP_QSTR_copy), MP_ROM_PTR(&hashlib_hash_copy_obj) },
};

static MP_DEFINE_CONST_DICT(hashlib_hash_locals_dict, hashlib_hash_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    hashlib_hash_type,
    MP_QSTR_Hash,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    locals_dict, &hashlib_hash_locals_dict
    );
