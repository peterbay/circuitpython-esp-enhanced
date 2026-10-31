// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Petr Vavrin
//
// SPDX-License-Identifier: MIT

#include "py/obj.h"
#include "py/runtime.h"

#include "bindings/espidf/extended.h"

#include "esp_event.h"
#include "esp_heap_caps.h"

#if CIRCUITPY_WIFI
#include "esp_eap_client.h"
#include "esp_smartconfig.h"
#endif

#if CIRCUITPY_BLEIO_NATIVE
#include "common-hal/_bleio/extended_adv.h"
#endif

extern const mp_obj_module_t espidf_module;

static const mp_rom_map_elem_t espidf_extended_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_cycles), MP_ROM_PTR(&espidf_cycles_obj) },
    // The same counter under the name it had first.
    { MP_ROM_QSTR(MP_QSTR_get_cycle_count), MP_ROM_PTR(&espidf_cycles_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_time_us), MP_ROM_PTR(&espidf_get_time_us_obj) },
    { MP_ROM_QSTR(MP_QSTR_task_stats), MP_ROM_PTR(&espidf_task_stats_obj) },
    { MP_ROM_QSTR(MP_QSTR_check_heap), MP_ROM_PTR(&espidf_check_heap_obj) },
    { MP_ROM_QSTR(MP_QSTR_pin_status), MP_ROM_PTR(&espidf_pin_status_obj) },
    { MP_ROM_QSTR(MP_QSTR_power_management), MP_ROM_PTR(&espidf_power_management_obj) },

    { MP_ROM_QSTR(MP_QSTR_heap_info), MP_ROM_PTR(&espidf_heap_info_obj) },
    { MP_ROM_QSTR(MP_QSTR_MALLOC_CAP_8BIT), MP_ROM_INT(MALLOC_CAP_8BIT) },
    { MP_ROM_QSTR(MP_QSTR_MALLOC_CAP_32BIT), MP_ROM_INT(MALLOC_CAP_32BIT) },
    { MP_ROM_QSTR(MP_QSTR_MALLOC_CAP_DMA), MP_ROM_INT(MALLOC_CAP_DMA) },
    { MP_ROM_QSTR(MP_QSTR_MALLOC_CAP_EXEC), MP_ROM_INT(MALLOC_CAP_EXEC) },
    { MP_ROM_QSTR(MP_QSTR_MALLOC_CAP_INTERNAL), MP_ROM_INT(MALLOC_CAP_INTERNAL) },
    { MP_ROM_QSTR(MP_QSTR_MALLOC_CAP_SPIRAM), MP_ROM_INT(MALLOC_CAP_SPIRAM) },
    { MP_ROM_QSTR(MP_QSTR_MALLOC_CAP_DEFAULT), MP_ROM_INT(MALLOC_CAP_DEFAULT) },
    { MP_ROM_QSTR(MP_QSTR_reset_reason), MP_ROM_PTR(&espidf_reset_reason_obj) },
    { MP_ROM_QSTR(MP_QSTR_chip_info), MP_ROM_PTR(&espidf_chip_info_obj) },
    { MP_ROM_QSTR(MP_QSTR_app_description), MP_ROM_PTR(&espidf_app_description_obj) },
    { MP_ROM_QSTR(MP_QSTR_efuse_user_data), MP_ROM_PTR(&espidf_efuse_user_data_obj) },

    { MP_ROM_QSTR(MP_QSTR_Timer), MP_ROM_PTR(&espidf_timer_type) },
    { MP_ROM_QSTR(MP_QSTR_NVS), MP_ROM_PTR(&espidf_nvs_type) },
    { MP_ROM_QSTR(MP_QSTR_Partition), MP_ROM_PTR(&espidf_partition_type) },
    { MP_ROM_QSTR(MP_QSTR_partitions), MP_ROM_PTR(&espidf_partitions_obj) },
    { MP_ROM_QSTR(MP_QSTR_running_partition), MP_ROM_PTR(&espidf_running_partition_obj) },
    #if CIRCUITPY_PARTITION_DISK
    { MP_ROM_QSTR(MP_QSTR_expose_partition), MP_ROM_PTR(&espidf_expose_partition_obj) },
    #endif
    { MP_ROM_QSTR(MP_QSTR_pbkdf2), MP_ROM_PTR(&espidf_pbkdf2_obj) },
    { MP_ROM_QSTR(MP_QSTR_hmac_sha256), MP_ROM_PTR(&espidf_hmac_sha256_obj) },
    { MP_ROM_QSTR(MP_QSTR_aes_gcm_encrypt), MP_ROM_PTR(&espidf_aes_gcm_encrypt_obj) },
    { MP_ROM_QSTR(MP_QSTR_aes_gcm_decrypt), MP_ROM_PTR(&espidf_aes_gcm_decrypt_obj) },

    { MP_ROM_QSTR(MP_QSTR_EventQueue), MP_ROM_PTR(&espidf_event_queue_type) },
    { MP_ROM_QSTR(MP_QSTR_ANY_ID), MP_ROM_INT(ESP_EVENT_ANY_ID) },
    #if CIRCUITPY_WIFI
    { MP_ROM_QSTR(MP_QSTR_WIFI_EVENT), MP_ROM_QSTR(MP_QSTR_WIFI_EVENT) },
    { MP_ROM_QSTR(MP_QSTR_IP_EVENT), MP_ROM_QSTR(MP_QSTR_IP_EVENT) },
    { MP_ROM_QSTR(MP_QSTR_SC_EVENT), MP_ROM_QSTR(MP_QSTR_SC_EVENT) },
    #endif

    #if CIRCUITPY_BLEIO_NATIVE
    { MP_ROM_QSTR(MP_QSTR_ble_advertising_phy), MP_ROM_PTR(&espidf_ble_advertising_phy_obj) },
    { MP_ROM_QSTR(MP_QSTR_ble_prefer_phy), MP_ROM_PTR(&espidf_ble_prefer_phy_obj) },
    { MP_ROM_QSTR(MP_QSTR_ble_connect_phy), MP_ROM_PTR(&espidf_ble_connect_phy_obj) },
    { MP_ROM_QSTR(MP_QSTR_ble_connection_info), MP_ROM_PTR(&espidf_ble_connection_info_obj) },
    { MP_ROM_QSTR(MP_QSTR_ble_connection_phy), MP_ROM_PTR(&espidf_ble_connection_phy_obj) },
    #endif

    #if CIRCUITPY_WIFI
    { MP_ROM_QSTR(MP_QSTR_set_vendor_ie), MP_ROM_PTR(&espidf_set_vendor_ie_obj) },
    { MP_ROM_QSTR(MP_QSTR_eap_enable), MP_ROM_PTR(&espidf_eap_enable_obj) },
    { MP_ROM_QSTR(MP_QSTR_eap_disable), MP_ROM_PTR(&espidf_eap_disable_obj) },
    { MP_ROM_QSTR(MP_QSTR_TTLS_PHASE2_EAP), MP_ROM_INT(ESP_EAP_TTLS_PHASE2_EAP) },
    { MP_ROM_QSTR(MP_QSTR_TTLS_PHASE2_MSCHAPV2), MP_ROM_INT(ESP_EAP_TTLS_PHASE2_MSCHAPV2) },
    { MP_ROM_QSTR(MP_QSTR_TTLS_PHASE2_MSCHAP), MP_ROM_INT(ESP_EAP_TTLS_PHASE2_MSCHAP) },
    { MP_ROM_QSTR(MP_QSTR_TTLS_PHASE2_PAP), MP_ROM_INT(ESP_EAP_TTLS_PHASE2_PAP) },
    { MP_ROM_QSTR(MP_QSTR_TTLS_PHASE2_CHAP), MP_ROM_INT(ESP_EAP_TTLS_PHASE2_CHAP) },
    { MP_ROM_QSTR(MP_QSTR_smartconfig_start), MP_ROM_PTR(&espidf_smartconfig_start_obj) },
    { MP_ROM_QSTR(MP_QSTR_smartconfig_result), MP_ROM_PTR(&espidf_smartconfig_result_obj) },
    { MP_ROM_QSTR(MP_QSTR_smartconfig_stop), MP_ROM_PTR(&espidf_smartconfig_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_SC_TYPE_ESPTOUCH), MP_ROM_INT(SC_TYPE_ESPTOUCH) },
    { MP_ROM_QSTR(MP_QSTR_SC_TYPE_AIRKISS), MP_ROM_INT(SC_TYPE_AIRKISS) },
    { MP_ROM_QSTR(MP_QSTR_SC_TYPE_ESPTOUCH_AIRKISS), MP_ROM_INT(SC_TYPE_ESPTOUCH_AIRKISS) },
    { MP_ROM_QSTR(MP_QSTR_SC_TYPE_ESPTOUCH_V2), MP_ROM_INT(SC_TYPE_ESPTOUCH_V2) },
    { MP_ROM_QSTR(MP_QSTR_wifi_raw_tx), MP_ROM_PTR(&espidf_wifi_raw_tx_obj) },
    { MP_ROM_QSTR(MP_QSTR_wifi_sleep_min_active_time), MP_ROM_PTR(&espidf_wifi_sleep_min_active_time_obj) },
    #endif

    #if CIRCUITPY_ESPIDF_CSI
    { MP_ROM_QSTR(MP_QSTR_CSI), MP_ROM_PTR(&espidf_csi_type) },
    #endif

    #if CIRCUITPY_ESPIDF_SDM
    { MP_ROM_QSTR(MP_QSTR_SigmaDelta), MP_ROM_PTR(&espidf_sdm_type) },
    #endif

    #if CIRCUITPY_ESPIDF_LEDC_FADE
    { MP_ROM_QSTR(MP_QSTR_pwm_fade), MP_ROM_PTR(&espidf_pwm_fade_obj) },
    { MP_ROM_QSTR(MP_QSTR_pwm_fading), MP_ROM_PTR(&espidf_pwm_fading_obj) },
    #endif

    #if CIRCUITPY_ESPIDF_I2S_TDM
    { MP_ROM_QSTR(MP_QSTR_TDM), MP_ROM_PTR(&espidf_tdm_type) },
    #endif

    #if CIRCUITPY_ESPIDF_PCNT
    { MP_ROM_QSTR(MP_QSTR_PulseCounter), MP_ROM_PTR(&espidf_pulse_counter_type) },
    #endif

    #if CIRCUITPY_ESPIDF_RMT
    { MP_ROM_QSTR(MP_QSTR_RMTOut), MP_ROM_PTR(&espidf_rmt_out_type) },
    { MP_ROM_QSTR(MP_QSTR_RMTIn), MP_ROM_PTR(&espidf_rmt_in_type) },
    #endif

    #if CIRCUITPY_ESPIDF_MCPWM
    { MP_ROM_QSTR(MP_QSTR_MotorPWM), MP_ROM_PTR(&espidf_mcpwm_type) },
    { MP_ROM_QSTR(MP_QSTR_PulseCapture), MP_ROM_PTR(&espidf_pulse_capture_type) },
    #endif

    #if CIRCUITPY_ESPIDF_ANA_CMPR
    { MP_ROM_QSTR(MP_QSTR_Comparator), MP_ROM_PTR(&espidf_ana_cmpr_type) },
    #endif

    #if CIRCUITPY_ESPIDF_ETM
    { MP_ROM_QSTR(MP_QSTR_PinLink), MP_ROM_PTR(&espidf_pinlink_type) },
    #endif

    #if CIRCUITPY_ESPIDF_BITSCRAMBLER
    { MP_ROM_QSTR(MP_QSTR_bitscramble), MP_ROM_PTR(&espidf_bitscramble_obj) },
    #endif

    #if CIRCUITPY_ESPIDF_BLE_PERIODIC
    { MP_ROM_QSTR(MP_QSTR_PeriodicAdvertiser), MP_ROM_PTR(&espidf_periodic_adv_type) },
    { MP_ROM_QSTR(MP_QSTR_PeriodicSync), MP_ROM_PTR(&espidf_periodic_sync_type) },
    #endif

    #if CIRCUITPY_ESPIDF_BLE_MESH
    { MP_ROM_QSTR(MP_QSTR_BLEMesh), MP_ROM_PTR(&espidf_ble_mesh_type) },
    #endif

    #if CIRCUITPY_ESPIDF_BLE_HID
    { MP_ROM_QSTR(MP_QSTR_BLEHIDDevice), MP_ROM_PTR(&espidf_ble_hid_type) },
    #endif

    #if CIRCUITPY_ESPIDF_NAN
    { MP_ROM_QSTR(MP_QSTR_WiFiAware), MP_ROM_PTR(&espidf_wifi_aware_type) },
    #endif

    #if CIRCUITPY_ESPIDF_LPCORE
    { MP_ROM_QSTR(MP_QSTR_LPCore), MP_ROM_PTR(&espidf_lpcore_type) },
    { MP_ROM_QSTR(MP_QSTR_LPCoreAlarm), MP_ROM_PTR(&espidf_lpcorealarm_type) },
    #endif

    #if CIRCUITPY_ESPIDF_PARLIO
    { MP_ROM_QSTR(MP_QSTR_ParallelOut), MP_ROM_PTR(&espidf_parallel_out_type) },
    { MP_ROM_QSTR(MP_QSTR_ParallelIn), MP_ROM_PTR(&espidf_parallel_in_type) },
    #endif

    #if CIRCUITPY_ESPIDF_FTM
    { MP_ROM_QSTR(MP_QSTR_ftm), MP_ROM_PTR(&espidf_ftm_obj) },
    { MP_ROM_QSTR(MP_QSTR_ftm_responder), MP_ROM_PTR(&espidf_ftm_responder_obj) },
    { MP_ROM_QSTR(MP_QSTR_ftm_scan), MP_ROM_PTR(&espidf_ftm_scan_obj) },
    #endif
};
static MP_DEFINE_CONST_MAP(espidf_extended_globals, espidf_extended_globals_table);

// Called for every name espidf's own table lacks. Only loads are answered:
// these are constants, and a store or delete falls through to the usual
// AttributeError.
void espidf_extended_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    (void)self_in;
    if (dest[0] != MP_OBJ_NULL) {
        return;
    }
    mp_map_elem_t *elem = mp_map_lookup((mp_map_t *)&espidf_extended_globals, MP_OBJ_NEW_QSTR(attr), MP_MAP_LOOKUP);
    if (elem != NULL) {
        dest[0] = elem->value;
    }
}

MP_REGISTER_MODULE_DELEGATION(espidf_module, espidf_extended_attr);

void espidf_get_bytes(mp_obj_t obj, qstr arg, mp_buffer_info_t *bufinfo) {
    if (mp_obj_is_str(obj)) {
        size_t len;
        const char *s = mp_obj_str_get_data(obj, &len);
        bufinfo->buf = (void *)s;
        bufinfo->len = len;
        bufinfo->typecode = 'B';
        return;
    }
    if (!mp_get_buffer(obj, bufinfo, MP_BUFFER_READ)) {
        mp_raise_TypeError_varg(MP_ERROR_TEXT("%q must be a string or buffer"), arg);
    }
}

void espidf_extended_reset(void) {
    espidf_timer_reset();
    espidf_partition_reset();
    espidf_event_reset();
    #if CIRCUITPY_WIFI
    espidf_eap_reset();
    espidf_smartconfig_reset();
    #endif
    #if CIRCUITPY_ESPIDF_CSI
    espidf_csi_reset();
    #endif
    #if CIRCUITPY_ESPIDF_FTM
    espidf_ftm_reset();
    #endif
    #if CIRCUITPY_ESPIDF_SDM
    espidf_sdm_reset();
    #endif
    #if CIRCUITPY_ESPIDF_LEDC_FADE
    espidf_ledc_fade_reset();
    #endif
    #if CIRCUITPY_ESPIDF_I2S_TDM
    espidf_i2s_tdm_reset();
    #endif
    #if CIRCUITPY_ESPIDF_PCNT
    espidf_pcnt_reset();
    #endif
    #if CIRCUITPY_ESPIDF_RMT
    espidf_rmt_reset();
    #endif
    #if CIRCUITPY_ESPIDF_MCPWM
    espidf_mcpwm_cap_reset();
    espidf_mcpwm_reset();
    #endif
    #if CIRCUITPY_ESPIDF_ANA_CMPR
    espidf_ana_cmpr_reset();
    #endif
    #if CIRCUITPY_ESPIDF_ETM
    espidf_etm_reset();
    #endif
    #if CIRCUITPY_ESPIDF_PARLIO
    espidf_parlio_reset();
    #endif
    #if CIRCUITPY_ESPIDF_LPCORE
    espidf_lpcore_reset();
    #endif
    #if CIRCUITPY_ESPIDF_BLE_PERIODIC
    espidf_ble_periodic_reset();
    #endif
    #if CIRCUITPY_ESPIDF_NAN
    espidf_nan_reset();
    #endif
    #if CIRCUITPY_ESPIDF_BLE_MESH
    espidf_ble_mesh_reset();
    #endif
    #if CIRCUITPY_ESPIDF_BLE_HID
    espidf_ble_hid_reset();
    #endif
    #if CIRCUITPY_BLEIO_NATIVE
    // Otherwise the BLE workflow's own advertising would carry on in whatever
    // mode the program left.
    bleio_extended_adv_reset();
    #endif
}
