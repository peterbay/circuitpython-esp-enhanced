// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Petr Vavrin
//
// SPDX-License-Identifier: MIT

// espidf.pbkdf2(), hmac_sha256(), aes_gcm_encrypt() and aes_gcm_decrypt();
// see extended.h.

#include <string.h>

#include "py/obj.h"
#include "py/runtime.h"

#include "bindings/espidf/__init__.h"
#include "bindings/espidf/extended.h"

#include "mbedtls/md.h"
#include "psa/crypto.h"
#include "soc/soc_caps.h"

// the crypto the ESP-IDF already has. aesio is tiny-AES-c, a
// portable software implementation, and there is no HMAC or PBKDF2 in the build
// at all -- doing those in Python costs two interpreter round trips per hash
// iteration, which is what makes a key derivation take seconds. mbedtls is
// already linked here for TLS and CONFIG_MBEDTLS_HARDWARE_AES/SHA are on, so
// exposing it is nearly free in flash.
//
// The tag is carried inside the ciphertext rather than returned beside it, so
// decryption cannot be asked for without it. Forgetting to check the tag is the
// usual way authenticated encryption gets misused.

//| def pbkdf2(
//|     password: str | ReadableBuffer,
//|     salt: str | ReadableBuffer,
//|     iterations: int,
//|     key_length: int = 32,
//| ) -> bytes:
//|     """Derive a key from a password with PBKDF2-HMAC-SHA256.
//|
//|     About 100 us per iteration on an ESP32-C5 at 240 MHz, against 1.2 ms for
//|     the same computation in Python, so iteration counts that actually slow an
//|     attacker down become practical. It runs to the end without returning to
//|     Python, and cannot be interrupted: 100000 iterations take some ten
//|     seconds."""
//|     ...
static mp_obj_t espidf_pbkdf2(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_password, ARG_salt, ARG_iterations, ARG_key_length };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_password, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_salt, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_iterations, MP_ARG_REQUIRED | MP_ARG_INT },
        { MP_QSTR_key_length, MP_ARG_INT, {.u_int = 32} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_buffer_info_t password, salt;
    espidf_get_bytes(args[ARG_password].u_obj, MP_QSTR_password, &password);
    espidf_get_bytes(args[ARG_salt].u_obj, MP_QSTR_salt, &salt);
    // An iteration count of zero derives nothing, and the cap keeps a typo from
    // locking the board up for hours with no way to interrupt it.
    mp_int_t iterations = mp_arg_validate_int_range(args[ARG_iterations].u_int,
        1, 10000000, MP_QSTR_iterations);
    mp_int_t key_length = mp_arg_validate_int_range(args[ARG_key_length].u_int,
        1, 1024, MP_QSTR_key_length);

    // mbedtls 4 moved pkcs5.h behind mbedtls/private, so the
    // derivation runs through PSA. PBKDF2 requires its inputs in this order:
    // cost, salt, password.
    if (psa_crypto_init() != PSA_SUCCESS) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("key derivation failed"));
    }
    psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
    psa_status_t status = psa_key_derivation_setup(&op, PSA_ALG_PBKDF2_HMAC(PSA_ALG_SHA_256));
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_input_integer(&op, PSA_KEY_DERIVATION_INPUT_COST,
            (uint64_t)iterations);
    }
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT,
            salt.buf, salt.len);
    }
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_PASSWORD,
            password.buf, password.len);
    }

    vstr_t vstr;
    vstr_init_len(&vstr, key_length);
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_output_bytes(&op, (uint8_t *)vstr.buf, key_length);
    }
    psa_key_derivation_abort(&op);
    if (status != PSA_SUCCESS) {
        vstr_clear(&vstr);
        mp_raise_RuntimeError(MP_ERROR_TEXT("key derivation failed"));
    }
    return mp_obj_new_bytes_from_vstr(&vstr);
}
MP_DEFINE_CONST_FUN_OBJ_KW(espidf_pbkdf2_obj, 3, espidf_pbkdf2);

//| def hmac_sha256(key: str | ReadableBuffer, data: str | ReadableBuffer) -> bytes:
//|     """HMAC-SHA256 of ``data`` under ``key``. Returns 32 bytes."""
//|     ...
static mp_obj_t espidf_hmac_sha256(mp_obj_t key_in, mp_obj_t data_in) {
    mp_buffer_info_t key, data;
    espidf_get_bytes(key_in, MP_QSTR_key, &key);
    espidf_get_bytes(data_in, MP_QSTR_data, &data);

    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (info == NULL) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("SHA256 unavailable"));
    }
    vstr_t vstr;
    vstr_init_len(&vstr, 32);
    int ret = mbedtls_md_hmac(info, key.buf, key.len, data.buf, data.len,
        (unsigned char *)vstr.buf);
    if (ret != 0) {
        vstr_clear(&vstr);
        mp_raise_RuntimeError(MP_ERROR_TEXT("HMAC failed"));
    }
    return mp_obj_new_bytes_from_vstr(&vstr);
}
MP_DEFINE_CONST_FUN_OBJ_2(espidf_hmac_sha256_obj, espidf_hmac_sha256);

#define ESPIDF_GCM_TAG_LEN (16)

// mbedtls 4 moved gcm.h behind mbedtls/private, so this goes
// through PSA. The key is held in the PSA key store only for the one call and
// destroyed before returning.
static mbedtls_svc_key_id_t espidf_gcm_import_key(mp_obj_t key_in, psa_key_usage_t usage) {
    mp_buffer_info_t key;
    espidf_get_bytes(key_in, MP_QSTR_key, &key);
    if (key.len != 16 && key.len != 24 && key.len != 32) {
        mp_raise_ValueError(MP_ERROR_TEXT("Key must be 16, 24, or 32 bytes long"));
    }
    #if !defined(SOC_AES_SUPPORT_AES_192)
    // Only the original ESP32 and the S2 have 192 bit keys in the AES peripheral.
    // Without this the failure surfaces as an opaque import error.
    if (key.len == 24) {
        mp_raise_ValueError(MP_ERROR_TEXT("AES-192 is not supported by this chip"));
    }
    #endif
    if (psa_crypto_init() != PSA_SUCCESS) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("AES setkey failed"));
    }
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attributes, usage);
    psa_set_key_algorithm(&attributes, PSA_ALG_GCM);
    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    mbedtls_svc_key_id_t key_id = 0;
    if (psa_import_key(&attributes, key.buf, key.len, &key_id) != PSA_SUCCESS) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("AES setkey failed"));
    }
    return key_id;
}

//| def aes_gcm_encrypt(
//|     key: ReadableBuffer,
//|     nonce: ReadableBuffer,
//|     data: str | ReadableBuffer,
//|     associated_data: str | ReadableBuffer = b"",
//| ) -> bytes:
//|     """Encrypt with AES-GCM. Returns the ciphertext with its 16 byte
//|     authentication tag appended, which is what `aes_gcm_decrypt` expects.
//|
//|     ``key`` is 16 or 32 bytes; the AES peripheral on this chip has no 192 bit
//|     key. ``nonce`` must never repeat for a given key;
//|     12 random bytes from `os.urandom` is the usual choice. ``associated_data`` is
//|     authenticated but not encrypted."""
//|     ...
static mp_obj_t espidf_aes_gcm_encrypt(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_key, ARG_nonce, ARG_data, ARG_associated_data };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_key, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_nonce, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_data, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_associated_data, MP_ARG_OBJ, {.u_obj = mp_const_none} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_buffer_info_t nonce, data, aad;
    espidf_get_bytes(args[ARG_nonce].u_obj, MP_QSTR_nonce, &nonce);
    espidf_get_bytes(args[ARG_data].u_obj, MP_QSTR_data, &data);
    aad.buf = NULL;
    aad.len = 0;
    if (args[ARG_associated_data].u_obj != mp_const_none) {
        espidf_get_bytes(args[ARG_associated_data].u_obj, MP_QSTR_associated_data, &aad);
    }
    if (nonce.len < 1) {
        mp_raise_ValueError(MP_ERROR_TEXT("nonce must not be empty"));
    }

    mbedtls_svc_key_id_t key_id = espidf_gcm_import_key(args[ARG_key].u_obj,
        PSA_KEY_USAGE_ENCRYPT);

    vstr_t vstr;
    vstr_init_len(&vstr, data.len + ESPIDF_GCM_TAG_LEN);
    // psa_aead_encrypt appends the tag to the ciphertext itself, which is the
    // layout aes_gcm_decrypt expects.
    size_t out_len = 0;
    psa_status_t status = psa_aead_encrypt(key_id, PSA_ALG_GCM,
        nonce.buf, nonce.len, aad.buf, aad.len, data.buf, data.len,
        (uint8_t *)vstr.buf, vstr.len, &out_len);
    psa_destroy_key(key_id);
    if (status != PSA_SUCCESS || out_len != vstr.len) {
        vstr_clear(&vstr);
        mp_raise_RuntimeError(MP_ERROR_TEXT("encryption failed"));
    }
    return mp_obj_new_bytes_from_vstr(&vstr);
}
MP_DEFINE_CONST_FUN_OBJ_KW(espidf_aes_gcm_encrypt_obj, 3, espidf_aes_gcm_encrypt);

//| def aes_gcm_decrypt(
//|     key: ReadableBuffer,
//|     nonce: ReadableBuffer,
//|     data: ReadableBuffer,
//|     associated_data: str | ReadableBuffer = b"",
//| ) -> bytes:
//|     """Decrypt what `aes_gcm_encrypt` produced, checking the appended tag.
//|
//|     Raises `ValueError` if the tag does not match, which means a wrong key, a
//|     wrong nonce, different associated data, or tampering. Nothing is returned in
//|     that case, so unauthenticated plaintext never reaches the caller."""
//|     ...
static mp_obj_t espidf_aes_gcm_decrypt(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_key, ARG_nonce, ARG_data, ARG_associated_data };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_key, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_nonce, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_data, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_associated_data, MP_ARG_OBJ, {.u_obj = mp_const_none} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_buffer_info_t nonce, data, aad;
    espidf_get_bytes(args[ARG_nonce].u_obj, MP_QSTR_nonce, &nonce);
    espidf_get_bytes(args[ARG_data].u_obj, MP_QSTR_data, &data);
    aad.buf = NULL;
    aad.len = 0;
    if (args[ARG_associated_data].u_obj != mp_const_none) {
        espidf_get_bytes(args[ARG_associated_data].u_obj, MP_QSTR_associated_data, &aad);
    }
    if (nonce.len < 1) {
        mp_raise_ValueError(MP_ERROR_TEXT("nonce must not be empty"));
    }
    if (data.len < ESPIDF_GCM_TAG_LEN) {
        mp_raise_ValueError(MP_ERROR_TEXT("data is too short to hold a tag"));
    }
    size_t body = data.len - ESPIDF_GCM_TAG_LEN;

    mbedtls_svc_key_id_t key_id = espidf_gcm_import_key(args[ARG_key].u_obj,
        PSA_KEY_USAGE_DECRYPT);

    vstr_t vstr;
    vstr_init_len(&vstr, body);
    // PSA reads the tag from the tail of the ciphertext, so the whole buffer
    // goes in rather than the body and the tag separately.
    size_t out_len = 0;
    psa_status_t status = psa_aead_decrypt(key_id, PSA_ALG_GCM,
        nonce.buf, nonce.len, aad.buf, aad.len, data.buf, data.len,
        (uint8_t *)vstr.buf, body, &out_len);
    psa_destroy_key(key_id);
    if (status != PSA_SUCCESS || out_len != body) {
        // The plaintext is discarded rather than returned, so a caller that
        // ignores the exception still cannot act on unauthenticated data.
        memset(vstr.buf, 0, body);
        vstr_clear(&vstr);
        mp_raise_ValueError(MP_ERROR_TEXT("authentication failed"));
    }
    return mp_obj_new_bytes_from_vstr(&vstr);
}
MP_DEFINE_CONST_FUN_OBJ_KW(espidf_aes_gcm_decrypt_obj, 3, espidf_aes_gcm_decrypt);
