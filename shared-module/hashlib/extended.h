// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Petr Vavrin
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "psa/crypto.h"
#include "shared-module/hashlib/Hash.h"

// Maps the algorithms upstream does not know -- md5, sha224, sha384, sha512 -- to
// their PSA identifier. common_hal_hashlib_new() asks here after its own list.
bool hashlib_extended_algorithm(const char *algorithm, psa_algorithm_t *alg);

size_t common_hal_hashlib_hash_get_block_size(hashlib_hash_obj_t *self);
const char *common_hal_hashlib_hash_get_name(hashlib_hash_obj_t *self);
void common_hal_hashlib_hash_copy(hashlib_hash_obj_t *self, hashlib_hash_obj_t *other);
