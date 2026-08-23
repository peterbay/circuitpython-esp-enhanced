// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2022 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include "shared-bindings/hashlib/Hash.h"
#include "shared-module/hashlib/__init__.h"

#include "mbedtls/ssl.h"

void common_hal_hashlib_hash_update(hashlib_hash_obj_t *self, const uint8_t *data, size_t datalen) {
    switch (self->hash_type) {
        case MBEDTLS_SSL_HASH_MD5:
            mbedtls_md5_update_ret(&self->md5, data, datalen);
            break;
        case MBEDTLS_SSL_HASH_SHA1:
            mbedtls_sha1_update_ret(&self->sha1, data, datalen);
            break;
        case MBEDTLS_SSL_HASH_SHA224:
        case MBEDTLS_SSL_HASH_SHA256:
            mbedtls_sha256_update_ret(&self->sha256, data, datalen);
            break;
        case MBEDTLS_SSL_HASH_SHA384:
        case MBEDTLS_SSL_HASH_SHA512:
            mbedtls_sha512_update_ret(&self->sha512, data, datalen);
            break;
        default:
            break;
    }
}

void common_hal_hashlib_hash_digest(hashlib_hash_obj_t *self, uint8_t *data, size_t datalen) {
    size_t size = common_hal_hashlib_hash_get_digest_size(self);
    if (datalen < size) {
        return;
    }
    // The state is copied so the object can keep being updated, or the digest
    // taken a second time. mbedtls_*_finish consumes the context.
    //
    // CIRCUITPY-CHANGE: sha224 and sha384 write a full 32 or 64 byte digest and
    // the caller keeps only the leading part, so finishing straight into the
    // destination would run 4 or 16 bytes past a correctly sized buffer.
    switch (self->hash_type) {
        case MBEDTLS_SSL_HASH_MD5: {
            mbedtls_md5_context copy;
            mbedtls_md5_clone(&copy, &self->md5);
            mbedtls_md5_finish_ret(&self->md5, data);
            mbedtls_md5_clone(&self->md5, &copy);
            break;
        }
        case MBEDTLS_SSL_HASH_SHA1: {
            mbedtls_sha1_context copy;
            mbedtls_sha1_clone(&copy, &self->sha1);
            mbedtls_sha1_finish_ret(&self->sha1, data);
            mbedtls_sha1_clone(&self->sha1, &copy);
            break;
        }
        case MBEDTLS_SSL_HASH_SHA224:
        case MBEDTLS_SSL_HASH_SHA256: {
            mbedtls_sha256_context copy;
            uint8_t full[32];
            mbedtls_sha256_clone(&copy, &self->sha256);
            mbedtls_sha256_finish_ret(&self->sha256, full);
            mbedtls_sha256_clone(&self->sha256, &copy);
            memcpy(data, full, size);
            break;
        }
        case MBEDTLS_SSL_HASH_SHA384:
        case MBEDTLS_SSL_HASH_SHA512: {
            mbedtls_sha512_context copy;
            uint8_t full[64];
            mbedtls_sha512_clone(&copy, &self->sha512);
            mbedtls_sha512_finish_ret(&self->sha512, full);
            mbedtls_sha512_clone(&self->sha512, &copy);
            memcpy(data, full, size);
            break;
        }
        default:
            break;
    }
}

size_t common_hal_hashlib_hash_get_digest_size(hashlib_hash_obj_t *self) {
    switch (self->hash_type) {
        case MBEDTLS_SSL_HASH_MD5:
            return 16;
        case MBEDTLS_SSL_HASH_SHA1:
            return 20;
        case MBEDTLS_SSL_HASH_SHA224:
            return 28;
        case MBEDTLS_SSL_HASH_SHA256:
            return 32;
        case MBEDTLS_SSL_HASH_SHA384:
            return 48;
        case MBEDTLS_SSL_HASH_SHA512:
            return 64;
        default:
            return 0;
    }
}

// CIRCUITPY-CHANGE: the block size HMAC pads to. It is a property of the
// algorithm, not of the context, and mbedtls does not report it.
size_t common_hal_hashlib_hash_get_block_size(hashlib_hash_obj_t *self) {
    switch (self->hash_type) {
        case MBEDTLS_SSL_HASH_SHA384:
        case MBEDTLS_SSL_HASH_SHA512:
            return 128;
        case MBEDTLS_SSL_HASH_MD5:
        case MBEDTLS_SSL_HASH_SHA1:
        case MBEDTLS_SSL_HASH_SHA224:
        case MBEDTLS_SSL_HASH_SHA256:
            return 64;
        default:
            return 0;
    }
}

const char *common_hal_hashlib_hash_get_name(hashlib_hash_obj_t *self) {
    switch (self->hash_type) {
        case MBEDTLS_SSL_HASH_MD5:
            return "md5";
        case MBEDTLS_SSL_HASH_SHA1:
            return "sha1";
        case MBEDTLS_SSL_HASH_SHA224:
            return "sha224";
        case MBEDTLS_SSL_HASH_SHA256:
            return "sha256";
        case MBEDTLS_SSL_HASH_SHA384:
            return "sha384";
        case MBEDTLS_SSL_HASH_SHA512:
            return "sha512";
        default:
            return "unknown";
    }
}

// CIRCUITPY-CHANGE: HMAC and any tree hash needs to fork a partially fed state.
// mbedtls_*_clone initialises the destination itself, so the new context must
// not be started first or its state would be overwritten anyway.
void common_hal_hashlib_hash_copy(hashlib_hash_obj_t *self, hashlib_hash_obj_t *other) {
    other->hash_type = self->hash_type;
    switch (self->hash_type) {
        case MBEDTLS_SSL_HASH_MD5:
            mbedtls_md5_init(&other->md5);
            mbedtls_md5_clone(&other->md5, &self->md5);
            break;
        case MBEDTLS_SSL_HASH_SHA1:
            mbedtls_sha1_init(&other->sha1);
            mbedtls_sha1_clone(&other->sha1, &self->sha1);
            break;
        case MBEDTLS_SSL_HASH_SHA224:
        case MBEDTLS_SSL_HASH_SHA256:
            mbedtls_sha256_init(&other->sha256);
            mbedtls_sha256_clone(&other->sha256, &self->sha256);
            break;
        case MBEDTLS_SSL_HASH_SHA384:
        case MBEDTLS_SSL_HASH_SHA512:
            mbedtls_sha512_init(&other->sha512);
            mbedtls_sha512_clone(&other->sha512, &self->sha512);
            break;
        default:
            break;
    }
}
