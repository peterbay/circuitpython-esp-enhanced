// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Petr Vavrin
//
// SPDX-License-Identifier: MIT

#pragma once

// The parts of CPython's hashlib that upstream lacks: the named constructors,
// hexdigest(), copy(), name and block_size. They live in extended.c; the two
// upstream tables take them in through the macros below, one line each.

#include "py/obj.h"
#include "py/objproperty.h"

#ifndef CIRCUITPY_HASHLIB_EXTENDED
#define CIRCUITPY_HASHLIB_EXTENDED (0)
#endif

#if CIRCUITPY_HASHLIB_EXTENDED

MP_DECLARE_CONST_FUN_OBJ_1(hashlib_hash_hexdigest_obj);
MP_DECLARE_CONST_FUN_OBJ_1(hashlib_hash_copy_obj);
extern const mp_obj_property_getter_t hashlib_hash_block_size_obj;
extern const mp_obj_property_getter_t hashlib_hash_name_obj;

MP_DECLARE_CONST_FUN_OBJ_VAR_BETWEEN(hashlib_md5_obj);
MP_DECLARE_CONST_FUN_OBJ_VAR_BETWEEN(hashlib_sha1_obj);
MP_DECLARE_CONST_FUN_OBJ_VAR_BETWEEN(hashlib_sha224_obj);
MP_DECLARE_CONST_FUN_OBJ_VAR_BETWEEN(hashlib_sha256_obj);
MP_DECLARE_CONST_FUN_OBJ_VAR_BETWEEN(hashlib_sha384_obj);
MP_DECLARE_CONST_FUN_OBJ_VAR_BETWEEN(hashlib_sha512_obj);

#define HASHLIB_HASH_EXTENDED_LOCALS \
    { MP_ROM_QSTR(MP_QSTR_block_size), MP_ROM_PTR(&hashlib_hash_block_size_obj) }, \
    { MP_ROM_QSTR(MP_QSTR_name), MP_ROM_PTR(&hashlib_hash_name_obj) }, \
    { MP_ROM_QSTR(MP_QSTR_hexdigest), MP_ROM_PTR(&hashlib_hash_hexdigest_obj) }, \
    { MP_ROM_QSTR(MP_QSTR_copy), MP_ROM_PTR(&hashlib_hash_copy_obj) },

#define HASHLIB_EXTENDED_GLOBALS \
    { MP_ROM_QSTR(MP_QSTR_md5), MP_ROM_PTR(&hashlib_md5_obj) }, \
    { MP_ROM_QSTR(MP_QSTR_sha1), MP_ROM_PTR(&hashlib_sha1_obj) }, \
    { MP_ROM_QSTR(MP_QSTR_sha224), MP_ROM_PTR(&hashlib_sha224_obj) }, \
    { MP_ROM_QSTR(MP_QSTR_sha256), MP_ROM_PTR(&hashlib_sha256_obj) }, \
    { MP_ROM_QSTR(MP_QSTR_sha384), MP_ROM_PTR(&hashlib_sha384_obj) }, \
    { MP_ROM_QSTR(MP_QSTR_sha512), MP_ROM_PTR(&hashlib_sha512_obj) },

#else

#define HASHLIB_HASH_EXTENDED_LOCALS
#define HASHLIB_EXTENDED_GLOBALS

#endif
