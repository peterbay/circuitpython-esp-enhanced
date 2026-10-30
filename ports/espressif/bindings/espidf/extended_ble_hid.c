// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Petr Vavrin
//
// SPDX-License-Identifier: MIT

// espidf.BLEHIDDevice; see extended.h.

#include <string.h>

#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/obj.h"
#include "py/objproperty.h"
#include "py/runtime.h"

#include "bindings/espidf/__init__.h"
#include "bindings/espidf/extended.h"
#include "shared-bindings/_bleio/Adapter.h"
#include "shared-bindings/_bleio/Address.h"
#include "shared-bindings/_bleio/__init__.h"
#include "shared-bindings/util.h"
#include "shared/runtime/context_manager_helpers.h"
#include "supervisor/shared/tick.h"

#include "freertos/FreeRTOS.h"
// The headers below exist only where the switch brings them in.
#if CIRCUITPY_ESPIDF_BLE_HID

#include "esp_hid_common.h"
#include "esp_random.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "os/os_mbuf.h"

// A host for HID over GATT (HOGP): Bluetooth LE keyboards, mice, game
// controllers and remote controls. The IDF's own host, esp_hidh, is not used:
// in this IDF its NimBLE half never reaches the code that reads the report map
// and the reports (a misplaced brace in nimble_hidh.c), it waits on its
// semaphores without a timeout, and it answers every passkey with 123456. Only
// its report map parser is used.
//
// Everything a device is set up with is read here, step by step, from the
// Python task; the NimBLE callbacks only store what they get in static memory,
// so a step that is given up on can still complete without harm.

#define MAX_DEVICES (2)
#define MAX_CHRS (32)
#define MAX_REPORTS (16)
#define QUEUE_LEN (16)
#define REPORT_MAX (64)
// A Report Map is at most 512 bytes (HID over GATT, 2.6.1).
#define READ_MAX (512)
#define CLOSE_WAIT_MS (1000)
#define OUTPUT_TIMEOUT_MS (5000)

#define UUID_HID_SERVICE (0x1812)
#define UUID_BATTERY_SERVICE (0x180f)
#define UUID_REPORT_MAP (0x2a4b)
#define UUID_REPORT (0x2a4d)
#define UUID_BATTERY_LEVEL (0x2a19)
#define UUID_CCCD (0x2902)
#define UUID_REPORT_REFERENCE (0x2908)

typedef struct {
    uint8_t len;
    // Into the slot's reports. Its usage is looked up when the report is read:
    // a bonded device sends reports as soon as the link is encrypted, before
    // the report map has been read.
    uint8_t index;
    uint8_t data[REPORT_MAX];
} queued_report_t;

typedef struct {
    uint16_t handle;
    uint16_t ccc;
    uint8_t id;
    // ESP_HID_REPORT_TYPE_*: the Report Reference uses the same numbers.
    uint8_t type;
    uint8_t usage;
} hid_report_t;

typedef struct {
    volatile enum { SLOT_FREE, SLOT_CONNECTING, SLOT_CONNECTED, SLOT_CLOSED } state;
    volatile uint16_t conn;
    volatile int close_reason;
    volatile bool enc_done;
    volatile int enc_status;
    hid_report_t reports[MAX_REPORTS];
    // Raised only once an entry is filled in: the NimBLE task reads them.
    volatile size_t n_reports;
    uint16_t battery_handle;
    volatile int16_t battery;
    // Under _lock: filled by the NimBLE task, emptied by read().
    queued_report_t queue[QUEUE_LEN];
    size_t head;
    size_t count;
} hid_slot_t;

static hid_slot_t _slots[MAX_DEVICES];
static portMUX_TYPE _lock = portMUX_INITIALIZER_UNLOCKED;
// Set by the NimBLE task when this side has to show a passkey.
static volatile int32_t _passkey = -1;

// One GATT procedure at a time, from the Python task. A callback from a
// procedure that was given up on carries an older sequence number and is
// ignored.
static uint32_t _op_seq;
static volatile bool _op_done;
static volatile int _op_status;
static bool _svc_found;
static uint16_t _svc_start;
static uint16_t _svc_end;
static struct ble_gatt_chr _chrs[MAX_CHRS];
static size_t _n_chrs;
static bool _chrs_overflow;
static uint16_t _dsc_ccc;
static uint16_t _dsc_ref;
static uint8_t _buf[READ_MAX];
static size_t _buf_len;

typedef struct {
    mp_obj_base_t base;
    hid_slot_t *slot;
    mp_obj_t report_map;
    uint8_t usage;
} espidf_ble_hid_obj_t;

static void check_nimble(int rc) {
    if (rc != 0) {
        mp_raise_OSError_msg_varg(MP_ERROR_TEXT("NimBLE error %d"), rc);
    }
}

static qstr usage_qstr(uint8_t usage) {
    switch (usage) {
        case ESP_HID_USAGE_KEYBOARD:
            return MP_QSTR_keyboard;
        case ESP_HID_USAGE_MOUSE:
            return MP_QSTR_mouse;
        case ESP_HID_USAGE_JOYSTICK:
            return MP_QSTR_joystick;
        case ESP_HID_USAGE_GAMEPAD:
            return MP_QSTR_gamepad;
        case ESP_HID_USAGE_TABLET:
            return MP_QSTR_tablet;
        case ESP_HID_USAGE_CCONTROL:
            return MP_QSTR_consumer;
        case ESP_HID_USAGE_VENDOR:
            return MP_QSTR_vendor;
        default:
            return MP_QSTR_generic;
    }
}

// --- NimBLE task --------------------------------------------------------------

static void queue_report(hid_slot_t *slot, size_t index, struct os_mbuf *om) {
    size_t len = OS_MBUF_PKTLEN(om);
    if (len > REPORT_MAX) {
        len = REPORT_MAX;
    }
    taskENTER_CRITICAL(&_lock);
    // A full queue drops its oldest report: the newest says best what is held
    // down now.
    if (slot->count == QUEUE_LEN) {
        slot->head = (slot->head + 1) % QUEUE_LEN;
        slot->count--;
    }
    queued_report_t *q = &slot->queue[(slot->head + slot->count) % QUEUE_LEN];
    q->len = len;
    q->index = index;
    os_mbuf_copydata(om, 0, len, q->data);
    slot->count++;
    taskEXIT_CRITICAL(&_lock);
}

static int gap_event(struct ble_gap_event *event, void *arg) {
    hid_slot_t *slot = arg;
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                slot->conn = event->connect.conn_handle;
                slot->state = SLOT_CONNECTED;
            } else {
                slot->close_reason = event->connect.status;
                slot->state = SLOT_CLOSED;
            }
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            // A late event for a slot already given back and taken again is
            // not this connection's.
            if (slot->state == SLOT_CONNECTED && event->disconnect.conn.conn_handle == slot->conn) {
                slot->close_reason = event->disconnect.reason;
                slot->state = SLOT_CLOSED;
            }
            break;
        case BLE_GAP_EVENT_ENC_CHANGE:
            slot->enc_status = event->enc_change.status;
            slot->enc_done = true;
            break;
        case BLE_GAP_EVENT_PASSKEY_ACTION:
            // This side pairs as display only: it makes up the passkey, which
            // is typed on the keyboard.
            if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
                struct ble_sm_io io = {
                    .action = BLE_SM_IOACT_DISP,
                    .passkey = esp_random() % 1000000,
                };
                _passkey = io.passkey;
                ble_sm_inject_io(event->passkey.conn_handle, &io);
            }
            break;
        case BLE_GAP_EVENT_REPEAT_PAIRING: {
            // The device forgot the bond and pairs anew: forget it here too.
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
                ble_store_util_delete_peer(&desc.peer_id_addr);
            }
            return BLE_GAP_REPEAT_PAIRING_RETRY;
        }
        case BLE_GAP_EVENT_NOTIFY_RX: {
            const uint16_t handle = event->notify_rx.attr_handle;
            if (handle == slot->battery_handle) {
                uint8_t level;
                if (os_mbuf_copydata(event->notify_rx.om, 0, 1, &level) == 0) {
                    slot->battery = level;
                }
                break;
            }
            for (size_t i = 0; i < slot->n_reports; i++) {
                const hid_report_t *report = &slot->reports[i];
                if (report->handle == handle && report->type == ESP_HID_REPORT_TYPE_INPUT) {
                    queue_report(slot, i, event->notify_rx.om);
                    break;
                }
            }
            break;
        }
        default:
            break;
    }
    return 0;
}

static bool op_current(void *arg) {
    return (uintptr_t)arg == _op_seq;
}

static void op_end(int status) {
    _op_status = status == BLE_HS_EDONE ? 0 : status;
    _op_done = true;
}

static int svc_cb(uint16_t conn, const struct ble_gatt_error *error, const struct ble_gatt_svc *svc, void *arg) {
    if (!op_current(arg)) {
        return BLE_HS_EDONE;
    }
    if (error->status != 0) {
        op_end(error->status);
    } else if (!_svc_found) {
        _svc_found = true;
        _svc_start = svc->start_handle;
        _svc_end = svc->end_handle;
    }
    return 0;
}

static int chr_cb(uint16_t conn, const struct ble_gatt_error *error, const struct ble_gatt_chr *chr, void *arg) {
    if (!op_current(arg)) {
        return BLE_HS_EDONE;
    }
    if (error->status != 0) {
        op_end(error->status);
    } else if (_n_chrs < MAX_CHRS) {
        _chrs[_n_chrs++] = *chr;
    } else {
        _chrs_overflow = true;
    }
    return 0;
}

static int dsc_cb(uint16_t conn, const struct ble_gatt_error *error, uint16_t chr_val_handle,
    const struct ble_gatt_dsc *dsc, void *arg) {
    if (!op_current(arg)) {
        return BLE_HS_EDONE;
    }
    if (error->status != 0) {
        op_end(error->status);
    } else if (dsc->uuid.u.type == BLE_UUID_TYPE_16) {
        if (dsc->uuid.u16.value == UUID_CCCD) {
            _dsc_ccc = dsc->handle;
        } else if (dsc->uuid.u16.value == UUID_REPORT_REFERENCE) {
            _dsc_ref = dsc->handle;
        }
    }
    return 0;
}

static int read_cb(uint16_t conn, const struct ble_gatt_error *error, struct ble_gatt_attr *attr, void *arg) {
    if (!op_current(arg)) {
        return BLE_HS_EDONE;
    }
    if (error->status != 0) {
        op_end(error->status);
        return 0;
    }
    size_t len = OS_MBUF_PKTLEN(attr->om);
    if (_buf_len + len > READ_MAX) {
        op_end(BLE_HS_ENOMEM);
        return BLE_HS_ENOMEM;
    }
    os_mbuf_copydata(attr->om, 0, len, _buf + _buf_len);
    _buf_len += len;
    return 0;
}

static int write_cb(uint16_t conn, const struct ble_gatt_error *error, struct ble_gatt_attr *attr, void *arg) {
    if (op_current(arg)) {
        op_end(error->status);
    }
    return 0;
}

// --- Python task --------------------------------------------------------------

static uint64_t _deadline;
static mp_obj_t _on_passkey;

static void show_passkey(void) {
    int32_t passkey = _passkey;
    if (passkey < 0) {
        return;
    }
    _passkey = -1;
    if (_on_passkey != mp_const_none) {
        mp_call_function_1(_on_passkey, MP_OBJ_NEW_SMALL_INT(passkey));
    } else {
        mp_printf(&mp_plat_print, "Passkey %06d: type it on the device, then Enter\n", (int)passkey);
    }
}

// Runs background tasks until *done; raises on Ctrl-C, the deadline, or the
// connection going away.
static void wait_for(volatile bool *done, hid_slot_t *slot) {
    while (!*done) {
        RUN_BACKGROUND_TASKS;
        show_passkey();
        if (mp_hal_is_interrupted()) {
            mp_handle_pending(true);
        }
        if (slot->state == SLOT_CLOSED) {
            mp_raise_ConnectionError(MP_ERROR_TEXT("HID device disconnected"));
        }
        if (supervisor_ticks_ms64() > _deadline) {
            mp_raise_OSError(MP_ETIMEDOUT);
        }
    }
}

static void *op_begin(void) {
    _op_seq++;
    _op_done = false;
    _op_status = 0;
    return (void *)(uintptr_t)_op_seq;
}

static void op_wait(int rc, hid_slot_t *slot) {
    check_nimble(rc);
    wait_for(&_op_done, slot);
    check_nimble(_op_status);
}

static bool find_service(hid_slot_t *slot, uint16_t uuid16) {
    ble_uuid16_t uuid = BLE_UUID16_INIT(uuid16);
    _svc_found = false;
    void *arg = op_begin();
    op_wait(ble_gattc_disc_svc_by_uuid(slot->conn, &uuid.u, svc_cb, arg), slot);
    return _svc_found;
}

static void find_chrs(hid_slot_t *slot) {
    _n_chrs = 0;
    _chrs_overflow = false;
    void *arg = op_begin();
    op_wait(ble_gattc_disc_all_chrs(slot->conn, _svc_start, _svc_end, chr_cb, arg), slot);
    if (_chrs_overflow) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("HID service too large"));
    }
}

// The descriptors of _chrs[i], which end where the next characteristic begins.
static void find_dscs(hid_slot_t *slot, size_t i) {
    uint16_t end = i + 1 < _n_chrs ? _chrs[i + 1].def_handle - 1 : _svc_end;
    _dsc_ccc = _dsc_ref = 0;
    if (end <= _chrs[i].val_handle) {
        return;
    }
    void *arg = op_begin();
    op_wait(ble_gattc_disc_all_dscs(slot->conn, _chrs[i].val_handle, end, dsc_cb, arg), slot);
}

static void read_attr(hid_slot_t *slot, uint16_t handle) {
    _buf_len = 0;
    void *arg = op_begin();
    op_wait(ble_gattc_read_long(slot->conn, handle, 0, read_cb, arg), slot);
}

static void write_attr(hid_slot_t *slot, uint16_t handle, const void *data, size_t len) {
    void *arg = op_begin();
    op_wait(ble_gattc_write_flat(slot->conn, handle, data, len, write_cb, arg), slot);
}

static uint16_t chr_uuid16(size_t i) {
    return _chrs[i].uuid.u.type == BLE_UUID_TYPE_16 ? _chrs[i].uuid.u16.value : 0;
}

// Encryption, which HID over GATT requires before anything is read. A bonded
// device is encrypted with the stored key; any other is paired, showing a
// passkey if the device has a keyboard.
static void secure(hid_slot_t *slot) {
    for (int attempt = 0; attempt < 2; attempt++) {
        slot->enc_done = false;
        int rc = ble_gap_security_initiate(slot->conn);
        // The device may have asked for security first.
        if (rc != BLE_HS_EALREADY) {
            check_nimble(rc);
        }
        wait_for(&slot->enc_done, slot);
        if (slot->enc_status == 0) {
            return;
        }
        if (slot->enc_status != BLE_HS_HCI_ERR(BLE_ERR_PINKEY_MISSING)) {
            break;
        }
        // The device no longer has the key this side kept: forget it and pair
        // again.
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(slot->conn, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
    }
    mp_raise_OSError_msg_varg(MP_ERROR_TEXT("Pairing failed: %d"), slot->enc_status);
}

// Reads the HID service: the report map, and every report with its id, type
// and CCCD. Returns the report map.
static mp_obj_t read_hid_service(hid_slot_t *slot) {
    if (!find_service(slot, UUID_HID_SERVICE)) {
        mp_raise_ValueError(MP_ERROR_TEXT("Not a HID device"));
    }
    find_chrs(slot);
    mp_obj_t report_map = mp_const_none;
    for (size_t i = 0; i < _n_chrs; i++) {
        uint16_t uuid = chr_uuid16(i);
        if (uuid == UUID_REPORT_MAP) {
            read_attr(slot, _chrs[i].val_handle);
            report_map = mp_obj_new_bytes(_buf, _buf_len);
        } else if (uuid == UUID_REPORT) {
            find_dscs(slot, i);
            if (_dsc_ref == 0) {
                continue;
            }
            uint16_t ccc = _dsc_ccc;
            read_attr(slot, _dsc_ref);
            if (_buf_len < 2) {
                continue;
            }
            if (slot->n_reports == MAX_REPORTS) {
                mp_raise_RuntimeError(MP_ERROR_TEXT("HID service too large"));
            }
            hid_report_t *report = &slot->reports[slot->n_reports];
            report->handle = _chrs[i].val_handle;
            report->ccc = ccc;
            report->id = _buf[0];
            report->type = _buf[1];
            report->usage = ESP_HID_USAGE_GENERIC;
            slot->n_reports++;
        }
    }
    if (report_map == mp_const_none) {
        mp_raise_ValueError(MP_ERROR_TEXT("HID device without a report map"));
    }
    return report_map;
}

// What each report is, from the report map; returns the device's dominant
// usage.
static uint8_t classify_reports(hid_slot_t *slot, mp_obj_t report_map) {
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(report_map, &bufinfo, MP_BUFFER_READ);
    esp_hid_report_map_t *map = esp_hid_parse_report_map(bufinfo.buf, bufinfo.len);
    if (map == NULL) {
        return ESP_HID_USAGE_GENERIC;
    }
    for (size_t i = 0; i < slot->n_reports; i++) {
        hid_report_t *report = &slot->reports[i];
        for (size_t j = 0; j < map->reports_len; j++) {
            const esp_hid_report_item_t *item = &map->reports[j];
            if (item->protocol_mode == ESP_HID_PROTOCOL_MODE_REPORT &&
                item->report_type == report->type && item->report_id == report->id) {
                report->usage = item->usage;
                break;
            }
        }
    }
    uint8_t usage = map->usage;
    esp_hid_free_report_map(map);
    return usage;
}

// The battery level, and its notifications where the device sends them.
static void read_battery(hid_slot_t *slot) {
    if (!find_service(slot, UUID_BATTERY_SERVICE)) {
        return;
    }
    find_chrs(slot);
    for (size_t i = 0; i < _n_chrs; i++) {
        if (chr_uuid16(i) != UUID_BATTERY_LEVEL) {
            continue;
        }
        read_attr(slot, _chrs[i].val_handle);
        if (_buf_len >= 1) {
            slot->battery = _buf[0];
        }
        if (_chrs[i].properties & BLE_GATT_CHR_PROP_NOTIFY) {
            find_dscs(slot, i);
            if (_dsc_ccc != 0) {
                slot->battery_handle = _chrs[i].val_handle;
                const uint8_t on[2] = { 1, 0 };
                write_attr(slot, _dsc_ccc, on, sizeof(on));
            }
        }
        return;
    }
}

// Takes the connection down, if there is one, and gives the slot back.
static void slot_release(hid_slot_t *slot) {
    if (slot->state == SLOT_CONNECTING) {
        ble_gap_conn_cancel();
    } else if (slot->state == SLOT_CONNECTED) {
        ble_gap_terminate(slot->conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    uint64_t until = supervisor_ticks_ms64() + CLOSE_WAIT_MS;
    while (slot->state != SLOT_CLOSED && supervisor_ticks_ms64() < until) {
        RUN_BACKGROUND_TASKS;
    }
    slot->state = SLOT_FREE;
}

static void slot_setup(espidf_ble_hid_obj_t *self, hid_slot_t *slot, const ble_addr_t *addr) {
    uint8_t own_addr_type;
    check_nimble(ble_hs_id_infer_auto(0, &own_addr_type));
    int32_t remaining = _deadline - supervisor_ticks_ms64();
    slot->state = SLOT_CONNECTING;
    int rc = ble_gap_connect(own_addr_type, addr, remaining > 0 ? remaining : 1, NULL, gap_event, slot);
    if (rc != 0) {
        slot->state = SLOT_FREE;
        check_nimble(rc);
    }
    while (slot->state == SLOT_CONNECTING) {
        RUN_BACKGROUND_TASKS;
        if (mp_hal_is_interrupted()) {
            mp_handle_pending(true);
        }
    }
    if (slot->state != SLOT_CONNECTED) {
        // NimBLE ends a connection attempt that ran out of time with
        // BLE_HS_ETIMEOUT.
        if (slot->close_reason == BLE_HS_ETIMEOUT) {
            mp_raise_OSError(MP_ETIMEDOUT);
        }
        mp_raise_ConnectionError(MP_ERROR_TEXT("HID device disconnected"));
    }
    // A longer ATT MTU lets longer reports through whole. Nothing waits for it.
    ble_gattc_exchange_mtu(slot->conn, NULL, NULL);

    // Pairing as display only with MITM protection: a keyboard is then asked
    // for a passkey, which a device without one skips. _bleio's own setting
    // comes back afterwards.
    uint8_t io_cap = ble_hs_cfg.sm_io_cap;
    unsigned mitm = ble_hs_cfg.sm_mitm;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_DISPLAY_ONLY;
    ble_hs_cfg.sm_mitm = 1;
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        secure(slot);
        nlr_pop();
        ble_hs_cfg.sm_io_cap = io_cap;
        ble_hs_cfg.sm_mitm = mitm;
    } else {
        ble_hs_cfg.sm_io_cap = io_cap;
        ble_hs_cfg.sm_mitm = mitm;
        nlr_jump(nlr.ret_val);
    }

    self->report_map = read_hid_service(slot);
    self->usage = classify_reports(slot, self->report_map);
    read_battery(slot);
    for (size_t i = 0; i < slot->n_reports; i++) {
        const hid_report_t *report = &slot->reports[i];
        if (report->type == ESP_HID_REPORT_TYPE_INPUT && report->ccc != 0) {
            const uint8_t on[2] = { 1, 0 };
            write_attr(slot, report->ccc, on, sizeof(on));
        }
    }
}

//| class BLEHIDDevice:
//|     """A Bluetooth LE keyboard, mouse, game controller or remote control
//|     (HID over GATT), connected, paired and read as reports. Starts BLE if
//|     `_bleio.adapter` has not. Up to two at a time.
//|
//|     Find the device with `_bleio.Adapter.start_scan` (it advertises the HID
//|     service, 0x1812, and must be in pairing mode the first time) and stop
//|     scanning before connecting.
//|
//|     The first connection pairs and bonds; later ones use the stored key. A
//|     device with a keyboard is asked for a passkey, which this side makes up
//|     and shows through ``on_passkey``. The bond is kept in NVS with
//|     `_bleio`'s, and `_bleio.Adapter.erase_bonding` forgets it."""
//|
//|     def __init__(
//|         self,
//|         address: _bleio.Address,
//|         *,
//|         timeout: float = 30.0,
//|         on_passkey: Optional[Callable[[int], None]] = None,
//|     ) -> None:
//|         """
//|         :param ~_bleio.Address address: the device's address, from a scan.
//|         :param float timeout: seconds for connecting, pairing (typing the
//|           passkey included) and reading the device; raises OSError ETIMEDOUT.
//|         :param on_passkey: called with the passkey (0 to 999999, to be shown
//|           with six digits) to type on the device followed by Enter. None
//|           prints it.
//|         """
//|         ...
//|
static mp_obj_t espidf_ble_hid_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_address, ARG_timeout, ARG_on_passkey };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_address, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_timeout, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = MP_OBJ_NULL } },
        { MP_QSTR_on_passkey, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = mp_const_none } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    bleio_address_obj_t *address = mp_arg_validate_type(args[ARG_address].u_obj, &bleio_address_type, MP_QSTR_address);
    mp_float_t timeout = args[ARG_timeout].u_obj == MP_OBJ_NULL ? 30 : mp_obj_get_float(args[ARG_timeout].u_obj);
    mp_arg_validate_float_range(timeout, 1, 300, MP_QSTR_timeout);
    if (args[ARG_on_passkey].u_obj != mp_const_none && !mp_obj_is_callable(args[ARG_on_passkey].u_obj)) {
        mp_raise_TypeError(MP_ERROR_TEXT("on_passkey must be callable"));
    }

    hid_slot_t *slot = NULL;
    for (size_t i = 0; i < MAX_DEVICES; i++) {
        if (_slots[i].state == SLOT_FREE) {
            slot = &_slots[i];
            break;
        }
    }
    if (slot == NULL) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("Too many HID devices"));
    }
    if (!common_hal_bleio_adapter_get_enabled(&common_hal_bleio_adapter_obj)) {
        common_hal_bleio_adapter_set_enabled(&common_hal_bleio_adapter_obj, true);
    }

    espidf_ble_hid_obj_t *self = mp_obj_malloc_with_finaliser(espidf_ble_hid_obj_t, &espidf_ble_hid_type);
    self->slot = NULL;
    self->report_map = mp_const_none;
    self->usage = ESP_HID_USAGE_GENERIC;

    slot->n_reports = 0;
    slot->battery_handle = 0;
    slot->battery = -1;
    slot->head = slot->count = 0;
    slot->close_reason = 0;
    _passkey = -1;
    _on_passkey = args[ARG_on_passkey].u_obj;
    _deadline = supervisor_ticks_ms64() + (uint64_t)(timeout * 1000);

    ble_addr_t addr;
    addr.type = address->type;
    memcpy(addr.val, address->bytes, 6);

    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        slot_setup(self, slot, &addr);
        nlr_pop();
    } else {
        _on_passkey = mp_const_none;
        slot_release(slot);
        nlr_jump(nlr.ret_val);
    }
    _on_passkey = mp_const_none;
    self->slot = slot;
    return MP_OBJ_FROM_PTR(self);
}

static espidf_ble_hid_obj_t *get_open(mp_obj_t self_in) {
    espidf_ble_hid_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->slot == NULL) {
        raise_deinited_error();
    }
    return self;
}

//|     def read(self) -> Optional[Tuple[str, int, bytes]]:
//|         """The oldest report not yet read, as ``(usage, report_id, data)``,
//|         or None if there is none. ``usage`` is what the report map says the
//|         report is: ``"keyboard"``, ``"mouse"``, ``"consumer"`` (media keys),
//|         ``"gamepad"``, ``"joystick"``, ``"tablet"``, ``"vendor"`` or
//|         ``"generic"``. ``data`` is the report without its id, at most 64
//|         bytes. The last 16 reports are kept; older ones are dropped. Once
//|         those are read, raises ConnectionError if the device has
//|         disconnected."""
//|         ...
//|
static mp_obj_t espidf_ble_hid_read(mp_obj_t self_in) {
    hid_slot_t *slot = get_open(self_in)->slot;
    queued_report_t report;
    bool got = false;
    taskENTER_CRITICAL(&_lock);
    if (slot->count > 0) {
        report = slot->queue[slot->head];
        slot->head = (slot->head + 1) % QUEUE_LEN;
        slot->count--;
        got = true;
    }
    taskEXIT_CRITICAL(&_lock);
    if (!got) {
        if (slot->state == SLOT_CLOSED) {
            mp_raise_ConnectionError(MP_ERROR_TEXT("HID device disconnected"));
        }
        return mp_const_none;
    }
    const hid_report_t *from = &slot->reports[report.index];
    mp_obj_t items[3] = {
        MP_OBJ_NEW_QSTR(usage_qstr(from->usage)),
        MP_OBJ_NEW_SMALL_INT(from->id),
        mp_obj_new_bytes(report.data, report.len),
    };
    return mp_obj_new_tuple(3, items);
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_ble_hid_read_obj, espidf_ble_hid_read);

//|     def set_output(self, report_id: int, data: ReadableBuffer) -> None:
//|         """Sends an output report, e.g. a keyboard's LEDs: report 0 or the
//|         keyboard's id, one byte, bit 0 Num Lock, 1 Caps Lock, 2 Scroll Lock.
//|         Raises ValueError if the device has no such output report."""
//|         ...
//|
static mp_obj_t espidf_ble_hid_set_output(mp_obj_t self_in, mp_obj_t report_id_in, mp_obj_t data_in) {
    hid_slot_t *slot = get_open(self_in)->slot;
    mp_int_t report_id = mp_arg_validate_int_range(mp_obj_get_int(report_id_in), 0, 255, MP_QSTR_report_id);
    mp_buffer_info_t data;
    mp_get_buffer_raise(data_in, &data, MP_BUFFER_READ);
    mp_arg_validate_length_max(data.len, READ_MAX, MP_QSTR_data);
    for (size_t i = 0; i < slot->n_reports; i++) {
        const hid_report_t *report = &slot->reports[i];
        if (report->type == ESP_HID_REPORT_TYPE_OUTPUT && report->id == report_id) {
            _deadline = supervisor_ticks_ms64() + OUTPUT_TIMEOUT_MS;
            write_attr(slot, report->handle, data.buf, data.len);
            return mp_const_none;
        }
    }
    mp_raise_ValueError_varg(MP_ERROR_TEXT("Invalid %q"), MP_QSTR_report_id);
}
static MP_DEFINE_CONST_FUN_OBJ_3(espidf_ble_hid_set_output_obj, espidf_ble_hid_set_output);

//|     connected: bool
//|     """Whether the device is still connected."""
//|
static mp_obj_t espidf_ble_hid_get_connected(mp_obj_t self_in) {
    espidf_ble_hid_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_bool(self->slot != NULL && self->slot->state == SLOT_CONNECTED);
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_ble_hid_get_connected_obj, espidf_ble_hid_get_connected);
MP_PROPERTY_GETTER(espidf_ble_hid_connected_obj, (mp_obj_t)&espidf_ble_hid_get_connected_obj);

//|     battery: Optional[int]
//|     """Battery level in percent: read when connecting, and kept up to date
//|     if the device sends it. None without a battery service."""
//|
static mp_obj_t espidf_ble_hid_get_battery(mp_obj_t self_in) {
    int battery = get_open(self_in)->slot->battery;
    return battery < 0 ? mp_const_none : MP_OBJ_NEW_SMALL_INT(battery);
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_ble_hid_get_battery_obj, espidf_ble_hid_get_battery);
MP_PROPERTY_GETTER(espidf_ble_hid_battery_obj, (mp_obj_t)&espidf_ble_hid_get_battery_obj);

//|     usage: str
//|     """What the device is, by its report map: the usage of `read`, the
//|     keyboard winning over the mouse when it has both."""
//|
static mp_obj_t espidf_ble_hid_get_usage(mp_obj_t self_in) {
    return MP_OBJ_NEW_QSTR(usage_qstr(get_open(self_in)->usage));
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_ble_hid_get_usage_obj, espidf_ble_hid_get_usage);
MP_PROPERTY_GETTER(espidf_ble_hid_usage_obj, (mp_obj_t)&espidf_ble_hid_get_usage_obj);

//|     report_map: bytes
//|     """The HID report descriptor, for decoding reports whose layout is not
//|     the usual one."""
//|
static mp_obj_t espidf_ble_hid_get_report_map(mp_obj_t self_in) {
    return get_open(self_in)->report_map;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_ble_hid_get_report_map_obj, espidf_ble_hid_get_report_map);
MP_PROPERTY_GETTER(espidf_ble_hid_report_map_obj, (mp_obj_t)&espidf_ble_hid_get_report_map_obj);

//|     def deinit(self) -> None:
//|         """Disconnects. The bond stays."""
//|         ...
//|
//|
static mp_obj_t espidf_ble_hid_deinit(mp_obj_t self_in) {
    espidf_ble_hid_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->slot != NULL) {
        slot_release(self->slot);
        self->slot = NULL;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_ble_hid_deinit_obj, espidf_ble_hid_deinit);

static mp_obj_t espidf_ble_hid___exit__(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    return espidf_ble_hid_deinit(args[0]);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(espidf_ble_hid___exit___obj, 4, 4, espidf_ble_hid___exit__);

static const mp_rom_map_elem_t espidf_ble_hid_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&espidf_ble_hid_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_output), MP_ROM_PTR(&espidf_ble_hid_set_output_obj) },
    { MP_ROM_QSTR(MP_QSTR_connected), MP_ROM_PTR(&espidf_ble_hid_connected_obj) },
    { MP_ROM_QSTR(MP_QSTR_battery), MP_ROM_PTR(&espidf_ble_hid_battery_obj) },
    { MP_ROM_QSTR(MP_QSTR_usage), MP_ROM_PTR(&espidf_ble_hid_usage_obj) },
    { MP_ROM_QSTR(MP_QSTR_report_map), MP_ROM_PTR(&espidf_ble_hid_report_map_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&espidf_ble_hid_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&espidf_ble_hid_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&espidf_ble_hid___exit___obj) },
};
static MP_DEFINE_CONST_DICT(espidf_ble_hid_locals_dict, espidf_ble_hid_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    espidf_ble_hid_type,
    MP_QSTR_BLEHIDDevice,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, espidf_ble_hid_make_new,
    locals_dict, &espidf_ble_hid_locals_dict
    );

void espidf_ble_hid_reset(void) {
    _on_passkey = mp_const_none;
    for (size_t i = 0; i < MAX_DEVICES; i++) {
        if (_slots[i].state != SLOT_FREE) {
            slot_release(&_slots[i]);
        }
    }
}

#endif
