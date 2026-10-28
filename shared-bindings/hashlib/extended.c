// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Petr Vavrin
//
// SPDX-License-Identifier: MIT

#include <string.h>

#include "py/obj.h"
#include "py/objproperty.h"
#include "py/objstr.h"
#include "py/runtime.h"
#include "shared-bindings/hashlib/__init__.h"
#include "shared-bindings/hashlib/Hash.h"
#include "shared-bindings/hashlib/extended.h"
#include "shared-module/hashlib/extended.h"

// hashlib.Hash: block_size, name, hexdigest() and copy() are what CPython's hash
// objects carry. Portable code -- HMAC and KDF implementations above all -- reads
// them, and a library that finds one missing either fails or falls back to hashing
// in Python, three orders of magnitude slower here.

// The block the algorithm pads to: 64 bytes for md5, sha1, sha224 and sha256,
// 128 for sha384 and sha512.
static mp_obj_t hashlib_hash_block_size_get(mp_obj_t self_in) {
    mp_check_self(mp_obj_is_type(self_in, &hashlib_hash_type));
    hashlib_hash_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return MP_OBJ_NEW_SMALL_INT(common_hal_hashlib_hash_get_block_size(self));
}
MP_DEFINE_CONST_FUN_OBJ_1(hashlib_hash_block_size_get_obj, hashlib_hash_block_size_get);
MP_PROPERTY_GETTER(hashlib_hash_block_size_obj, (mp_obj_t)&hashlib_hash_block_size_get_obj);

// The canonical name, as hashlib.new() takes it.
static mp_obj_t hashlib_hash_name_get(mp_obj_t self_in) {
    mp_check_self(mp_obj_is_type(self_in, &hashlib_hash_type));
    hashlib_hash_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const char *name = common_hal_hashlib_hash_get_name(self);
    return mp_obj_new_str(name, strlen(name));
}
MP_DEFINE_CONST_FUN_OBJ_1(hashlib_hash_name_get_obj, hashlib_hash_name_get);
MP_PROPERTY_GETTER(hashlib_hash_name_obj, (mp_obj_t)&hashlib_hash_name_get_obj);

static mp_obj_t hashlib_hash_hexdigest(mp_obj_t self_in) {
    mp_check_self(mp_obj_is_type(self_in, &hashlib_hash_type));
    hashlib_hash_obj_t *self = MP_OBJ_TO_PTR(self_in);

    size_t size = common_hal_hashlib_hash_get_digest_size(self);
    // sha512's 64 bytes are the longest digest PSA hashes produce.
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
MP_DEFINE_CONST_FUN_OBJ_1(hashlib_hash_hexdigest_obj, hashlib_hash_hexdigest);

// A separate hash carrying the same state, so the two can be fed differently from
// here on. HMAC forks its keyed inner and outer states this way.
static mp_obj_t hashlib_hash_copy(mp_obj_t self_in) {
    mp_check_self(mp_obj_is_type(self_in, &hashlib_hash_type));
    hashlib_hash_obj_t *self = MP_OBJ_TO_PTR(self_in);
    hashlib_hash_obj_t *other = mp_obj_malloc(hashlib_hash_obj_t, &hashlib_hash_type);
    common_hal_hashlib_hash_copy(self, other);
    return MP_OBJ_FROM_PTR(other);
}
MP_DEFINE_CONST_FUN_OBJ_1(hashlib_hash_copy_obj, hashlib_hash_copy);

// hashlib.md5() ... hashlib.sha512(): only new() existed, so
// `from hashlib import sha256` -- the form most portable code uses -- failed.
// Each takes an optional first chunk of data, as new() does.
#define HASHLIB_CONSTRUCTOR(algorithm) \
    static mp_obj_t hashlib_##algorithm(size_t n_args, const mp_obj_t *args) { \
        hashlib_hash_obj_t *self = mp_obj_malloc(hashlib_hash_obj_t, &hashlib_hash_type); \
        if (!common_hal_hashlib_new(self, #algorithm)) { \
            mp_raise_ValueError(MP_ERROR_TEXT("Unsupported hash algorithm")); \
        } \
        if (n_args > 0) { \
            hashlib_hash_update(self, args[0]); \
        } \
        return MP_OBJ_FROM_PTR(self); \
    } \
    MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(hashlib_##algorithm##_obj, 0, 1, hashlib_##algorithm);

HASHLIB_CONSTRUCTOR(md5)
HASHLIB_CONSTRUCTOR(sha1)
HASHLIB_CONSTRUCTOR(sha224)
HASHLIB_CONSTRUCTOR(sha256)
HASHLIB_CONSTRUCTOR(sha384)
HASHLIB_CONSTRUCTOR(sha512)
