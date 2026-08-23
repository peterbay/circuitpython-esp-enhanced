// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2022 Jeff Epler for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#pragma once

#include "psa/crypto.h"

// CIRCUITPY-CHANGE: only sha1 and sha256 were here, and only through
// hashlib.new(). adafruit_hashlib asks for the CPython constructors in one
// import, so a single missing name dropped every algorithm in it -- and
// everything built on it -- into its pure Python implementations. SHA-256 of
// 4 kB then took 2.2 s instead of 238 us. PSA already carries md5, sha224,
// sha384 and sha512, so covering the set costs little.
typedef struct {
    mp_obj_base_t base;
    psa_hash_operation_t hash_op;
    psa_algorithm_t hash_alg;
} hashlib_hash_obj_t;
