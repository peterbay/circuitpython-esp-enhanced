// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2022 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include <string.h>

#include "py/obj.h"
#include "py/mpconfig.h"
#include "py/runtime.h"
#include "shared-bindings/hashlib/__init__.h"
#include "shared-bindings/hashlib/Hash.h"

//| """Hashing related functions
//|
//| |see_cpython_module| :mod:`cpython:hashlib`.
//| """
//|
//|
//| def new(name: str, data: bytes = b"") -> hashlib.Hash:
//|     """Returns a Hash object setup for the named algorithm. Raises ValueError when the named
//|     algorithm is unsupported. Supported algorithms for ``name`` are ``'md5'``,
//|     ``'sha1'``, ``'sha224'``, ``'sha256'``, ``'sha384'`` and ``'sha512'``.
//|
//|     :return: a hash object for the given algorithm
//|     :rtype: hashlib.Hash"""
//|     ...
//|
//|
static mp_obj_t hashlib_new(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_name, ARG_data };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_name, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_data,  MP_ARG_OBJ, {.u_obj = mp_const_none} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    const char *algorithm = mp_obj_str_get_str(args[ARG_name].u_obj);

    hashlib_hash_obj_t *self = mp_obj_malloc(hashlib_hash_obj_t, &hashlib_hash_type);

    if (!common_hal_hashlib_new(self, algorithm)) {
        mp_raise_ValueError(MP_ERROR_TEXT("Unsupported hash algorithm"));
    }

    if (args[ARG_data].u_obj != mp_const_none) {
        hashlib_hash_update(self, args[ARG_data].u_obj);
    }
    return self;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(hashlib_new_obj, 1, hashlib_new);

// CIRCUITPY-CHANGE: CPython's named constructors. Only new() existed, so
// `from hashlib import sha256` -- the form every portable library uses -- failed,
// and adafruit_hashlib in particular imports six names at once and drops all of
// them to pure Python if any is missing.
#define HASHLIB_CONSTRUCTOR(algorithm)                                              \
    static mp_obj_t hashlib_##algorithm(size_t n_args, const mp_obj_t *args) {      \
        hashlib_hash_obj_t *self = mp_obj_malloc(hashlib_hash_obj_t, &hashlib_hash_type); \
        if (!common_hal_hashlib_new(self, #algorithm)) {                            \
            mp_raise_ValueError(MP_ERROR_TEXT("Unsupported hash algorithm"));       \
        }                                                                           \
        if (n_args > 0) {                                                           \
            hashlib_hash_update(self, args[0]);                                     \
        }                                                                           \
        return MP_OBJ_FROM_PTR(self);                                               \
    }                                                                               \
    static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(hashlib_##algorithm##_obj, 0, 1, hashlib_##algorithm);

//| def md5(data: bytes = b"") -> hashlib.Hash:
//|     """Returns an MD5 hash object. Provided for interoperability with existing
//|     formats, not because MD5 is a sound hash."""
//|     ...
//|
//| def sha1(data: bytes = b"") -> hashlib.Hash: ...
//| def sha224(data: bytes = b"") -> hashlib.Hash: ...
//| def sha256(data: bytes = b"") -> hashlib.Hash: ...
//| def sha384(data: bytes = b"") -> hashlib.Hash: ...
//| def sha512(data: bytes = b"") -> hashlib.Hash: ...
//|
HASHLIB_CONSTRUCTOR(md5)
HASHLIB_CONSTRUCTOR(sha1)
HASHLIB_CONSTRUCTOR(sha224)
HASHLIB_CONSTRUCTOR(sha256)
HASHLIB_CONSTRUCTOR(sha384)
HASHLIB_CONSTRUCTOR(sha512)

static const mp_rom_map_elem_t hashlib_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_hashlib) },

    { MP_ROM_QSTR(MP_QSTR_new), MP_ROM_PTR(&hashlib_new_obj) },
    { MP_ROM_QSTR(MP_QSTR_md5), MP_ROM_PTR(&hashlib_md5_obj) },
    { MP_ROM_QSTR(MP_QSTR_sha1), MP_ROM_PTR(&hashlib_sha1_obj) },
    { MP_ROM_QSTR(MP_QSTR_sha224), MP_ROM_PTR(&hashlib_sha224_obj) },
    { MP_ROM_QSTR(MP_QSTR_sha256), MP_ROM_PTR(&hashlib_sha256_obj) },
    { MP_ROM_QSTR(MP_QSTR_sha384), MP_ROM_PTR(&hashlib_sha384_obj) },
    { MP_ROM_QSTR(MP_QSTR_sha512), MP_ROM_PTR(&hashlib_sha512_obj) },

    // Hash is deliberately omitted here because CPython doesn't expose the
    // object on `hashlib` only the internal `_hashlib`.
};

static MP_DEFINE_CONST_DICT(hashlib_module_globals, hashlib_module_globals_table);

const mp_obj_module_t hashlib_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&hashlib_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_hashlib, hashlib_module);
