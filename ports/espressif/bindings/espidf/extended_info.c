// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Petr Vavrin
//
// SPDX-License-Identifier: MIT

// What the chip, the firmware and the heap say about themselves; see
// extended.h.

#include <string.h>

#include "py/obj.h"
#include "py/runtime.h"

#include "bindings/espidf/__init__.h"
#include "bindings/espidf/extended.h"

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_efuse.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_system.h"

static void store_int(mp_obj_t dict, qstr key, size_t value) {
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(key), mp_obj_new_int_from_uint(value));
}

static void store_str(mp_obj_t dict, qstr key, const char *value, size_t max_len) {
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(key), mp_obj_new_str(value, strnlen(value, max_len)));
}

//| MALLOC_CAP_8BIT: int
//| MALLOC_CAP_32BIT: int
//| MALLOC_CAP_DMA: int
//| MALLOC_CAP_EXEC: int
//| MALLOC_CAP_INTERNAL: int
//| MALLOC_CAP_SPIRAM: int
//| MALLOC_CAP_DEFAULT: int
//| """Capability bits for `heap_info`, as in ESP-IDF's esp_heap_caps.h."""
//|
//| def heap_info(caps: int = MALLOC_CAP_8BIT) -> dict:
//|     """The ESP-IDF heap regions that have every bit of ``caps``, as a dict with
//|     ``total``, ``free``, ``allocated``, ``largest_free_block``,
//|     ``minimum_free`` (the lowest ``free`` has been since boot),
//|     ``allocated_blocks`` and ``free_blocks``, all in bytes or blocks.
//|
//|     The default covers PSRAM too, which on a board that has it dwarfs
//|     everything else. The Wi-Fi, Bluetooth and 802.15.4 drivers allocate from
//|     internal RAM, so ``heap_info(MALLOC_CAP_INTERNAL)`` is the figure that says
//|     whether they will start."""
//|     ...
//|
//|
static mp_obj_t espidf_heap_info(size_t n_args, const mp_obj_t *args) {
    uint32_t caps = n_args > 0 ? (uint32_t)mp_obj_get_int(args[0]) : MALLOC_CAP_8BIT;
    multi_heap_info_t info;
    heap_caps_get_info(&info, caps);

    mp_obj_t result = mp_obj_new_dict(7);
    store_int(result, MP_QSTR_total, heap_caps_get_total_size(caps));
    store_int(result, MP_QSTR_free, info.total_free_bytes);
    store_int(result, MP_QSTR_allocated, info.total_allocated_bytes);
    store_int(result, MP_QSTR_largest_free_block, info.largest_free_block);
    store_int(result, MP_QSTR_minimum_free, info.minimum_free_bytes);
    store_int(result, MP_QSTR_allocated_blocks, info.allocated_blocks);
    store_int(result, MP_QSTR_free_blocks, info.free_blocks);
    return result;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(espidf_heap_info_obj, 0, 1, espidf_heap_info);

//| def reset_reason() -> str:
//|     """Why the chip last reset, as ESP-IDF reports it: ``"POWERON"``, ``"EXT"``,
//|     ``"SW"``, ``"PANIC"``, ``"INT_WDT"``, ``"TASK_WDT"``, ``"WDT"``,
//|     ``"DEEPSLEEP"``, ``"BROWNOUT"``, ``"SDIO"``, ``"USB"``, ``"JTAG"``,
//|     ``"EFUSE"``, ``"PWR_GLITCH"``, ``"CPU_LOCKUP"`` or ``"UNKNOWN"``.
//|
//|     Finer than `microcontroller.Processor.reset_reason`, which folds several of
//|     these together. ``"PANIC"`` after an unexplained reboot means the firmware
//|     crashed; it reboots silently, so this is often the only trace."""
//|     ...
//|
//|
static mp_obj_t espidf_reset_reason(void) {
    qstr name;
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:
            name = MP_QSTR_POWERON;
            break;
        case ESP_RST_EXT:
            name = MP_QSTR_EXT;
            break;
        case ESP_RST_SW:
            name = MP_QSTR_SW;
            break;
        case ESP_RST_PANIC:
            name = MP_QSTR_PANIC;
            break;
        case ESP_RST_INT_WDT:
            name = MP_QSTR_INT_WDT;
            break;
        case ESP_RST_TASK_WDT:
            name = MP_QSTR_TASK_WDT;
            break;
        case ESP_RST_WDT:
            name = MP_QSTR_WDT;
            break;
        case ESP_RST_DEEPSLEEP:
            name = MP_QSTR_DEEPSLEEP;
            break;
        case ESP_RST_BROWNOUT:
            name = MP_QSTR_BROWNOUT;
            break;
        case ESP_RST_SDIO:
            name = MP_QSTR_SDIO;
            break;
        case ESP_RST_USB:
            name = MP_QSTR_USB;
            break;
        case ESP_RST_JTAG:
            name = MP_QSTR_JTAG;
            break;
        case ESP_RST_EFUSE:
            name = MP_QSTR_EFUSE;
            break;
        case ESP_RST_PWR_GLITCH:
            name = MP_QSTR_PWR_GLITCH;
            break;
        case ESP_RST_CPU_LOCKUP:
            name = MP_QSTR_CPU_LOCKUP;
            break;
        default:
            name = MP_QSTR_UNKNOWN;
            break;
    }
    return MP_OBJ_NEW_QSTR(name);
}
MP_DEFINE_CONST_FUN_OBJ_0(espidf_reset_reason_obj, espidf_reset_reason);

static qstr chip_model_name(esp_chip_model_t model) {
    switch (model) {
        case CHIP_ESP32:
            return MP_QSTR_ESP32;
        case CHIP_ESP32S2:
            return MP_QSTR_ESP32_hyphen_S2;
        case CHIP_ESP32S3:
            return MP_QSTR_ESP32_hyphen_S3;
        case CHIP_ESP32C2:
            return MP_QSTR_ESP32_hyphen_C2;
        case CHIP_ESP32C3:
            return MP_QSTR_ESP32_hyphen_C3;
        case CHIP_ESP32C5:
            return MP_QSTR_ESP32_hyphen_C5;
        case CHIP_ESP32C6:
            return MP_QSTR_ESP32_hyphen_C6;
        case CHIP_ESP32C61:
            return MP_QSTR_ESP32_hyphen_C61;
        case CHIP_ESP32H2:
            return MP_QSTR_ESP32_hyphen_H2;
        case CHIP_ESP32P4:
            return MP_QSTR_ESP32_hyphen_P4;
        default:
            return MP_QSTR_unknown;
    }
}

//| def chip_info() -> dict:
//|     """The chip as ESP-IDF sees it, as a dict with ``model`` (``"ESP32-C5"``
//|     and so on), ``revision`` (``"1.0"``), ``cores``, ``features`` (a tuple of
//|     ``"wifi"``, ``"ble"``, ``"bt"``, ``"ieee802154"``, ``"embedded_flash"`` and
//|     ``"embedded_psram"``, whichever apply) and ``mac``, the 6-byte base MAC
//|     address from eFuse."""
//|     ...
//|
//|
static mp_obj_t espidf_chip_info(void) {
    esp_chip_info_t info;
    esp_chip_info(&info);

    mp_obj_t result = mp_obj_new_dict(5);
    mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_model), MP_OBJ_NEW_QSTR(chip_model_name(info.model)));

    // The revision is held as major * 100 + minor.
    vstr_t revision;
    vstr_init(&revision, 8);
    vstr_printf(&revision, "%u.%u", info.revision / 100, info.revision % 100);
    mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_revision), mp_obj_new_str_from_vstr(&revision));

    store_int(result, MP_QSTR_cores, info.cores);

    static const struct {
        uint32_t bit;
        qstr name;
    } feature_names[] = {
        { CHIP_FEATURE_WIFI_BGN, MP_QSTR_wifi },
        { CHIP_FEATURE_BLE, MP_QSTR_ble },
        { CHIP_FEATURE_BT, MP_QSTR_bt },
        { CHIP_FEATURE_IEEE802154, MP_QSTR_ieee802154 },
        { CHIP_FEATURE_EMB_FLASH, MP_QSTR_embedded_flash },
        { CHIP_FEATURE_EMB_PSRAM, MP_QSTR_embedded_psram },
    };
    mp_obj_t features[MP_ARRAY_SIZE(feature_names)];
    size_t count = 0;
    for (size_t i = 0; i < MP_ARRAY_SIZE(feature_names); i++) {
        if (info.features & feature_names[i].bit) {
            features[count++] = MP_OBJ_NEW_QSTR(feature_names[i].name);
        }
    }
    mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_features), mp_obj_new_tuple(count, features));

    // Not esp_efuse_mac_get_default(): on parts with an 802.15.4 radio it writes
    // the 8-byte EUI-64.
    uint8_t mac[6];
    CHECK_ESP_RESULT(esp_read_mac(mac, ESP_MAC_BASE));
    mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_mac), mp_obj_new_bytes(mac, sizeof(mac)));
    return result;
}
MP_DEFINE_CONST_FUN_OBJ_0(espidf_chip_info_obj, espidf_chip_info);

//| def app_description() -> dict:
//|     """What the running firmware says about itself, as a dict with
//|     ``project_name``, ``version``, ``idf_version``, ``date``, ``time`` (when it
//|     was compiled) and ``elf_sha256``, the hash of the ELF file as hex -- the
//|     surest way to tell two builds apart."""
//|     ...
//|
//|
static mp_obj_t espidf_app_description(void) {
    const esp_app_desc_t *desc = esp_app_get_description();

    mp_obj_t result = mp_obj_new_dict(6);
    store_str(result, MP_QSTR_project_name, desc->project_name, sizeof(desc->project_name));
    store_str(result, MP_QSTR_version, desc->version, sizeof(desc->version));
    store_str(result, MP_QSTR_idf_version, desc->idf_ver, sizeof(desc->idf_ver));
    store_str(result, MP_QSTR_date, desc->date, sizeof(desc->date));
    store_str(result, MP_QSTR_time, desc->time, sizeof(desc->time));

    vstr_t sha;
    vstr_init(&sha, 2 * sizeof(desc->app_elf_sha256) + 1);
    for (size_t i = 0; i < sizeof(desc->app_elf_sha256); i++) {
        vstr_printf(&sha, "%02x", desc->app_elf_sha256[i]);
    }
    mp_obj_dict_store(result, MP_OBJ_NEW_QSTR(MP_QSTR_elf_sha256), mp_obj_new_str_from_vstr(&sha));
    return result;
}
MP_DEFINE_CONST_FUN_OBJ_0(espidf_app_description_obj, espidf_app_description);

//| def efuse_user_data() -> bytes:
//|     """The 32 bytes of the eFuse user data block (BLOCK3), as burned. Unburned
//|     bits read 0. Reading only: burning eFuse is irreversible and is left to
//|     espefuse.py."""
//|     ...
//|
//|
static mp_obj_t espidf_efuse_user_data(void) {
    uint8_t data[32];
    CHECK_ESP_RESULT(esp_efuse_read_block(EFUSE_BLK_USER_DATA, data, 0, sizeof(data) * 8));
    return mp_obj_new_bytes(data, sizeof(data));
}
MP_DEFINE_CONST_FUN_OBJ_0(espidf_efuse_user_data_obj, espidf_efuse_user_data);
