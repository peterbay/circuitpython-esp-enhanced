// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2022 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include "shared-bindings/hashlib/Hash.h"
#include "shared-module/hashlib/__init__.h"

#include "py/runtime.h"
#include "psa/crypto.h"

// CIRCUITPY-CHANGE: every PSA status here used to be discarded. The binding pre-fills
// the digest with zeros, so a failed clone or finish handed back a valid-looking
// all-zero digest: a constant, forgeable value for a signature or HMAC check, with
// no signal to the caller. The hardware SHA drivers do have failure paths.
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
