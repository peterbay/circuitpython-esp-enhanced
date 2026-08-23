// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2022 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include "shared-bindings/hashlib/__init__.h"
#include "shared-module/hashlib/__init__.h"

#include "mbedtls/ssl.h"

// CIRCUITPY-CHANGE: sha224 and sha384 are the same compression functions as
// sha256 and sha512 with different initial state and a truncated result, which
// is what the is224/is384 flag selects. They share the context accordingly.
bool common_hal_hashlib_new(hashlib_hash_obj_t *self, const char *algorithm) {
    if (strcmp(algorithm, "sha1") == 0) {
        self->hash_type = MBEDTLS_SSL_HASH_SHA1;
        mbedtls_sha1_init(&self->sha1);
        mbedtls_sha1_starts_ret(&self->sha1);
        return true;
    } else if (strcmp(algorithm, "sha256") == 0) {
        self->hash_type = MBEDTLS_SSL_HASH_SHA256;
        mbedtls_sha256_init(&self->sha256);
        mbedtls_sha256_starts_ret(&self->sha256, 0);
        return true;
    } else if (strcmp(algorithm, "sha224") == 0) {
        self->hash_type = MBEDTLS_SSL_HASH_SHA224;
        mbedtls_sha256_init(&self->sha256);
        mbedtls_sha256_starts_ret(&self->sha256, 1);
        return true;
    } else if (strcmp(algorithm, "sha512") == 0) {
        self->hash_type = MBEDTLS_SSL_HASH_SHA512;
        mbedtls_sha512_init(&self->sha512);
        mbedtls_sha512_starts_ret(&self->sha512, 0);
        return true;
    } else if (strcmp(algorithm, "sha384") == 0) {
        self->hash_type = MBEDTLS_SSL_HASH_SHA384;
        mbedtls_sha512_init(&self->sha512);
        mbedtls_sha512_starts_ret(&self->sha512, 1);
        return true;
    } else if (strcmp(algorithm, "md5") == 0) {
        // Offered for interoperability with things that still specify it, not
        // because it is a sound hash.
        self->hash_type = MBEDTLS_SSL_HASH_MD5;
        mbedtls_md5_init(&self->md5);
        mbedtls_md5_starts_ret(&self->md5);
        return true;
    }
    return false;
}
