// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Petr Vavrin
//
// SPDX-License-Identifier: MIT

// espidf.PeriodicAdvertiser and espidf.PeriodicSync; see extended.h.

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
#if CIRCUITPY_ESPIDF_BLE_PERIODIC

#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "os/os_mbuf.h"

// Bluetooth 5 periodic advertising: data broadcast on a fixed schedule,
// which a receiver synchronises to and then listens for only at those
// moments. Unlike ordinary advertising it carries up to 1650 bytes and needs
// no scanning once synchronised.
//
// The advertiser is extended advertising instance 1; _bleio's own
// advertising uses instance 0, so both can run.

#define PERIODIC_INSTANCE (1)
#define PERIODIC_SID (1)
#define PERIODIC_MAX_DATA (CONFIG_BT_NIMBLE_EXT_ADV_MAX_SIZE)
// What one LE Set Periodic Advertising Data command carries. Longer data goes
// in fragments, which the specification allows only while periodic
// advertising is off: an update of more than this while broadcasting was
// refused with BLE_HS_EINVAL.
#define PERIODIC_MAX_UPDATE (252)

static void ensure_ble(void) {
    if (!common_hal_bleio_adapter_get_enabled(&common_hal_bleio_adapter_obj)) {
        common_hal_bleio_adapter_set_enabled(&common_hal_bleio_adapter_obj, true);
    }
}

static void check_nimble(int rc) {
    if (rc != 0) {
        mp_raise_OSError_msg_varg(MP_ERROR_TEXT("NimBLE error %d"), rc);
    }
}

// --- PeriodicAdvertiser ------------------------------------------------------

typedef struct {
    mp_obj_base_t base;
    bool running;
    uint16_t interval_ms;
} espidf_periodic_adv_obj_t;

static espidf_periodic_adv_obj_t *_adv;

static int adv_gap_event(struct ble_gap_event *event, void *arg) {
    return 0;
}

static void adv_stop(espidf_periodic_adv_obj_t *self) {
    if (!self->running) {
        return;
    }
    ble_gap_periodic_adv_stop(PERIODIC_INSTANCE);
    ble_gap_ext_adv_stop(PERIODIC_INSTANCE);
    ble_gap_ext_adv_remove(PERIODIC_INSTANCE);
    self->running = false;
    if (_adv == self) {
        _adv = NULL;
    }
}

static void adv_set_data(mp_obj_t data_in, bool running) {
    mp_buffer_info_t data;
    mp_get_buffer_raise(data_in, &data, MP_BUFFER_READ);
    mp_arg_validate_length_max(data.len, running ? PERIODIC_MAX_UPDATE : PERIODIC_MAX_DATA, MP_QSTR_data);
    struct os_mbuf *buf = os_msys_get_pkthdr(data.len, 0);
    if (buf == NULL) {
        mp_raise_msg(&mp_type_MemoryError, NULL);
    }
    int rc = os_mbuf_append(buf, data.buf, data.len);
    if (rc != 0) {
        os_mbuf_free_chain(buf);
        check_nimble(rc);
    }
    // The stack takes the chain, whether it succeeds or not.
    #if MYNEWT_VAL(BLE_PERIODIC_ADV_ENH)
    check_nimble(ble_gap_periodic_adv_set_data(PERIODIC_INSTANCE, buf, NULL));
    #else
    check_nimble(ble_gap_periodic_adv_set_data(PERIODIC_INSTANCE, buf));
    #endif
}

//| class PeriodicAdvertiser:
//|     """Bluetooth 5 periodic advertising: ``data`` broadcast every
//|     ``interval_ms``, for `PeriodicSync` on another device. Uses extended
//|     advertising instance 1, so `_bleio`'s own advertising can run beside it.
//|     Starts BLE if `_bleio.adapter` has not."""
//|
//|     def __init__(self, data: ReadableBuffer, *, interval_ms: int = 100) -> None:
//|         """
//|         :param data: up to 1650 bytes, e.g. advertising-data structures or
//|           anything else; the receiver gets it as is. Later updates through
//|           `data` are limited to 252.
//|         :param int interval_ms: 7.5 to 81918 ms."""
//|         ...
//|
static mp_obj_t espidf_periodic_adv_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_data, ARG_interval_ms };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_data, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_interval_ms, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 100 } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    mp_int_t interval_ms = mp_arg_validate_int_range(args[ARG_interval_ms].u_int, 8, 81918, MP_QSTR_interval_ms);
    if (_adv != NULL) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("PeriodicAdvertiser in use"));
    }
    ensure_ble();

    espidf_periodic_adv_obj_t *self = mp_obj_malloc_with_finaliser(espidf_periodic_adv_obj_t, &espidf_periodic_adv_type);
    self->running = false;
    self->interval_ms = interval_ms;

    uint8_t own_addr_type;
    check_nimble(ble_hs_id_infer_auto(0, &own_addr_type));
    // Non-connectable, non-scannable extended advertising: what periodic
    // advertising requires. Its own interval only paces the pointers to the
    // periodic train.
    struct ble_gap_ext_adv_params params = {
        .own_addr_type = own_addr_type,
        .primary_phy = BLE_HCI_LE_PHY_1M,
        .secondary_phy = BLE_HCI_LE_PHY_1M,
        .sid = PERIODIC_SID,
        .itvl_min = BLE_GAP_ADV_ITVL_MS(100),
        .itvl_max = BLE_GAP_ADV_ITVL_MS(100),
        .tx_power = 127,
    };
    check_nimble(ble_gap_ext_adv_configure(PERIODIC_INSTANCE, &params, NULL, adv_gap_event, NULL));
    struct ble_gap_periodic_adv_params periodic = {
        .itvl_min = (interval_ms * 4 + 4) / 5,
        .itvl_max = (interval_ms * 4 + 4) / 5,
    };
    int rc = ble_gap_periodic_adv_configure(PERIODIC_INSTANCE, &periodic);
    if (rc == 0) {
        nlr_buf_t nlr;
        if (nlr_push(&nlr) == 0) {
            adv_set_data(args[ARG_data].u_obj, false);
            nlr_pop();
        } else {
            ble_gap_ext_adv_remove(PERIODIC_INSTANCE);
            nlr_jump(nlr.ret_val);
        }
        rc = ble_gap_periodic_adv_start(PERIODIC_INSTANCE);
    }
    if (rc == 0) {
        rc = ble_gap_ext_adv_start(PERIODIC_INSTANCE, 0, 0);
    }
    if (rc != 0) {
        ble_gap_periodic_adv_stop(PERIODIC_INSTANCE);
        ble_gap_ext_adv_remove(PERIODIC_INSTANCE);
        check_nimble(rc);
    }
    self->running = true;
    _adv = self;
    return MP_OBJ_FROM_PTR(self);
}

//|     data: ReadableBuffer
//|     """Write-only: what is broadcast from the next period on. At most 252
//|     bytes here: longer data goes in fragments, which Bluetooth allows only
//|     before broadcasting starts -- give it to the constructor."""
//|
static mp_obj_t espidf_periodic_adv_set_data_attr(mp_obj_t self_in, mp_obj_t value) {
    espidf_periodic_adv_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->running) {
        raise_deinited_error();
    }
    adv_set_data(value, true);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(espidf_periodic_adv_set_data_attr_obj, espidf_periodic_adv_set_data_attr);
MP_PROPERTY_GETSET(espidf_periodic_adv_data_obj, MP_ROM_NONE, (mp_obj_t)&espidf_periodic_adv_set_data_attr_obj);

//|     def deinit(self) -> None:
//|         """Stop broadcasting."""
//|         ...
//|
static mp_obj_t espidf_periodic_adv_deinit(mp_obj_t self_in) {
    adv_stop(MP_OBJ_TO_PTR(self_in));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_periodic_adv_deinit_obj, espidf_periodic_adv_deinit);

static mp_obj_t espidf_periodic_adv___exit__(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    return espidf_periodic_adv_deinit(args[0]);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(espidf_periodic_adv___exit___obj, 4, 4, espidf_periodic_adv___exit__);

static const mp_rom_map_elem_t espidf_periodic_adv_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_data), MP_ROM_PTR(&espidf_periodic_adv_data_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&espidf_periodic_adv_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&espidf_periodic_adv_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&espidf_periodic_adv___exit___obj) },
};
static MP_DEFINE_CONST_DICT(espidf_periodic_adv_locals_dict, espidf_periodic_adv_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    espidf_periodic_adv_type,
    MP_QSTR_PeriodicAdvertiser,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, espidf_periodic_adv_make_new,
    locals_dict, &espidf_periodic_adv_locals_dict
    );

// --- PeriodicSync ------------------------------------------------------------

typedef struct {
    mp_obj_base_t base;
    bool active;
} espidf_periodic_sync_obj_t;

static espidf_periodic_sync_obj_t *_sync;

// Shared with the NimBLE host task, under _lock.
static portMUX_TYPE _lock = portMUX_INITIALIZER_UNLOCKED;
static volatile enum { SYNC_IDLE, SYNC_SCANNING, SYNC_CREATING, SYNC_ESTABLISHED, SYNC_LOST, SYNC_FAILED } _state;
static ble_addr_t _want_addr;
static bool _want_any;
static ble_addr_t _found_addr;
static uint8_t _found_sid;
static uint16_t _sync_handle;
static uint16_t _interval_1_25ms;
static uint8_t _assembly[PERIODIC_MAX_DATA];
static size_t _assembly_len;
static uint8_t _latest[PERIODIC_MAX_DATA];
static size_t _latest_len;
static volatile bool _fresh;
static volatile uint32_t _reports;
static volatile int8_t _rssi;

static int sync_gap_event(struct ble_gap_event *event, void *arg);

static int scan_gap_event(struct ble_gap_event *event, void *arg) {
    if (event->type != BLE_GAP_EVENT_EXT_DISC || _state != SYNC_SCANNING) {
        return 0;
    }
    const struct ble_gap_ext_disc_desc *d = &event->ext_disc;
    if (d->periodic_adv_itvl == 0) {
        return 0;
    }
    if (!_want_any && (d->addr.type != _want_addr.type || memcmp(d->addr.val, _want_addr.val, 6) != 0)) {
        return 0;
    }
    _found_addr = d->addr;
    _found_sid = d->sid;
    _state = SYNC_CREATING;
    struct ble_gap_periodic_sync_params params = { .skip = 0, .sync_timeout = 1000 };
    if (ble_gap_periodic_adv_sync_create(&_found_addr, _found_sid, &params, sync_gap_event, NULL) != 0) {
        _state = SYNC_FAILED;
    }
    return 0;
}

static int sync_gap_event(struct ble_gap_event *event, void *arg) {
    switch (event->type) {
        case BLE_GAP_EVENT_PERIODIC_SYNC:
            if (event->periodic_sync.status == 0) {
                _sync_handle = event->periodic_sync.sync_handle;
                _interval_1_25ms = event->periodic_sync.per_adv_ival;
                _state = SYNC_ESTABLISHED;
            } else {
                _state = SYNC_FAILED;
            }
            break;
        case BLE_GAP_EVENT_PERIODIC_REPORT: {
            const uint8_t status = event->periodic_report.data_status;
            const size_t len = event->periodic_report.data_length;
            taskENTER_CRITICAL(&_lock);
            if (status == BLE_HCI_PERIODIC_DATA_STATUS_RX_FAILED || _assembly_len + len > sizeof(_assembly)) {
                _assembly_len = 0;
            } else {
                memcpy(_assembly + _assembly_len, event->periodic_report.data, len);
                _assembly_len += len;
                // A long payload comes in pieces, marked incomplete until the last.
                if (status != BLE_HCI_PERIODIC_DATA_STATUS_INCOMPLETE) {
                    memcpy(_latest, _assembly, _assembly_len);
                    _latest_len = _assembly_len;
                    _assembly_len = 0;
                    _fresh = true;
                    _reports++;
                    _rssi = event->periodic_report.rssi;
                }
            }
            taskEXIT_CRITICAL(&_lock);
            break;
        }
        case BLE_GAP_EVENT_PERIODIC_SYNC_LOST:
            _state = SYNC_LOST;
            break;
        default:
            break;
    }
    return 0;
}

static void sync_stop(espidf_periodic_sync_obj_t *self) {
    if (!self->active) {
        return;
    }
    if (_state == SYNC_SCANNING) {
        ble_gap_disc_cancel();
    } else if (_state == SYNC_CREATING) {
        ble_gap_disc_cancel();
        ble_gap_periodic_adv_sync_create_cancel();
    } else if (_state == SYNC_ESTABLISHED) {
        ble_gap_periodic_adv_sync_terminate(_sync_handle);
    }
    _state = SYNC_IDLE;
    self->active = false;
    if (_sync == self) {
        _sync = NULL;
    }
}

//| class PeriodicSync:
//|     """Receives Bluetooth 5 periodic advertising: finds a periodic
//|     advertiser by scanning, synchronises to its schedule, and from then on
//|     receives each broadcast without scanning. Starts BLE if `_bleio.adapter`
//|     has not; do not scan with `_bleio` meanwhile."""
//|
//|     def __init__(self, *, address: Optional[_bleio.Address] = None, timeout: float = 10.0) -> None:
//|         """
//|         :param ~_bleio.Address address: the advertiser's address, or None
//|           for the first periodic advertiser heard.
//|         :param float timeout: seconds to wait for it; raises OSError ETIMEDOUT.
//|         """
//|         ...
//|
static mp_obj_t espidf_periodic_sync_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_address, ARG_timeout };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_address, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = mp_const_none } },
        { MP_QSTR_timeout, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = MP_OBJ_NULL } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    mp_float_t timeout = args[ARG_timeout].u_obj == MP_OBJ_NULL ? 10 : mp_obj_get_float(args[ARG_timeout].u_obj);
    if (_sync != NULL) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("PeriodicSync in use"));
    }
    _want_any = args[ARG_address].u_obj == mp_const_none;
    if (!_want_any) {
        bleio_address_obj_t *address = mp_arg_validate_type(args[ARG_address].u_obj, &bleio_address_type, MP_QSTR_address);
        memcpy(_want_addr.val, address->bytes, 6);
        _want_addr.type = address->type;
    }
    ensure_ble();

    espidf_periodic_sync_obj_t *self = mp_obj_malloc_with_finaliser(espidf_periodic_sync_obj_t, &espidf_periodic_sync_type);
    self->active = false;
    _assembly_len = _latest_len = 0;
    _fresh = false;
    _reports = 0;
    _state = SYNC_SCANNING;

    uint8_t own_addr_type;
    check_nimble(ble_hs_id_infer_auto(0, &own_addr_type));
    struct ble_gap_ext_disc_params scan = { .itvl = BLE_GAP_SCAN_ITVL_MS(50), .window = BLE_GAP_SCAN_WIN_MS(50), .passive = 1 };
    int rc = ble_gap_ext_disc(own_addr_type, 0, 0, 0, 0, 0, &scan, NULL, scan_gap_event, NULL);
    if (rc != 0) {
        _state = SYNC_IDLE;
        check_nimble(rc);
    }
    self->active = true;
    _sync = self;

    uint64_t deadline = supervisor_ticks_ms64() + (uint64_t)(timeout * 1000);
    while (_state == SYNC_SCANNING || _state == SYNC_CREATING) {
        RUN_BACKGROUND_TASKS;
        if (mp_hal_is_interrupted() || supervisor_ticks_ms64() > deadline) {
            sync_stop(self);
            if (mp_hal_is_interrupted()) {
                mp_handle_pending(true);
            }
            mp_raise_OSError(MP_ETIMEDOUT);
        }
    }
    // Synchronised (or failed): the scan has done its job.
    ble_gap_disc_cancel();
    if (_state != SYNC_ESTABLISHED) {
        sync_stop(self);
        mp_raise_OSError_msg(MP_ERROR_TEXT("Periodic sync failed"));
    }
    return MP_OBJ_FROM_PTR(self);
}

//|     def read(self) -> Optional[bytes]:
//|         """The latest complete broadcast, or None if none has come since the
//|         last read. Raises OSError if the advertiser has been lost."""
//|         ...
//|
static mp_obj_t espidf_periodic_sync_read(mp_obj_t self_in) {
    espidf_periodic_sync_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->active) {
        raise_deinited_error();
    }
    if (_state == SYNC_LOST) {
        mp_raise_OSError_msg(MP_ERROR_TEXT("Periodic advertiser lost"));
    }
    if (!_fresh) {
        return mp_const_none;
    }
    // Copied out under the lock, which no allocation may happen inside.
    vstr_t out;
    vstr_init_len(&out, PERIODIC_MAX_DATA);
    taskENTER_CRITICAL(&_lock);
    size_t len = _latest_len;
    memcpy(out.buf, _latest, len);
    _fresh = false;
    taskEXIT_CRITICAL(&_lock);
    out.len = len;
    return mp_obj_new_bytes_from_vstr(&out);
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_periodic_sync_read_obj, espidf_periodic_sync_read);

//|     rssi: int
//|     """Signal strength of the latest broadcast, dBm."""
//|
//|     reports: int
//|     """Complete broadcasts received since the sync was made."""
//|
//|     interval_ms: float
//|     """The advertiser's period."""
//|
//|     address: bytes
//|     """The advertiser's address (6 bytes, least significant first)."""
//|
//|
static mp_obj_t espidf_periodic_sync_get_rssi(mp_obj_t self_in) {
    return MP_OBJ_NEW_SMALL_INT(_rssi);
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_periodic_sync_get_rssi_obj, espidf_periodic_sync_get_rssi);
MP_PROPERTY_GETTER(espidf_periodic_sync_rssi_obj, (mp_obj_t)&espidf_periodic_sync_get_rssi_obj);

static mp_obj_t espidf_periodic_sync_get_reports(mp_obj_t self_in) {
    return mp_obj_new_int_from_uint(_reports);
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_periodic_sync_get_reports_obj, espidf_periodic_sync_get_reports);
MP_PROPERTY_GETTER(espidf_periodic_sync_reports_obj, (mp_obj_t)&espidf_periodic_sync_get_reports_obj);

static mp_obj_t espidf_periodic_sync_get_interval(mp_obj_t self_in) {
    return mp_obj_new_float(_interval_1_25ms * 1.25f);
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_periodic_sync_get_interval_obj, espidf_periodic_sync_get_interval);
MP_PROPERTY_GETTER(espidf_periodic_sync_interval_obj, (mp_obj_t)&espidf_periodic_sync_get_interval_obj);

static mp_obj_t espidf_periodic_sync_get_address(mp_obj_t self_in) {
    return mp_obj_new_bytes(_found_addr.val, 6);
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_periodic_sync_get_address_obj, espidf_periodic_sync_get_address);
MP_PROPERTY_GETTER(espidf_periodic_sync_address_obj, (mp_obj_t)&espidf_periodic_sync_get_address_obj);

static mp_obj_t espidf_periodic_sync_deinit(mp_obj_t self_in) {
    sync_stop(MP_OBJ_TO_PTR(self_in));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_periodic_sync_deinit_obj, espidf_periodic_sync_deinit);

static mp_obj_t espidf_periodic_sync___exit__(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    return espidf_periodic_sync_deinit(args[0]);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(espidf_periodic_sync___exit___obj, 4, 4, espidf_periodic_sync___exit__);

static const mp_rom_map_elem_t espidf_periodic_sync_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&espidf_periodic_sync_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_rssi), MP_ROM_PTR(&espidf_periodic_sync_rssi_obj) },
    { MP_ROM_QSTR(MP_QSTR_reports), MP_ROM_PTR(&espidf_periodic_sync_reports_obj) },
    { MP_ROM_QSTR(MP_QSTR_interval_ms), MP_ROM_PTR(&espidf_periodic_sync_interval_obj) },
    { MP_ROM_QSTR(MP_QSTR_address), MP_ROM_PTR(&espidf_periodic_sync_address_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&espidf_periodic_sync_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&espidf_periodic_sync_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&espidf_periodic_sync___exit___obj) },
};
static MP_DEFINE_CONST_DICT(espidf_periodic_sync_locals_dict, espidf_periodic_sync_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    espidf_periodic_sync_type,
    MP_QSTR_PeriodicSync,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, espidf_periodic_sync_make_new,
    locals_dict, &espidf_periodic_sync_locals_dict
    );

void espidf_ble_periodic_reset(void) {
    if (_adv != NULL) {
        adv_stop(_adv);
    }
    if (_sync != NULL) {
        sync_stop(_sync);
    }
}

#endif
