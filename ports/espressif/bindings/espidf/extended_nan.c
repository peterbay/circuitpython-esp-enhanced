// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Petr Vavrin
//
// SPDX-License-Identifier: MIT

// espidf.WiFiAware; see extended.h.

#include <string.h>

#include "py/obj.h"
#include "py/objproperty.h"
#include "py/objstr.h"
#include "py/runtime.h"

#include "bindings/espidf/__init__.h"
#include "bindings/espidf/extended.h"
#include "shared-bindings/util.h"
#include "shared-bindings/wifi/__init__.h"
#include "shared-bindings/wifi/Radio.h"
#include "shared/runtime/context_manager_helpers.h"

#include "esp_event.h"
#include "esp_wifi.h"

// The headers below exist only on chips with Wi-Fi Aware.
#if CIRCUITPY_ESPIDF_NAN

#include "esp_nan.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// Wi-Fi Aware (NAN): devices that agree on a schedule of discovery windows
// find each other's services by name and exchange short messages, with no
// access point. This covers discovery and those messages (follow-ups), not
// the datapath, which would need a network interface of its own beside
// wifi's.
//
// NAN takes the radio for itself: the IDF switches Wi-Fi to NAN mode, so the
// station (and any access point) of wifi.radio stops while it runs and is
// brought back afterwards.

#define NAN_QUEUE_DEPTH (16)

typedef enum { NAN_EV_MATCH, NAN_EV_REPLIED, NAN_EV_MESSAGE } nan_event_kind_t;

// Copied out of the event loop's task into the IDF heap, and freed by read().
typedef struct {
    nan_event_kind_t kind;
    uint8_t own_id;
    uint8_t peer_id;
    uint8_t peer_mac[6];
    uint16_t len;
    uint8_t data[];
} nan_event_t;

typedef struct {
    mp_obj_base_t base;
    bool running;
} espidf_wifi_aware_obj_t;

static espidf_wifi_aware_obj_t *_aware;
static QueueHandle_t _queue;
static esp_event_handler_instance_t _handler;
static volatile uint32_t _dropped;
static wifi_mode_t _saved_mode;
static bool _radio_was_started;
// The IDF only marks NAN started once its default handler has a NAN netif,
// and esp_wifi_nan_sync_start() waits for that. Made once and kept, like
// wifi's own netifs.
static esp_netif_t *_netif;

static void queue_event(nan_event_kind_t kind, uint8_t own_id, uint8_t peer_id,
    const uint8_t *mac, const uint8_t *data, uint16_t len) {
    nan_event_t *ev = malloc(sizeof(nan_event_t) + len);
    if (ev == NULL) {
        _dropped++;
        return;
    }
    ev->kind = kind;
    ev->own_id = own_id;
    ev->peer_id = peer_id;
    memcpy(ev->peer_mac, mac, 6);
    ev->len = len;
    memcpy(ev->data, data, len);
    if (xQueueSend(_queue, &ev, 0) != pdTRUE) {
        free(ev);
        _dropped++;
    }
}

static void nan_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    switch (id) {
        case WIFI_EVENT_NAN_SVC_MATCH: {
            const wifi_event_nan_svc_match_t *e = data;
            queue_event(NAN_EV_MATCH, e->subscribe_id, e->publish_id, e->pub_if_mac, e->ssi, e->ssi_len);
            break;
        }
        case WIFI_EVENT_NAN_REPLIED: {
            const wifi_event_nan_replied_t *e = data;
            queue_event(NAN_EV_REPLIED, e->publish_id, e->subscribe_id, e->sub_if_mac, e->ssi, e->ssi_len);
            break;
        }
        case WIFI_EVENT_NAN_RECEIVE: {
            const wifi_event_nan_receive_t *e = data;
            queue_event(NAN_EV_MESSAGE, e->inst_id, e->peer_inst_id, e->peer_if_mac, e->ssi, e->ssi_len);
            break;
        }
        default:
            break;
    }
}

static void drain_queue(void) {
    nan_event_t *ev;
    while (_queue != NULL && xQueueReceive(_queue, &ev, 0) == pdTRUE) {
        free(ev);
    }
}

static void restore_radio(void) {
    esp_wifi_set_mode(_saved_mode);
    if (_radio_was_started) {
        common_hal_wifi_radio_set_enabled(&common_hal_wifi_radio_obj, true);
    }
}

static void aware_stop(espidf_wifi_aware_obj_t *self) {
    if (!self->running) {
        return;
    }
    esp_wifi_nan_sync_stop();
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, _handler);
    drain_queue();
    restore_radio();
    self->running = false;
    if (_aware == self) {
        _aware = NULL;
    }
}

static espidf_wifi_aware_obj_t *get_running(mp_obj_t self_in) {
    espidf_wifi_aware_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->running) {
        raise_deinited_error();
    }
    return self;
}

// A str, at most max - 1 bytes, into a NUL-terminated field.
static void copy_name(char *dest, size_t max, mp_obj_t obj, qstr arg) {
    size_t len;
    const char *s = mp_obj_str_get_data(obj, &len);
    mp_arg_validate_length_max(len, max - 1, arg);
    memcpy(dest, s, len);
    dest[len] = '\0';
}

//| class WiFiAware:
//|     """Wi-Fi Aware (NAN): find services of nearby devices by name and
//|     exchange short messages with them, without an access point. Discovery
//|     and messages only, no datapath. ESP32-C5.
//|
//|     NAN takes the radio: `wifi.radio`'s station (and access point) stop
//|     while this runs, a connection is dropped, and Wi-Fi is put back as it
//|     was by `deinit`. Leave `wifi.radio` alone meanwhile."""
//|
//|     def __init__(self, *, channel: int = 6) -> None:
//|         """Join or start a NAN cluster.
//|
//|         :param int channel: the 2.4 GHz channel of the discovery windows;
//|           6 is the standard one."""
//|         ...
//|
static mp_obj_t espidf_wifi_aware_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_channel };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_channel, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 6 } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    mp_int_t channel = mp_arg_validate_int_range(args[ARG_channel].u_int, 1, 13, MP_QSTR_channel);
    if (_aware != NULL) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("WiFiAware in use"));
    }
    if (_queue == NULL) {
        _queue = xQueueCreate(NAN_QUEUE_DEPTH, sizeof(nan_event_t *));
        if (_queue == NULL) {
            mp_raise_msg(&mp_type_MemoryError, NULL);
        }
    }
    if (_netif == NULL) {
        _netif = esp_netif_create_default_wifi_nan();
        if (_netif == NULL) {
            mp_raise_RuntimeError(MP_ERROR_TEXT("NAN interface could not be made"));
        }
    }

    espidf_wifi_aware_obj_t *self = mp_obj_malloc_with_finaliser(espidf_wifi_aware_obj_t, &espidf_wifi_aware_type);
    self->running = false;

    // What `import wifi` does; without it the IDF's Wi-Fi is not initialised.
    common_hal_wifi_init(true);
    wifi_radio_obj_t *radio = &common_hal_wifi_radio_obj;
    _radio_was_started = common_hal_wifi_radio_get_enabled(radio);
    CHECK_ESP_RESULT(esp_wifi_get_mode(&_saved_mode));
    if (_radio_was_started) {
        common_hal_wifi_radio_set_enabled(radio, false);
    }

    _dropped = 0;
    esp_err_t err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, nan_event_handler, NULL, &_handler);
    if (err == ESP_OK) {
        wifi_nan_sync_config_t config = WIFI_NAN_SYNC_CONFIG_DEFAULT();
        config.op_channel = channel;
        err = esp_wifi_nan_sync_start(&config);
        if (err != ESP_OK) {
            esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, _handler);
        }
    }
    if (err != ESP_OK) {
        restore_radio();
        CHECK_ESP_RESULT(err);
    }
    self->running = true;
    _aware = self;
    return MP_OBJ_FROM_PTR(self);
}

//|     def publish(self, service: str, *, info: ReadableBuffer = b"", solicited: bool = False, filter: str = "") -> int:
//|         """Offer a service. Unsolicited (the default) announces it in every
//|         discovery window; solicited answers only subscribers asking for it.
//|         A subscriber that finds it shows up in `read` as ``"replied"``.
//|
//|         :param str service: the name subscribers look for, up to 255 bytes.
//|         :param info: up to 512 bytes the subscriber gets with the match.
//|         :param str filter: comma-separated matching filter, up to 255 bytes.
//|         :return: the service id, for `send` and `cancel`. Two services at
//|           most, published and subscribed together."""
//|         ...
//|
static mp_obj_t espidf_wifi_aware_publish(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_self, ARG_service, ARG_info, ARG_solicited, ARG_filter };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_self, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_service, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_info, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = mp_const_empty_bytes } },
        { MP_QSTR_solicited, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = false } },
        { MP_QSTR_filter, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = MP_OBJ_NEW_QSTR(MP_QSTR_) } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    get_running(args[ARG_self].u_obj);

    // About half a kilobyte, so off the stack.
    wifi_nan_publish_cfg_t *cfg = m_new0(wifi_nan_publish_cfg_t, 1);
    copy_name(cfg->service_name, sizeof(cfg->service_name), args[ARG_service].u_obj, MP_QSTR_service);
    copy_name(cfg->matching_filter, sizeof(cfg->matching_filter), args[ARG_filter].u_obj, MP_QSTR_filter);
    mp_buffer_info_t info;
    mp_get_buffer_raise(args[ARG_info].u_obj, &info, MP_BUFFER_READ);
    mp_arg_validate_length_max(info.len, ESP_WIFI_MAX_SVC_SSI_LEN, MP_QSTR_info);
    cfg->type = args[ARG_solicited].u_bool ? NAN_PUBLISH_SOLICITED : NAN_PUBLISH_UNSOLICITED;
    cfg->single_replied_event = 1;
    cfg->ssi_len = info.len;
    // The IDF refuses a pointer with a length of zero.
    cfg->ssi = info.len ? info.buf : NULL;
    uint8_t id = esp_wifi_nan_publish_service(cfg);
    m_del(wifi_nan_publish_cfg_t, cfg, 1);
    if (id == 0) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("Service not started"));
    }
    return MP_OBJ_NEW_SMALL_INT(id);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(espidf_wifi_aware_publish_obj, 2, espidf_wifi_aware_publish);

//|     def subscribe(self, service: str, *, active: bool = True, filter: str = "") -> int:
//|         """Look for a service. Active subscribers ask in every discovery
//|         window, passive ones only listen for unsolicited publishers. Each
//|         publisher found shows up in `read` as ``"match"``.
//|
//|         :return: the service id, for `send` and `cancel`."""
//|         ...
//|
static mp_obj_t espidf_wifi_aware_subscribe(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_self, ARG_service, ARG_active, ARG_filter };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_self, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_service, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_active, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = true } },
        { MP_QSTR_filter, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = MP_OBJ_NEW_QSTR(MP_QSTR_) } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    get_running(args[ARG_self].u_obj);

    wifi_nan_subscribe_cfg_t *cfg = m_new0(wifi_nan_subscribe_cfg_t, 1);
    copy_name(cfg->service_name, sizeof(cfg->service_name), args[ARG_service].u_obj, MP_QSTR_service);
    copy_name(cfg->matching_filter, sizeof(cfg->matching_filter), args[ARG_filter].u_obj, MP_QSTR_filter);
    cfg->type = args[ARG_active].u_bool ? NAN_SUBSCRIBE_ACTIVE : NAN_SUBSCRIBE_PASSIVE;
    cfg->single_match_event = 1;
    uint8_t id = esp_wifi_nan_subscribe_service(cfg);
    m_del(wifi_nan_subscribe_cfg_t, cfg, 1);
    if (id == 0) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("Service not started"));
    }
    return MP_OBJ_NEW_SMALL_INT(id);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(espidf_wifi_aware_subscribe_obj, 2, espidf_wifi_aware_subscribe);

//|     def cancel(self, service_id: int) -> None:
//|         """End a published or subscribed service."""
//|         ...
//|
static mp_obj_t espidf_wifi_aware_cancel(mp_obj_t self_in, mp_obj_t id_in) {
    get_running(self_in);
    CHECK_ESP_RESULT(esp_wifi_nan_cancel_service(mp_arg_validate_int_range(mp_obj_get_int(id_in), 1, 255, MP_QSTR_service_id)));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(espidf_wifi_aware_cancel_obj, espidf_wifi_aware_cancel);

//|     def send(self, service_id: int, peer: ReadableBuffer, peer_id: int, data: ReadableBuffer) -> None:
//|         """Send a message (a follow-up) to a peer found through `read`: its
//|         ``peer`` address and ``peer_id``, from our ``service_id``. It
//|         arrives there as ``"message"``. Up to 2048 bytes; not acknowledged,
//|         and sent in the next discovery window, so allow for loss."""
//|         ...
//|
static mp_obj_t espidf_wifi_aware_send(size_t n_args, const mp_obj_t *args) {
    get_running(args[0]);
    wifi_nan_followup_params_t fup = {
        .inst_id = mp_arg_validate_int_range(mp_obj_get_int(args[1]), 1, 255, MP_QSTR_service_id),
        .peer_inst_id = mp_arg_validate_int_range(mp_obj_get_int(args[3]), 1, 255, MP_QSTR_peer_id),
    };
    mp_buffer_info_t peer;
    mp_get_buffer_raise(args[2], &peer, MP_BUFFER_READ);
    mp_arg_validate_length(peer.len, 6, MP_QSTR_peer);
    memcpy(fup.peer_mac, peer.buf, 6);
    mp_buffer_info_t data;
    mp_get_buffer_raise(args[4], &data, MP_BUFFER_READ);
    mp_arg_validate_length_max(data.len, ESP_WIFI_MAX_FUP_SSI_LEN, MP_QSTR_data);
    fup.ssi_len = data.len;
    fup.ssi = data.len ? data.buf : NULL;
    CHECK_ESP_RESULT(esp_wifi_nan_send_message(&fup));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(espidf_wifi_aware_send_obj, 5, 5, espidf_wifi_aware_send);

//|     def read(self) -> Optional[Tuple[str, int, bytes, int, bytes]]:
//|         """The oldest event not yet read, or None: ``(kind, service_id,
//|         peer, peer_id, data)``. ``kind`` is ``"match"`` (our subscription
//|         found a publisher; ``data`` is its ``info``), ``"replied"`` (a
//|         subscriber found our publication) or ``"message"`` (a peer `send`
//|         to us). ``peer`` is its 6-byte NAN address. Sixteen are kept;
//|         later ones are counted in `dropped`."""
//|         ...
//|
static mp_obj_t espidf_wifi_aware_read(mp_obj_t self_in) {
    get_running(self_in);
    nan_event_t *ev;
    if (xQueueReceive(_queue, &ev, 0) != pdTRUE) {
        return mp_const_none;
    }
    static const qstr kinds[] = { MP_QSTR_match, MP_QSTR_replied, MP_QSTR_message };
    mp_obj_t items[5] = {
        MP_OBJ_NEW_QSTR(kinds[ev->kind]),
        MP_OBJ_NEW_SMALL_INT(ev->own_id),
        mp_const_none,
        MP_OBJ_NEW_SMALL_INT(ev->peer_id),
        mp_const_none,
    };
    // Freed even if making the bytes objects raises.
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        items[2] = mp_obj_new_bytes(ev->peer_mac, 6);
        items[4] = mp_obj_new_bytes(ev->data, ev->len);
        nlr_pop();
    } else {
        free(ev);
        nlr_jump(nlr.ret_val);
    }
    free(ev);
    return mp_obj_new_tuple(5, items);
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_wifi_aware_read_obj, espidf_wifi_aware_read);

//|     address: bytes
//|     """Our NAN interface address, what peers see as ``peer``."""
//|
//|     dropped: int
//|     """Events lost because `read` did not keep up."""
//|
static mp_obj_t espidf_wifi_aware_get_address(mp_obj_t self_in) {
    get_running(self_in);
    uint8_t mac[6];
    CHECK_ESP_RESULT(esp_wifi_get_mac(WIFI_IF_NAN, mac));
    return mp_obj_new_bytes(mac, 6);
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_wifi_aware_get_address_obj, espidf_wifi_aware_get_address);
MP_PROPERTY_GETTER(espidf_wifi_aware_address_obj, (mp_obj_t)&espidf_wifi_aware_get_address_obj);

static mp_obj_t espidf_wifi_aware_get_dropped(mp_obj_t self_in) {
    return mp_obj_new_int_from_uint(_dropped);
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_wifi_aware_get_dropped_obj, espidf_wifi_aware_get_dropped);
MP_PROPERTY_GETTER(espidf_wifi_aware_dropped_obj, (mp_obj_t)&espidf_wifi_aware_get_dropped_obj);

//|     def deinit(self) -> None:
//|         """Leave the cluster, end all services, and give Wi-Fi back."""
//|         ...
//|
static mp_obj_t espidf_wifi_aware_deinit(mp_obj_t self_in) {
    aware_stop(MP_OBJ_TO_PTR(self_in));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_wifi_aware_deinit_obj, espidf_wifi_aware_deinit);

static mp_obj_t espidf_wifi_aware___exit__(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    return espidf_wifi_aware_deinit(args[0]);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(espidf_wifi_aware___exit___obj, 4, 4, espidf_wifi_aware___exit__);

static const mp_rom_map_elem_t espidf_wifi_aware_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_publish), MP_ROM_PTR(&espidf_wifi_aware_publish_obj) },
    { MP_ROM_QSTR(MP_QSTR_subscribe), MP_ROM_PTR(&espidf_wifi_aware_subscribe_obj) },
    { MP_ROM_QSTR(MP_QSTR_cancel), MP_ROM_PTR(&espidf_wifi_aware_cancel_obj) },
    { MP_ROM_QSTR(MP_QSTR_send), MP_ROM_PTR(&espidf_wifi_aware_send_obj) },
    { MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&espidf_wifi_aware_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_address), MP_ROM_PTR(&espidf_wifi_aware_address_obj) },
    { MP_ROM_QSTR(MP_QSTR_dropped), MP_ROM_PTR(&espidf_wifi_aware_dropped_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&espidf_wifi_aware_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&espidf_wifi_aware_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&espidf_wifi_aware___exit___obj) },
};
static MP_DEFINE_CONST_DICT(espidf_wifi_aware_locals_dict, espidf_wifi_aware_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    espidf_wifi_aware_type,
    MP_QSTR_WiFiAware,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, espidf_wifi_aware_make_new,
    locals_dict, &espidf_wifi_aware_locals_dict
    );

void espidf_nan_reset(void) {
    if (_aware != NULL) {
        aware_stop(_aware);
    }
}

#endif
