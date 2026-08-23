// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2022 Jeff Epler for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#pragma once

#include "mbedtls/md5.h"
#include "mbedtls/sha1.h"
#include "mbedtls/sha256.h"
#include "mbedtls/sha512.h"

// CIRCUITPY-CHANGE: only sha1 and sha256 were here, and only through
// hashlib.new(). adafruit_hashlib asks for the CPython constructors in one
// import, so a single missing name dropped every algorithm in it -- and
// everything built on it -- into its pure Python implementations. SHA-256 of
// 4 kB then took 2.2 s instead of 238 us. mbedtls already carries md5 and
// sha512 here, so covering the set costs little.
typedef struct {
    mp_obj_base_t base;
    union {
        mbedtls_md5_context md5;
        mbedtls_sha1_context sha1;
        mbedtls_sha256_context sha256;
        mbedtls_sha512_context sha512;
    };
    // Of MBEDTLS_SSL_HASH_*
    uint8_t hash_type;
} hashlib_hash_obj_t;
