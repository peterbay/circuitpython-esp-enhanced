// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Petr Vavrin
//
// SPDX-License-Identifier: MIT

#pragma once

#include "py/obj.h"

// What this firmware adds to the espidf module. None of it is listed in
// __init__.c: extended.c registers a delegation for the module, which the runtime
// asks for any name the module's own table does not have. That keeps the
// upstream file untouched.

// Not static: the generated moduledefs.h declares it extern.
void espidf_extended_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest);

// Called from reset_port() on every soft reset: takes down whatever the ESP-IDF
// still holds that points into the Python heap, before the heap goes.
void espidf_extended_reset(void);

// Accepts a str as its UTF-8 bytes, or anything with a buffer; raises TypeError
// naming the argument otherwise.
void espidf_get_bytes(mp_obj_t obj, qstr arg, mp_buffer_info_t *bufinfo);

// extended_system.c
extern const mp_obj_fun_builtin_fixed_t espidf_cycles_obj;
extern const mp_obj_fun_builtin_fixed_t espidf_get_time_us_obj;
extern const mp_obj_fun_builtin_fixed_t espidf_task_stats_obj;
extern const mp_obj_fun_builtin_fixed_t espidf_check_heap_obj;
extern const mp_obj_fun_builtin_fixed_t espidf_pin_status_obj;
extern const mp_obj_fun_builtin_var_t espidf_power_management_obj;

// extended_info.c
extern const mp_obj_fun_builtin_var_t espidf_heap_info_obj;
extern const mp_obj_fun_builtin_fixed_t espidf_reset_reason_obj;
extern const mp_obj_fun_builtin_fixed_t espidf_chip_info_obj;
extern const mp_obj_fun_builtin_fixed_t espidf_app_description_obj;
extern const mp_obj_fun_builtin_fixed_t espidf_efuse_user_data_obj;

// extended_timer.c
extern const mp_obj_type_t espidf_timer_type;
void espidf_timer_reset(void);

// extended_nvs.c
extern const mp_obj_type_t espidf_nvs_type;

// extended_partition.c
extern const mp_obj_type_t espidf_partition_type;
extern const mp_obj_fun_builtin_fixed_t espidf_partitions_obj;
extern const mp_obj_fun_builtin_fixed_t espidf_running_partition_obj;
void espidf_partition_reset(void);
#if CIRCUITPY_PARTITION_DISK
extern const mp_obj_fun_builtin_var_t espidf_expose_partition_obj;
#endif

// extended_crypto.c
extern const mp_obj_fun_builtin_var_t espidf_pbkdf2_obj;
extern const mp_obj_fun_builtin_fixed_t espidf_hmac_sha256_obj;
extern const mp_obj_fun_builtin_var_t espidf_aes_gcm_encrypt_obj;
extern const mp_obj_fun_builtin_var_t espidf_aes_gcm_decrypt_obj;

// extended_eventqueue.c
extern const mp_obj_type_t espidf_event_queue_type;
void espidf_event_reset(void);

// extended_wifi.c
#if CIRCUITPY_WIFI
extern const mp_obj_fun_builtin_var_t espidf_set_vendor_ie_obj;
extern const mp_obj_fun_builtin_var_t espidf_eap_enable_obj;
extern const mp_obj_fun_builtin_fixed_t espidf_eap_disable_obj;
extern const mp_obj_fun_builtin_var_t espidf_smartconfig_start_obj;
extern const mp_obj_fun_builtin_fixed_t espidf_smartconfig_result_obj;
extern const mp_obj_fun_builtin_fixed_t espidf_smartconfig_stop_obj;
extern const mp_obj_fun_builtin_var_t espidf_wifi_raw_tx_obj;
extern const mp_obj_fun_builtin_fixed_t espidf_wifi_sleep_min_active_time_obj;
void espidf_eap_reset(void);
void espidf_smartconfig_reset(void);
#endif

// extended_sdm.c
#if CIRCUITPY_ESPIDF_SDM
extern const mp_obj_type_t espidf_sdm_type;
void espidf_sdm_reset(void);
#endif

// extended_mcpwm.c
#if CIRCUITPY_ESPIDF_MCPWM
extern const mp_obj_type_t espidf_mcpwm_type;
void espidf_mcpwm_reset(void);
#endif

// extended_parlio.c
#if CIRCUITPY_ESPIDF_PARLIO
extern const mp_obj_type_t espidf_parallel_out_type;
extern const mp_obj_type_t espidf_parallel_in_type;
void espidf_parlio_reset(void);
#endif

// extended_etm.c
#if CIRCUITPY_ESPIDF_ETM
#include "esp_etm.h"
extern const mp_obj_type_t espidf_pinlink_type;
void espidf_etm_reset(void);
// A pin set by one ETM event and cleared by another (swapped with invert),
// through two ETM channels and the GPIO set/clear tasks. The pin becomes an
// input-output starting at `initial`. On error everything made is undone.
typedef struct {
    esp_etm_channel_handle_t chan[2];
    esp_etm_task_handle_t task_set;
    esp_etm_task_handle_t task_clr;
    int target;
} espidf_etm_drive_t;
esp_err_t espidf_etm_drive_start(espidf_etm_drive_t *d, esp_etm_event_handle_t up,
    esp_etm_event_handle_t down, int target, bool invert, int initial);
void espidf_etm_drive_stop(espidf_etm_drive_t *d);
#endif

// extended_ana_cmpr.c
#if CIRCUITPY_ESPIDF_ANA_CMPR
extern const mp_obj_type_t espidf_ana_cmpr_type;
void espidf_ana_cmpr_reset(void);
#endif

// extended_ftm.c
#if CIRCUITPY_ESPIDF_FTM
extern const mp_obj_fun_builtin_var_t espidf_ftm_obj;
extern const mp_obj_fun_builtin_var_t espidf_ftm_responder_obj;
extern const mp_obj_fun_builtin_fixed_t espidf_ftm_scan_obj;
void espidf_ftm_reset(void);
#endif

// extended_csi.c
#if CIRCUITPY_ESPIDF_CSI
extern const mp_obj_type_t espidf_csi_type;
void espidf_csi_reset(void);
#endif

// extended_ble.c
#if CIRCUITPY_BLEIO_NATIVE
extern const mp_obj_fun_builtin_var_t espidf_ble_advertising_phy_obj;
extern const mp_obj_fun_builtin_fixed_t espidf_ble_prefer_phy_obj;
#endif
