// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Petr Vavrin
//
// SPDX-License-Identifier: MIT

// Wi-Fi settings and tools that have no place in the shared wifi module:
// a vendor IE in the beacon, WPA2/WPA3 Enterprise, SmartConfig, a raw 802.11
// transmit, and the radio's awake window. See extended.h.

#include <string.h>

#include "py/mpstate.h"
#include "py/objlist.h"
#include "py/obj.h"
#include "py/runtime.h"

#include "bindings/espidf/__init__.h"
#include "bindings/espidf/extended.h"
#include "supervisor/port.h"

#include "esp_eap_client.h"
#include "esp_event.h"
#include "esp_smartconfig.h"
#include "esp_wifi.h"
// esp_wifi_set_sleep_min_active_time().
#include "esp_private/wifi.h"

#if CIRCUITPY_WIFI

// put a vendor-specific information element into the frames the
// radio sends as an AP, so nearby devices see custom data in the beacon without
// connecting. The complement of wifi.Monitor on the receiving side.
//| def set_vendor_ie(payload: ReadableBuffer | None, *, oui: ReadableBuffer = b"\xff\xff\xff") -> None:
//|     """Attach a vendor information element to the AP's beacons, or remove it with
//|     None. ``payload`` is up to 251 bytes. ``oui`` is the three-byte vendor id.
//|     WiFi has to be running as an AP (`wifi.radio.start_ap`)."""
//|     ...
//|
//|
static mp_obj_t espidf_set_vendor_ie(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_payload, ARG_oui };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_payload, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_oui, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = mp_const_none } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    if (args[ARG_payload].u_obj == mp_const_none) {
        // Disabling does not read the element, but the IDF wants a non-null
        // pointer with a valid header anyway.
        static const vendor_ie_data_t empty = { 0xDD, 4, {0xff, 0xff, 0xff}, 0 };
        CHECK_ESP_RESULT(esp_wifi_set_vendor_ie(false, WIFI_VND_IE_TYPE_BEACON,
            WIFI_VND_IE_ID_0, &empty));
        return mp_const_none;
    }

    mp_buffer_info_t payload;
    mp_get_buffer_raise(args[ARG_payload].u_obj, &payload, MP_BUFFER_READ);
    if (payload.len > 251) {
        mp_raise_ValueError(MP_ERROR_TEXT("payload is at most 251 bytes"));
    }

    uint8_t oui[3] = { 0xff, 0xff, 0xff };
    if (args[ARG_oui].u_obj != mp_const_none) {
        mp_buffer_info_t oui_buf;
        mp_get_buffer_raise(args[ARG_oui].u_obj, &oui_buf, MP_BUFFER_READ);
        if (oui_buf.len != 3) {
            mp_raise_ValueError(MP_ERROR_TEXT("oui must be 3 bytes"));
        }
        memcpy(oui, oui_buf.buf, 3);
    }

    // Header is 6 bytes: element_id, length, oui[3], oui_type. length counts
    // everything after itself, so oui (3) + type (1) + payload.
    size_t total = sizeof(vendor_ie_data_t) + payload.len;
    vendor_ie_data_t *ie = m_malloc(total);
    ie->element_id = 0xDD;
    ie->length = 4 + payload.len;
    memcpy(ie->vendor_oui, oui, 3);
    ie->vendor_oui_type = 0;
    memcpy(ie->payload, payload.buf, payload.len);

    // Setting the same element twice returns INVALID_ARG, and the element
    // survives a soft reset, so clear it first. The result is ignored: there is
    // nothing to clear on the very first call.
    esp_wifi_set_vendor_ie(false, WIFI_VND_IE_TYPE_BEACON, WIFI_VND_IE_ID_0, NULL);
    esp_err_t err = esp_wifi_set_vendor_ie(true, WIFI_VND_IE_TYPE_BEACON,
        WIFI_VND_IE_ID_0, ie);
    m_free(ie);
    CHECK_ESP_RESULT(err);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_KW(espidf_set_vendor_ie_obj, 1, espidf_set_vendor_ie);

// WPA2/WPA3 Enterprise. wifi.AuthMode.ENTERPRISE already came
// back from a scan, but wifi.radio.connect() only takes a PSK, so an enterprise
// network could be seen and not joined. The credentials go in here instead of
// through connect(), because that binding is shared by every port and this is
// specific to the ESP-IDF supplicant. esp_wifi_sta_enterprise_enable() is a
// standing mode, so the existing connect() then works unchanged.

// esp_eap_client copies the identity, username and password, but for the
// certificates it keeps only the pointer (esp_eap_client.c: g_wpa_ca_cert =
// ca_cert). Those buffers therefore have to outlive the call, so the objects are
// held here until the supplicant has been told to forget them.
MP_REGISTER_ROOT_POINTER(mp_obj_t espidf_eap_certs);

static bool espidf_eap_enabled = false;

// Returns false when the argument was omitted.
static bool espidf_eap_buffer(mp_obj_t obj, qstr arg, mp_buffer_info_t *bufinfo, bool retain) {
    if (obj == mp_const_none) {
        return false;
    }
    espidf_get_bytes(obj, arg, bufinfo);
    // The IDF rejects a zero length itself, but as ESP_ERR_INVALID_ARG, which
    // surfaces as "Invalid argument" with no clue which argument.
    if (bufinfo->len == 0) {
        mp_raise_ValueError_varg(MP_ERROR_TEXT("%q must not be empty"), arg);
    }
    if (retain) {
        if (MP_STATE_VM(espidf_eap_certs) == MP_OBJ_NULL) {
            MP_STATE_VM(espidf_eap_certs) = mp_obj_new_list(0, NULL);
        }
        mp_obj_list_append(MP_STATE_VM(espidf_eap_certs), obj);
    }
    return true;
}

static void espidf_eap_forget(void) {
    esp_eap_client_clear_ca_cert();
    esp_eap_client_clear_certificate_and_key();
    esp_eap_client_clear_identity();
    esp_eap_client_clear_username();
    esp_eap_client_clear_password();
    MP_STATE_VM(espidf_eap_certs) = MP_OBJ_NULL;
}

//| def eap_enable(
//|     *,
//|     identity: Optional[str] = None,
//|     username: Optional[str] = None,
//|     password: Optional[str] = None,
//|     ca_cert: Optional[ReadableBuffer] = None,
//|     client_cert: Optional[ReadableBuffer] = None,
//|     client_key: Optional[ReadableBuffer] = None,
//|     client_key_password: Optional[str] = None,
//|     ttls_phase2: Optional[int] = None,
//| ) -> None:
//|     """Supply WPA2/WPA3 Enterprise credentials and put the station into
//|     enterprise mode. Afterwards join with ``wifi.radio.connect(ssid)`` and no
//|     password.
//|
//|     PEAP and TTLS need ``username`` and ``password``, and usually ``identity``
//|     as the anonymous outer identity. TLS needs ``client_cert`` and
//|     ``client_key``. ``ca_cert`` is optional but without it the server is not
//|     authenticated. ``ttls_phase2`` is one of the ``espidf.TTLS_PHASE2_*``
//|     values, MSCHAPV2 being the common one.
//|
//|     Certificates are kept by reference, so they stay alive until
//|     `eap_disable`."""
//|     ...
static mp_obj_t espidf_eap_enable(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_identity, ARG_username, ARG_password, ARG_ca_cert, ARG_client_cert,
           ARG_client_key, ARG_client_key_password, ARG_ttls_phase2 };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_identity, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = mp_const_none } },
        { MP_QSTR_username, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = mp_const_none } },
        { MP_QSTR_password, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = mp_const_none } },
        { MP_QSTR_ca_cert, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = mp_const_none } },
        { MP_QSTR_client_cert, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = mp_const_none } },
        { MP_QSTR_client_key, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = mp_const_none } },
        { MP_QSTR_client_key_password, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = mp_const_none } },
        { MP_QSTR_ttls_phase2, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = mp_const_none } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    // A client certificate is useless without its key and the reverse is worse:
    // esp_eap_client_set_certificate_and_key takes both together.
    bool have_cert = args[ARG_client_cert].u_obj != mp_const_none;
    bool have_key = args[ARG_client_key].u_obj != mp_const_none;
    if (have_cert != have_key) {
        mp_raise_ValueError(MP_ERROR_TEXT("client_cert and client_key go together"));
    }

    mp_buffer_info_t buf, buf2, buf3;
    if (espidf_eap_buffer(args[ARG_identity].u_obj, MP_QSTR_identity, &buf, false)) {
        CHECK_ESP_RESULT(esp_eap_client_set_identity(buf.buf, buf.len));
    }
    if (espidf_eap_buffer(args[ARG_username].u_obj, MP_QSTR_username, &buf, false)) {
        CHECK_ESP_RESULT(esp_eap_client_set_username(buf.buf, buf.len));
    }
    if (espidf_eap_buffer(args[ARG_password].u_obj, MP_QSTR_password, &buf, false)) {
        CHECK_ESP_RESULT(esp_eap_client_set_password(buf.buf, buf.len));
    }
    if (espidf_eap_buffer(args[ARG_ca_cert].u_obj, MP_QSTR_ca_cert, &buf, true)) {
        CHECK_ESP_RESULT(esp_eap_client_set_ca_cert(buf.buf, buf.len));
    }
    if (have_cert) {
        espidf_eap_buffer(args[ARG_client_cert].u_obj, MP_QSTR_client_cert, &buf, true);
        espidf_eap_buffer(args[ARG_client_key].u_obj, MP_QSTR_client_key, &buf2, true);
        bool have_pw = espidf_eap_buffer(args[ARG_client_key_password].u_obj,
            MP_QSTR_client_key_password, &buf3, true);
        CHECK_ESP_RESULT(esp_eap_client_set_certificate_and_key(buf.buf, buf.len,
            buf2.buf, buf2.len, have_pw ? buf3.buf : NULL, have_pw ? buf3.len : 0));
    }
    if (args[ARG_ttls_phase2].u_obj != mp_const_none) {
        mp_int_t phase2 = mp_arg_validate_int_range(mp_obj_get_int(args[ARG_ttls_phase2].u_obj),
            ESP_EAP_TTLS_PHASE2_EAP, ESP_EAP_TTLS_PHASE2_CHAP, MP_QSTR_ttls_phase2);
        CHECK_ESP_RESULT(esp_eap_client_set_ttls_phase2_method(phase2));
    }

    esp_err_t err = esp_wifi_sta_enterprise_enable();
    if (err != ESP_OK) {
        espidf_eap_forget();
        CHECK_ESP_RESULT(err);
    }
    espidf_eap_enabled = true;
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_KW(espidf_eap_enable_obj, 0, espidf_eap_enable);

//| def eap_disable() -> None:
//|     """Leave enterprise mode and forget the credentials. Safe to call when it
//|     was never enabled."""
//|     ...
static mp_obj_t espidf_eap_disable(void) {
    if (espidf_eap_enabled) {
        esp_wifi_sta_enterprise_disable();
        espidf_eap_enabled = false;
    }
    // Only after the supplicant has been told to stop, since it holds the
    // certificate pointers rather than copies.
    espidf_eap_forget();
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(espidf_eap_disable_obj, espidf_eap_disable);

void espidf_eap_reset(void) {
    if (espidf_eap_enabled) {
        esp_wifi_sta_enterprise_disable();
        espidf_eap_enabled = false;
    }
    esp_eap_client_clear_ca_cert();
    esp_eap_client_clear_certificate_and_key();
    esp_eap_client_clear_identity();
    esp_eap_client_clear_username();
    esp_eap_client_clear_password();
    MP_STATE_VM(espidf_eap_certs) = MP_OBJ_NULL;
}

// SmartConfig, so a phone can hand the board an SSID and
// password over the air instead of settings.toml being edited.
//
// The result is 114 bytes, more than EventQueue carries, so it is kept whole in a
// static here and read back through smartconfig_result(). The other SC_EVENT ids
// have no payload and can be watched with an EventQueue if the progress matters.

static bool espidf_sc_running = false;
static esp_event_handler_instance_t espidf_sc_instance;
static volatile bool espidf_sc_got;
static smartconfig_event_got_ssid_pswd_t espidf_sc_result;

// Runs in the event loop task.
static void espidf_sc_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == SC_EVENT && id == SC_EVENT_GOT_SSID_PSWD && data != NULL) {
        memcpy(&espidf_sc_result, data, sizeof(espidf_sc_result));
        espidf_sc_got = true;
        port_wake_main_task();
    }
}

//| def smartconfig_start(sc_type: int = SC_TYPE_ESPTOUCH) -> None:
//|     """Start listening for credentials from a phone running EspTouch or AirKiss.
//|
//|     The station has to be running first (``wifi.radio.start_station()``). Poll
//|     `smartconfig_result` until it returns something, connect with it, and only
//|     then call `smartconfig_stop` — the acknowledgement to the phone is sent once
//|     the board has an address, so stopping earlier makes the phone report a
//|     failure even though the board joined."""
//|     ...
static mp_obj_t espidf_smartconfig_start(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_sc_type };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_sc_type, MP_ARG_INT, { .u_int = SC_TYPE_ESPTOUCH } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    if (espidf_sc_running) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("SmartConfig is already running"));
    }
    mp_int_t sc_type = mp_arg_validate_int_range(args[ARG_sc_type].u_int,
        SC_TYPE_ESPTOUCH, SC_TYPE_ESPTOUCH_V2, MP_QSTR_sc_type);

    esp_err_t loop_err = esp_event_loop_create_default();
    if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) {
        CHECK_ESP_RESULT(loop_err);
    }

    espidf_sc_got = false;
    memset(&espidf_sc_result, 0, sizeof(espidf_sc_result));

    CHECK_ESP_RESULT(esp_smartconfig_set_type(sc_type));
    CHECK_ESP_RESULT(esp_event_handler_instance_register(SC_EVENT, ESP_EVENT_ANY_ID,
        &espidf_sc_handler, NULL, &espidf_sc_instance));
    smartconfig_start_config_t cfg = SMARTCONFIG_START_CONFIG_DEFAULT();
    esp_err_t err = esp_smartconfig_start(&cfg);
    if (err != ESP_OK) {
        esp_event_handler_instance_unregister(SC_EVENT, ESP_EVENT_ANY_ID, espidf_sc_instance);
        CHECK_ESP_RESULT(err);
    }
    espidf_sc_running = true;
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_KW(espidf_smartconfig_start_obj, 0, espidf_smartconfig_start);

//| def smartconfig_result() -> Optional[Tuple[str, str, Optional[bytes]]]:
//|     """``(ssid, password, bssid)`` once a phone has sent them, otherwise None.
//|     ``bssid`` is None unless the phone named a particular access point."""
//|     ...
static mp_obj_t espidf_smartconfig_result(void) {
    if (!espidf_sc_got) {
        return mp_const_none;
    }
    // Both fields are documented as null terminated, but a full length one fills
    // the array with no room for the terminator, so the length is bounded.
    size_t ssid_len = strnlen((const char *)espidf_sc_result.ssid, sizeof(espidf_sc_result.ssid));
    size_t pw_len = strnlen((const char *)espidf_sc_result.password, sizeof(espidf_sc_result.password));
    mp_obj_t items[3] = {
        mp_obj_new_str((const char *)espidf_sc_result.ssid, ssid_len),
        mp_obj_new_str((const char *)espidf_sc_result.password, pw_len),
        espidf_sc_result.bssid_set
            ? mp_obj_new_bytes(espidf_sc_result.bssid, sizeof(espidf_sc_result.bssid))
            : mp_const_none,
    };
    return mp_obj_new_tuple(3, items);
}
MP_DEFINE_CONST_FUN_OBJ_0(espidf_smartconfig_result_obj, espidf_smartconfig_result);

//| def smartconfig_stop() -> None:
//|     """Stop listening and free what SmartConfig allocated. Safe to call when it
//|     was never started."""
//|     ...
static mp_obj_t espidf_smartconfig_stop(void) {
    if (espidf_sc_running) {
        esp_smartconfig_stop();
        esp_event_handler_instance_unregister(SC_EVENT, ESP_EVENT_ANY_ID, espidf_sc_instance);
        espidf_sc_running = false;
    }
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(espidf_smartconfig_stop_obj, espidf_smartconfig_stop);

void espidf_smartconfig_reset(void) {
    if (espidf_sc_running) {
        esp_smartconfig_stop();
        esp_event_handler_instance_unregister(SC_EVENT, ESP_EVENT_ANY_ID, espidf_sc_instance);
        espidf_sc_running = false;
    }
    espidf_sc_got = false;
}

// transmit a raw 802.11 frame.
//
// This is a test and development tool for your own network and hardware. The
// radio will send whatever bytes you hand it, so it is also what is used to
// spoof frames and knock other devices off a network, which is why CircuitPython
// does not expose it. Use it on gear you own and are allowed to test.
//| def wifi_raw_tx(frame: ReadableBuffer, *, channel: int | None = None) -> None:
//|     """Send a raw 802.11 frame. The FCS is appended by the hardware, so do not
//|     include it. WiFi must be started (`wifi.radio.enabled = True`); set
//|     ``channel`` to move there first.
//|
//|     For testing on hardware you own and are authorised to test."""
//|     ...
//|
//|
//| def wifi_sleep_min_active_time(milliseconds: int) -> None:
//|     """How long the radio stays awake after a packet before it is allowed to
//|     sleep again, with `wifi.PowerManagement.MIN` or ``MAX`` in effect. Every
//|     further packet restarts the window, so a peer that talks more often than
//|     this keeps the radio up and never waits for the next DTIM beacon.
//|
//|     The build sets this once from CONFIG_ESP_WIFI_SLP_DEFAULT_MIN_ACTIVE_TIME,
//|     which menuconfig caps at 60 ms. Raising it at run time trades current for
//|     latency without going all the way to `wifi.PowerManagement.NONE`: a server
//|     answering a poll every 200 ms can hold the radio up across the gap while it
//|     is busy and drop back to a few tens of milliseconds when it is not.
//|
//|     Applies to the whole radio, not one socket."""
//|     ...
//|
//|
static mp_obj_t espidf_wifi_sleep_min_active_time(mp_obj_t ms_in) {
    // The driver takes microseconds. An hour is far past any useful setting and
    // keeps the multiplication below inside 32 bits.
    mp_int_t ms = mp_arg_validate_int_range(mp_obj_get_int(ms_in), 1, 3600000,
        MP_QSTR_milliseconds);
    esp_wifi_set_sleep_min_active_time((uint32_t)ms * 1000);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(espidf_wifi_sleep_min_active_time_obj, espidf_wifi_sleep_min_active_time);

static mp_obj_t espidf_wifi_raw_tx(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_frame, ARG_channel };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_frame, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_channel, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = mp_const_none } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(args[ARG_frame].u_obj, &bufinfo, MP_BUFFER_READ);
    if (bufinfo.len < 24 || bufinfo.len > 1500) {
        mp_raise_ValueError(MP_ERROR_TEXT("frame must be 24 to 1500 bytes"));
    }

    if (args[ARG_channel].u_obj != mp_const_none) {
        mp_int_t channel = mp_obj_get_int(args[ARG_channel].u_obj);
        if (channel < 1 || channel > 14) {
            mp_raise_ValueError(MP_ERROR_TEXT("channel must be 1 to 14"));
        }
        CHECK_ESP_RESULT(esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE));
    }

    // en_sys_seq false: leave the sequence number as given in the frame.
    CHECK_ESP_RESULT(esp_wifi_80211_tx(WIFI_IF_STA, bufinfo.buf, bufinfo.len, false));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_KW(espidf_wifi_raw_tx_obj, 1, espidf_wifi_raw_tx);

#endif // CIRCUITPY_WIFI
