// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2022 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include "shared-bindings/hashlib/Hash.h"
#include "shared-module/hashlib/__init__.h"

#include "py/runtime.h"
#include "psa/crypto.h"

// CIRCUITPY-CHANGE: every PSA status in this file used to be discarded. The digest
// buffer is pre-filled with zeros by the binding, so a failed clone or finish handed
// back a valid-looking all-zero digest -- the worst outcome for a signature or HMAC
// comparison, since a constant digest is trivially forgeable and the caller gets no
// signal. The hardware SHA driver on this chip does have failure paths
// (PSA_ERROR_HARDWARE_FAILURE on a DMA fault, PSA_ERROR_BAD_STATE on an inactive
// operation), so this is not merely theoretical.
static void check_psa(psa_status_t status) {
    if (status != PSA_SUCCESS) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("hash operation failed"));
    }
}

void common_hal_hashlib_hash_update(hashlib_hash_obj_t *self, const uint8_t *data, size_t datalen) {
    check_psa(psa_hash_update(&self->hash_op, data, datalen));
}

void common_hal_hashlib_hash_digest(hashlib_hash_obj_t *self, uint8_t *data, size_t datalen) {
    if (datalen < common_hal_hashlib_hash_get_digest_size(self)) {
        return;
    }
    // Clone the operation so we can continue to update or get digest again.
    psa_hash_operation_t clone = PSA_HASH_OPERATION_INIT;
    check_psa(psa_hash_clone(&self->hash_op, &clone));
    size_t hash_len;
    check_psa(psa_hash_finish(&clone, data, datalen, &hash_len));
}

size_t common_hal_hashlib_hash_get_digest_size(hashlib_hash_obj_t *self) {
    return PSA_HASH_LENGTH(self->hash_alg);
}

// CIRCUITPY-CHANGE: the block size HMAC pads its key to. It is a property of the
// algorithm rather than of the operation, so it comes from the identifier.
size_t common_hal_hashlib_hash_get_block_size(hashlib_hash_obj_t *self) {
    return PSA_HASH_BLOCK_LENGTH(self->hash_alg);
}

// CIRCUITPY-CHANGE: CPython names the algorithm on the object, and both
// adafruit_hashlib and circuitpython_hmac read it.
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

// CIRCUITPY-CHANGE: HMAC has to fork a partially fed state, and both
// adafruit_hashlib and circuitpython_hmac test for this method before using it.
// psa_hash_clone requires an inactive destination, which is what
// psa_hash_operation_init leaves behind.
void common_hal_hashlib_hash_copy(hashlib_hash_obj_t *self, hashlib_hash_obj_t *other) {
    other->hash_alg = self->hash_alg;
    other->hash_op = psa_hash_operation_init();
    check_psa(psa_hash_clone(&self->hash_op, &other->hash_op));
}
