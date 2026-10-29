// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Petr Vavrin
//
// SPDX-License-Identifier: MIT

// espidf.ftm() and espidf.ftm_responder(); see extended.h.

#include <string.h>

#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/obj.h"
#include "py/objstr.h"
#include "py/runtime.h"

#include "bindings/espidf/__init__.h"
#include "bindings/espidf/extended.h"
#include "shared-bindings/wifi/__init__.h"
#include "shared-bindings/wifi/Radio.h"
#include "supervisor/shared/tick.h"

#include "esp_event.h"
#include "esp_wifi.h"

// Wi-Fi Fine Timing Measurement (802.11mc): the initiator and a responder
// exchange timestamped frames, and the round trip time, less the responder's
// turnaround, is twice the distance over the speed of light. Both ends have
// to take part; this measures nothing to an ordinary transmitter.

#if CIRCUITPY_ESPIDF_FTM

// The report arrives as an event in the event loop's task. It is copied here
// and picked up by the waiting Python call; one session runs at a time.
static wifi_event_ftm_report_t _report;
static volatile bool _report_ready;
static bool _handler_registered;

static void ftm_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)base;
    (void)id;
    memcpy(&_report, data, sizeof(_report));
    _report_ready = true;
}

static const char *status_name(wifi_ftm_status_t status) {
    switch (status) {
        case FTM_STATUS_SUCCESS:
            return "success";
        case FTM_STATUS_UNSUPPORTED:
            return "unsupported";
        case FTM_STATUS_CONF_REJECTED:
            return "rejected";
        case FTM_STATUS_NO_RESPONSE:
            return "no response";
        case FTM_STATUS_NO_VALID_MSMT:
            return "no valid measurement";
        case FTM_STATUS_USER_TERM:
            return "terminated";
        default:
            return "failed";
    }
}

//| def ftm(
//|     bssid: ReadableBuffer,
//|     channel: int,
//|     *,
//|     frames: int = 32,
//|     burst_period: int = 2,
//|     timeout: float = 5.0,
//| ) -> dict:
//|     """Measure the distance to an FTM responder (802.11mc), as the initiator.
//|
//|     ``bssid`` and ``channel`` are the responder's, e.g. from
//|     `wifi.Radio.start_scanning_networks`. The responder must have FTM switched
//|     on; on another board running this firmware, `ftm_responder` does that.
//|     Wi-Fi must be enabled; a connection is not needed.
//|
//|     :param int frames: FTM frames to ask for: 16, 24, 32 or 64.
//|     :param int burst_period: between bursts, in 100 ms units.
//|     :param float timeout: seconds to wait for the report.
//|
//|     Returns a dict: ``status`` (``"success"`` or why not), ``rtt_ns`` (the
//|     driver's estimate of the round trip), ``rtt_raw_ns`` (the plain mean),
//|     ``distance_m`` (from ``rtt_ns``), and ``entries``, one ``(rssi, rtt_ps)``
//|     tuple per frame exchange. Raises OSError if the session cannot start."""
//|     ...
//|
//|
static mp_obj_t espidf_ftm(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_bssid, ARG_channel, ARG_frames, ARG_burst_period, ARG_timeout };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_bssid, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_channel, MP_ARG_REQUIRED | MP_ARG_INT },
        { MP_QSTR_frames, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 32 } },
        { MP_QSTR_burst_period, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 2 } },
        { MP_QSTR_timeout, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = MP_OBJ_NULL } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_buffer_info_t bssid;
    mp_get_buffer_raise(args[ARG_bssid].u_obj, &bssid, MP_BUFFER_READ);
    if (bssid.len != 6) {
        mp_raise_ValueError(MP_ERROR_TEXT("bssid must be 6 bytes"));
    }
    mp_int_t channel = mp_arg_validate_int_range(args[ARG_channel].u_int, 1, 196, MP_QSTR_channel);
    mp_int_t frames = args[ARG_frames].u_int;
    if (frames != 16 && frames != 24 && frames != 32 && frames != 64) {
        mp_raise_ValueError(MP_ERROR_TEXT("frames must be 16, 24, 32 or 64"));
    }
    mp_int_t burst_period = mp_arg_validate_int_range(args[ARG_burst_period].u_int, 0, 100, MP_QSTR_burst_period);
    mp_float_t timeout = args[ARG_timeout].u_obj == MP_OBJ_NULL ? 5.0f : mp_obj_get_float(args[ARG_timeout].u_obj);

    common_hal_wifi_init(true);
    if (!_handler_registered) {
        CHECK_ESP_RESULT(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_FTM_REPORT, ftm_event_handler, NULL));
        _handler_registered = true;
    }

    wifi_ftm_initiator_cfg_t cfg = {
        .channel = channel,
        .frm_count = frames,
        .burst_period = burst_period,
    };
    memcpy(cfg.resp_mac, bssid.buf, 6);
    _report_ready = false;
    CHECK_ESP_RESULT(esp_wifi_ftm_initiate_session(&cfg));

    uint64_t deadline = supervisor_ticks_ms64() + (uint64_t)(timeout * 1000);
    while (!_report_ready) {
        RUN_BACKGROUND_TASKS;
        if (mp_hal_is_interrupted() || supervisor_ticks_ms64() > deadline) {
            esp_wifi_ftm_end_session();
            if (mp_hal_is_interrupted()) {
                mp_handle_pending(true);
            }
            mp_raise_OSError(MP_ETIMEDOUT);
        }
    }

    mp_obj_t entries = mp_obj_new_list(0, NULL);
    uint8_t count = _report.ftm_report_num_entries;
    if (count) {
        wifi_ftm_report_entry_t *report = m_new(wifi_ftm_report_entry_t, count);
        if (esp_wifi_ftm_get_report(report, count) == ESP_OK) {
            for (uint8_t i = 0; i < count; i++) {
                mp_obj_t pair[2] = {
                    MP_OBJ_NEW_SMALL_INT(report[i].rssi),
                    mp_obj_new_int((int32_t)report[i].rtt),
                };
                mp_obj_list_append(entries, mp_obj_new_tuple(2, pair));
            }
        }
        m_del(wifi_ftm_report_entry_t, report, count);
    }

    mp_obj_t result = mp_obj_new_dict(5);
    mp_obj_dict_store(result, MP_ROM_QSTR(MP_QSTR_status),
        mp_obj_new_str_from_cstr(status_name(_report.status)));
    // The driver declares these unsigned, but once it has taken off the
    // responder's turnaround a pair of boards close together can come out
    // below zero, and read as unsigned that is 4 294 967 295 ps, 644 km.
    mp_obj_dict_store(result, MP_ROM_QSTR(MP_QSTR_rtt_ns), mp_obj_new_int((int32_t)_report.rtt_est));
    mp_obj_dict_store(result, MP_ROM_QSTR(MP_QSTR_rtt_raw_ns), mp_obj_new_int((int32_t)_report.rtt_raw));
    mp_obj_dict_store(result, MP_ROM_QSTR(MP_QSTR_distance_m), mp_obj_new_float((int32_t)_report.dist_est / 100.0f));
    mp_obj_dict_store(result, MP_ROM_QSTR(MP_QSTR_entries), entries);
    return result;
}
MP_DEFINE_CONST_FUN_OBJ_KW(espidf_ftm_obj, 2, espidf_ftm);

//| def ftm_responder(enabled: bool = True, *, offset_cm: int = 0) -> None:
//|     """Answer FTM requests (802.11mc) on this board's access point, so that
//|     another board can measure the distance to it with `ftm`. Wi-Fi has to be
//|     running as an AP (`wifi.Radio.start_ap`).
//|
//|     :param int offset_cm: taken off every distance measured to this board
//|       (the driver adds it to the frame's departure time). Set it to the
//|       measured minus the true distance at a known distance; that covers the
//|       delays of this particular pair of boards and antennas. A distance that
//|       comes out below zero is dropped by the measuring side as no valid
//|       measurement, so overcorrecting loses readings rather than showing
//|       negative ones."""
//|     ...
//|
//|
static mp_obj_t espidf_ftm_responder(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_enabled, ARG_offset_cm };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_enabled, MP_ARG_BOOL, { .u_bool = true } },
        { MP_QSTR_offset_cm, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 0 } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    mp_int_t offset = mp_arg_validate_int_range(args[ARG_offset_cm].u_int, -32768, 32767, MP_QSTR_offset_cm);

    wifi_config_t config;
    CHECK_ESP_RESULT(esp_wifi_get_config(WIFI_IF_AP, &config));
    config.ap.ftm_responder = args[ARG_enabled].u_bool;
    CHECK_ESP_RESULT(esp_wifi_set_config(WIFI_IF_AP, &config));
    if (args[ARG_enabled].u_bool) {
        CHECK_ESP_RESULT(esp_wifi_ftm_resp_set_offset(offset));
    }
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_KW(espidf_ftm_responder_obj, 0, espidf_ftm_responder);

//| def ftm_scan() -> list[dict]:
//|     """Scan all channels and list the access points heard, with what they
//|     announce about FTM in their beacons: dicts with ``ssid``, ``bssid``,
//|     ``channel``, ``rssi``, ``ftm_responder`` and ``ftm_initiator``. Only an AP
//|     with ``ftm_responder`` set answers `ftm`. `wifi.Network` does not carry
//|     these flags, hence a scan of its own. Wi-Fi must be enabled and not
//|     already scanning."""
//|     ...
//|
//|
static mp_obj_t espidf_ftm_scan(void) {
    // What "import wifi" does, which a script using only espidf has not done.
    common_hal_wifi_init(true);
    wifi_radio_obj_t *radio = &common_hal_wifi_radio_obj;
    if (!common_hal_wifi_radio_get_enabled(radio)) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("Wi-Fi is not enabled"));
    }
    if (radio->current_scan != NULL) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("Already scanning for wifi networks"));
    }
    wifi_scan_config_t config = { .show_hidden = true };
    // The radio's event handler sets this bit on WIFI_EVENT_SCAN_DONE; waiting
    // on it with clear-on-exit, as ScannedNetworks does, leaves it clear for
    // the next wifi.radio scan.
    xEventGroupClearBits(radio->event_group_handle, WIFI_SCAN_DONE_BIT);
    CHECK_ESP_RESULT(esp_wifi_scan_start(&config, false));
    EventBits_t bits = 0;
    while ((bits & WIFI_SCAN_DONE_BIT) == 0) {
        RUN_BACKGROUND_TASKS;
        if (mp_hal_is_interrupted()) {
            esp_wifi_scan_stop();
            mp_handle_pending(true);
        }
        bits = xEventGroupWaitBits(radio->event_group_handle, WIFI_SCAN_DONE_BIT, pdTRUE, pdTRUE, 0);
    }

    uint16_t count = 0;
    CHECK_ESP_RESULT(esp_wifi_scan_get_ap_num(&count));
    mp_obj_t result = mp_obj_new_list(0, NULL);
    if (count == 0) {
        esp_wifi_clear_ap_list();
        return result;
    }
    wifi_ap_record_t *records = m_new(wifi_ap_record_t, count);
    esp_err_t err = esp_wifi_scan_get_ap_records(&count, records);
    if (err != ESP_OK) {
        m_del(wifi_ap_record_t, records, count);
        raise_esp_error(err);
    }
    for (uint16_t i = 0; i < count; i++) {
        const wifi_ap_record_t *r = &records[i];
        mp_obj_t ap = mp_obj_new_dict(6);
        mp_obj_dict_store(ap, MP_ROM_QSTR(MP_QSTR_ssid),
            mp_obj_new_str((const char *)r->ssid, strnlen((const char *)r->ssid, sizeof(r->ssid))));
        mp_obj_dict_store(ap, MP_ROM_QSTR(MP_QSTR_bssid), mp_obj_new_bytes(r->bssid, 6));
        mp_obj_dict_store(ap, MP_ROM_QSTR(MP_QSTR_channel), MP_OBJ_NEW_SMALL_INT(r->primary));
        mp_obj_dict_store(ap, MP_ROM_QSTR(MP_QSTR_rssi), MP_OBJ_NEW_SMALL_INT(r->rssi));
        mp_obj_dict_store(ap, MP_ROM_QSTR(MP_QSTR_ftm_responder), mp_obj_new_bool(r->ftm_responder));
        mp_obj_dict_store(ap, MP_ROM_QSTR(MP_QSTR_ftm_initiator), mp_obj_new_bool(r->ftm_initiator));
        mp_obj_list_append(result, ap);
    }
    m_del(wifi_ap_record_t, records, count);
    return result;
}
MP_DEFINE_CONST_FUN_OBJ_0(espidf_ftm_scan_obj, espidf_ftm_scan);

void espidf_ftm_reset(void) {
    // A session left running would report into a handler that outlives the VM
    // harmlessly, but it would also keep the radio busy.
    if (_handler_registered) {
        esp_wifi_ftm_end_session();
    }
}

#endif
