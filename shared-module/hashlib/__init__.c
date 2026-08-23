// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2022 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include "shared-bindings/hashlib/__init__.h"
#include "shared-module/hashlib/__init__.h"

#include "psa/crypto.h"

// CIRCUITPY-CHANGE: sha1 and sha256 were the only two accepted here. The rest of
// the set costs one PSA algorithm identifier each, because the driver behind it
// already implements them.
bool common_hal_hashlib_new(hashlib_hash_obj_t *self, const char *algorithm) {
    if (strcmp(algorithm, "sha1") == 0) {
        self->hash_alg = PSA_ALG_SHA_1;
    } else if (strcmp(algorithm, "sha224") == 0) {
        self->hash_alg = PSA_ALG_SHA_224;
    } else if (strcmp(algorithm, "sha256") == 0) {
        self->hash_alg = PSA_ALG_SHA_256;
    } else if (strcmp(algorithm, "sha384") == 0) {
        self->hash_alg = PSA_ALG_SHA_384;
    } else if (strcmp(algorithm, "sha512") == 0) {
        self->hash_alg = PSA_ALG_SHA_512;
    } else if (strcmp(algorithm, "md5") == 0) {
        // Offered for interoperability with things that still specify it, not
        // because it is a sound hash.
        self->hash_alg = PSA_ALG_MD5;
    } else {
        return false;
    }
    // PSA has to be initialized before any of it is used. OK to psa_crypto_init()
    // multiple times. On espressif, ESP-IDF has already done this during its own system
    // init and this call is a no-op.
    if (psa_crypto_init() != PSA_SUCCESS) {
        return false;
    }
    self->hash_op = psa_hash_operation_init();
    // A build whose PSA configuration leaves one of these out answers
    // PSA_ERROR_NOT_SUPPORTED here, which the caller turns into a ValueError.
    return psa_hash_setup(&self->hash_op, self->hash_alg) == PSA_SUCCESS;
}
