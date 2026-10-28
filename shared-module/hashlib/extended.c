// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Petr Vavrin
//
// SPDX-License-Identifier: MIT

#include <string.h>

#include "py/runtime.h"
#include "shared-module/hashlib/extended.h"

#include "psa/crypto.h"

// The PSA hash driver already implements these; each costs one identifier. A build
// whose PSA configuration leaves one out answers PSA_ERROR_NOT_SUPPORTED from
// psa_hash_setup(), which hashlib turns into a ValueError.
bool hashlib_extended_algorithm(const char *algorithm, psa_algorithm_t *alg) {
    if (strcmp(algorithm, "sha224") == 0) {
        *alg = PSA_ALG_SHA_224;
    } else if (strcmp(algorithm, "sha384") == 0) {
        *alg = PSA_ALG_SHA_384;
    } else if (strcmp(algorithm, "sha512") == 0) {
        *alg = PSA_ALG_SHA_512;
    } else if (strcmp(algorithm, "md5") == 0) {
        // For formats that still specify it, not because it is a sound hash.
        *alg = PSA_ALG_MD5;
    } else {
        return false;
    }
    return true;
}

// A property of the algorithm, not of the operation, so it comes from the identifier.
size_t common_hal_hashlib_hash_get_block_size(hashlib_hash_obj_t *self) {
    return PSA_HASH_BLOCK_LENGTH(self->hash_alg);
}

const char *common_hal_hashlib_hash_get_name(hashlib_hash_obj_t *self) {
    switch (self->hash_alg) {
        case PSA_ALG_MD5:
            return "md5";
        case PSA_ALG_SHA_1:
            return "sha1";
        case PSA_ALG_SHA_224:
            return "sha224";
        case PSA_ALG_SHA_256:
            return "sha256";
        case PSA_ALG_SHA_384:
            return "sha384";
        case PSA_ALG_SHA_512:
            return "sha512";
        default:
            return "unknown";
    }
}

// psa_hash_clone() wants an inactive destination, which psa_hash_operation_init()
// leaves behind.
void common_hal_hashlib_hash_copy(hashlib_hash_obj_t *self, hashlib_hash_obj_t *other) {
    other->hash_alg = self->hash_alg;
    other->hash_op = psa_hash_operation_init();
    if (psa_hash_clone(&self->hash_op, &other->hash_op) != PSA_SUCCESS) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("hash operation failed"));
    }
}
