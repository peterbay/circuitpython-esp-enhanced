// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Petr Vavrin
//
// SPDX-License-Identifier: MIT

// espidf.EventQueue; see extended.h.

#include <string.h>

#include "py/mpstate.h"
#include "py/obj.h"
#include "py/objlist.h"
#include "py/objproperty.h"
#include "py/runtime.h"

#include "bindings/espidf/__init__.h"
#include "bindings/espidf/extended.h"
#include "supervisor/port.h"

#include "esp_event.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#if CIRCUITPY_WIFI
#include "esp_netif.h"
#include "esp_smartconfig.h"
#include "esp_wifi.h"
#endif

// ESP-IDF events, readable from Python.
//
// Handlers run in the event loop task, alongside the Wi-Fi stack, not on the
// Python thread. Nothing there may touch the interpreter or block, so the handler only
// copies the event into a FreeRTOS queue and wakes the main task; Python drains
// it whenever it gets round to it. That is the same shape as the Wi-Fi handler in
// common-hal/wifi/__init__.c and as keypad.EventQueue.
//
// The handler's argument is the static slot below, never the Python object, so a
// stale registration cannot write into the GC heap. The object is traced as well,
// for the ordinary reason that the ESP-IDF holds a pointer into it.

#define ESPIDF_EVENT_MAX_DATA (48)
#define ESPIDF_MAX_EVENT_QUEUES (4)

typedef struct {
    esp_event_base_t base;
    int32_t id;
    int64_t time_us;
    // The true length before truncation, so a caller can tell that it happened.
    uint16_t data_len;
    uint8_t data[ESPIDF_EVENT_MAX_DATA];
} espidf_event_record_t;

// Kept outside the heap, like the timer handles: soft reset has to unregister
// these, and by then the traced list may already be gone.
typedef struct {
    esp_event_handler_instance_t instance;
    esp_event_base_t base;
    int32_t id;
    QueueHandle_t queue;
    volatile bool overflowed;
    bool in_use;
} espidf_event_reg_t;

static espidf_event_reg_t espidf_live_events[ESPIDF_MAX_EVENT_QUEUES];

typedef struct {
    mp_obj_base_t base;
    espidf_event_reg_t *reg;
} espidf_event_queue_obj_t;

MP_REGISTER_ROOT_POINTER(mp_obj_t espidf_active_event_queues);

// esp_event does not pass the handler the length of event_data, and the buffer it
// hands over is calloc'd to exactly event_data_size (esp_event.c, esp_event_post_to),
// so copying a fixed amount reads off the end of a heap allocation. The size is a
// property of the (base, id) pair, so it has to be looked up. sizeof does the
// arithmetic, and anything not listed yields no payload rather than a guess.
typedef struct {
    const esp_event_base_t *base;
    int32_t id;
    uint16_t size;
} espidf_event_size_t;

static const espidf_event_size_t espidf_event_sizes[] = {
    #if CIRCUITPY_WIFI
    { &WIFI_EVENT, WIFI_EVENT_SCAN_DONE, sizeof(wifi_event_sta_scan_done_t) },
    { &WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, sizeof(wifi_event_sta_connected_t) },
    { &WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, sizeof(wifi_event_sta_disconnected_t) },
    { &WIFI_EVENT, WIFI_EVENT_STA_AUTHMODE_CHANGE, sizeof(wifi_event_sta_authmode_change_t) },
    { &WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, sizeof(wifi_event_ap_staconnected_t) },
    { &WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, sizeof(wifi_event_ap_stadisconnected_t) },
    { &WIFI_EVENT, WIFI_EVENT_AP_PROBEREQRECVED, sizeof(wifi_event_ap_probe_req_rx_t) },
    { &WIFI_EVENT, WIFI_EVENT_STA_BSS_RSSI_LOW, sizeof(wifi_event_bss_rssi_low_t) },
    { &WIFI_EVENT, WIFI_EVENT_HOME_CHANNEL_CHANGE, sizeof(wifi_event_home_channel_change_t) },
    { &IP_EVENT, IP_EVENT_STA_GOT_IP, sizeof(ip_event_got_ip_t) },
    { &IP_EVENT, IP_EVENT_ETH_GOT_IP, sizeof(ip_event_got_ip_t) },
    { &IP_EVENT, IP_EVENT_PPP_GOT_IP, sizeof(ip_event_got_ip_t) },
    { &IP_EVENT, IP_EVENT_GOT_IP6, sizeof(ip_event_got_ip6_t) },
    { &IP_EVENT, IP_EVENT_AP_STAIPASSIGNED, sizeof(ip_event_ap_staipassigned_t) },
    #endif
};

static uint16_t espidf_event_data_size(esp_event_base_t base, int32_t id) {
    for (size_t i = 0; i < MP_ARRAY_SIZE(espidf_event_sizes); i++) {
        if (base == *espidf_event_sizes[i].base && id == espidf_event_sizes[i].id) {
            uint16_t size = espidf_event_sizes[i].size;
            return size > ESPIDF_EVENT_MAX_DATA ? ESPIDF_EVENT_MAX_DATA : size;
        }
    }
    return 0;
}

// Runs in the event loop task. No allocation, no blocking, no interpreter.
static void espidf_event_handler(void *arg, esp_event_base_t base,
    int32_t id, void *event_data) {
    espidf_event_reg_t *reg = arg;
    if (reg->queue == NULL) {
        return;
    }
    espidf_event_record_t record;
    record.base = base;
    record.id = id;
    record.time_us = esp_timer_get_time();
    record.data_len = event_data == NULL ? 0 : espidf_event_data_size(base, id);
    memset(record.data, 0, sizeof(record.data));
    if (record.data_len != 0) {
        memcpy(record.data, event_data, record.data_len);
    }
    if (xQueueSendToBack(reg->queue, &record, 0) != pdTRUE) {
        reg->overflowed = true;
    }
    port_wake_main_task();
}

static const struct {
    const char *name;
    const esp_event_base_t *base;
} espidf_event_bases[] = {
    #if CIRCUITPY_WIFI
    { "WIFI_EVENT", &WIFI_EVENT },
    { "IP_EVENT", &IP_EVENT },
    { "SC_EVENT", &SC_EVENT },
    #endif
};

static esp_event_base_t espidf_event_base_from_name(mp_obj_t name_in) {
    const char *name = mp_obj_str_get_str(name_in);
    for (size_t i = 0; i < MP_ARRAY_SIZE(espidf_event_bases); i++) {
        if (strcmp(name, espidf_event_bases[i].name) == 0) {
            return *espidf_event_bases[i].base;
        }
    }
    mp_raise_ValueError_varg(MP_ERROR_TEXT("%q must be a known event base"), MP_QSTR_base);
}

static espidf_event_reg_t *espidf_event_queue_reg(espidf_event_queue_obj_t *self) {
    if (self->reg == NULL) {
        mp_raise_ValueError(MP_ERROR_TEXT("EventQueue is deinitialized"));
    }
    return self->reg;
}

// Unregister before deleting the queue, never the other way round: between the
// two the handler would still be live and would write into freed memory.
static void espidf_event_release(espidf_event_reg_t *reg) {
    if (!reg->in_use) {
        return;
    }
    esp_event_handler_instance_unregister(reg->base, reg->id, reg->instance);
    reg->instance = NULL;
    QueueHandle_t queue = reg->queue;
    reg->queue = NULL;
    if (queue != NULL) {
        vQueueDelete(queue);
    }
    reg->in_use = false;
    reg->overflowed = false;
}

// Called on soft reset. Only the slots outside the heap are touched, because the
// objects they belong to may already be gone.
void espidf_event_reset(void) {
    for (size_t i = 0; i < ESPIDF_MAX_EVENT_QUEUES; i++) {
        espidf_event_release(&espidf_live_events[i]);
    }
    MP_STATE_VM(espidf_active_event_queues) = MP_OBJ_NULL;
}

//| class EventQueue:
//|     """Buffers ESP-IDF events so they can be read from Python.
//|
//|     The event loop calls its handlers in its own task, where the interpreter
//|     cannot be touched, so events are copied into a fixed size queue instead and
//|     read here. If the queue fills before it is drained, further events are
//|     dropped and `overflowed` becomes True.
//|
//|     Known bases are ``"WIFI_EVENT"``, ``"IP_EVENT"`` and ``"SC_EVENT"``
//|     (SmartConfig); they are also available as `espidf.WIFI_EVENT`,
//|     `espidf.IP_EVENT` and `espidf.SC_EVENT`. At most four queues at a time."""
//|
//|     def __init__(self, base: str, event_id: int = ANY_ID, *, size: int = 16) -> None:
//|         """Start listening. ``event_id`` defaults to every event of that base."""
//|         ...
static mp_obj_t espidf_event_queue_make_new(const mp_obj_type_t *type, size_t n_args,
    size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_base, ARG_event_id, ARG_size };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_base, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_event_id, MP_ARG_INT, { .u_int = ESP_EVENT_ANY_ID } },
        { MP_QSTR_size, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 16 } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    esp_event_base_t base = espidf_event_base_from_name(args[ARG_base].u_obj);
    mp_int_t size = mp_arg_validate_int_range(args[ARG_size].u_int, 1, 64, MP_QSTR_size);

    // The default loop is normally created by wifi.radio, but an EventQueue may
    // well be the first thing to want it. Creating it twice is ESP_ERR_INVALID_STATE,
    // which is not an error here.
    esp_err_t loop_err = esp_event_loop_create_default();
    if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) {
        CHECK_ESP_RESULT(loop_err);
    }

    espidf_event_reg_t *reg = NULL;
    for (size_t i = 0; i < ESPIDF_MAX_EVENT_QUEUES; i++) {
        if (!espidf_live_events[i].in_use) {
            reg = &espidf_live_events[i];
            break;
        }
    }
    if (reg == NULL) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("too many event queues"));
    }

    QueueHandle_t queue = xQueueCreate(size, sizeof(espidf_event_record_t));
    if (queue == NULL) {
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("could not allocate event queue"));
    }

    reg->base = base;
    reg->id = args[ARG_event_id].u_int;
    reg->queue = queue;
    reg->overflowed = false;
    reg->in_use = true;
    esp_err_t err = esp_event_handler_instance_register(base, reg->id,
        &espidf_event_handler, reg, &reg->instance);
    if (err != ESP_OK) {
        reg->queue = NULL;
        reg->in_use = false;
        vQueueDelete(queue);
        CHECK_ESP_RESULT(err);
    }

    espidf_event_queue_obj_t *self = mp_obj_malloc(espidf_event_queue_obj_t, &espidf_event_queue_type);
    self->reg = reg;
    if (MP_STATE_VM(espidf_active_event_queues) == MP_OBJ_NULL) {
        MP_STATE_VM(espidf_active_event_queues) = mp_obj_new_list(0, NULL);
    }
    mp_obj_list_append(MP_STATE_VM(espidf_active_event_queues), MP_OBJ_FROM_PTR(self));
    return MP_OBJ_FROM_PTR(self);
}

//|     def get(self) -> Optional[Tuple[str, int, int, bytes]]:
//|         """The oldest event as ``(base, event_id, time_us, data)``, or None if
//|         there is none. ``time_us`` comes from the same clock as
//|         `espidf.get_time_us`. ``data`` is the raw event structure for events
//|         whose layout this module knows, and empty otherwise — esp_event does not
//|         tell a handler how long the payload is, so the length has to come from a
//|         table rather than a guess. Decoding it is the caller's job."""
//|         ...
static mp_obj_t espidf_event_queue_get(mp_obj_t self_in) {
    espidf_event_reg_t *reg = espidf_event_queue_reg(MP_OBJ_TO_PTR(self_in));
    espidf_event_record_t record;
    if (xQueueReceive(reg->queue, &record, 0) != pdTRUE) {
        return mp_const_none;
    }
    mp_obj_t items[4] = {
        mp_obj_new_str(record.base, strlen(record.base)),
        mp_obj_new_int(record.id),
        mp_obj_new_int_from_ll(record.time_us),
        mp_obj_new_bytes(record.data, record.data_len),
    };
    return mp_obj_new_tuple(4, items);
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_event_queue_get_obj, espidf_event_queue_get);

//|     overflowed: bool
//|     """True once an event has been dropped because the queue was full. Stays set
//|     until `clear`."""
static mp_obj_t espidf_event_queue_get_overflowed(mp_obj_t self_in) {
    return mp_obj_new_bool(espidf_event_queue_reg(MP_OBJ_TO_PTR(self_in))->overflowed);
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_event_queue_get_overflowed_obj,
    espidf_event_queue_get_overflowed);
MP_PROPERTY_GETTER(espidf_event_queue_overflowed_obj,
    (mp_obj_t)&espidf_event_queue_get_overflowed_obj);

//|     def clear(self) -> None:
//|         """Drop everything queued and reset `overflowed`."""
//|         ...
static mp_obj_t espidf_event_queue_clear(mp_obj_t self_in) {
    espidf_event_reg_t *reg = espidf_event_queue_reg(MP_OBJ_TO_PTR(self_in));
    xQueueReset(reg->queue);
    reg->overflowed = false;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_event_queue_clear_obj, espidf_event_queue_clear);

//|     def deinit(self) -> None:
//|         """Stop listening and release the queue."""
//|         ...
static mp_obj_t espidf_event_queue_deinit(mp_obj_t self_in) {
    espidf_event_queue_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->reg != NULL) {
        espidf_event_release(self->reg);
        self->reg = NULL;
    }
    mp_obj_t list_obj = MP_STATE_VM(espidf_active_event_queues);
    if (list_obj != MP_OBJ_NULL) {
        size_t len;
        mp_obj_t *items;
        mp_obj_list_get(list_obj, &len, &items);
        for (size_t i = 0; i < len; i++) {
            if (items[i] == self_in) {
                mp_obj_list_remove(list_obj, items[i]);
                break;
            }
        }
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_event_queue_deinit_obj, espidf_event_queue_deinit);

//|     def __len__(self) -> int:
//|         """The number of events waiting."""
//|         ...
static mp_obj_t espidf_event_queue_unary_op(mp_unary_op_t op, mp_obj_t self_in) {
    espidf_event_reg_t *reg = espidf_event_queue_reg(MP_OBJ_TO_PTR(self_in));
    uint32_t waiting = uxQueueMessagesWaiting(reg->queue);
    switch (op) {
        case MP_UNARY_OP_BOOL:
            return mp_obj_new_bool(waiting != 0);
        case MP_UNARY_OP_LEN:
            return MP_OBJ_NEW_SMALL_INT(waiting);
        default:
            return MP_OBJ_NULL;
    }
}

static const mp_rom_map_elem_t espidf_event_queue_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_get), MP_ROM_PTR(&espidf_event_queue_get_obj) },
    { MP_ROM_QSTR(MP_QSTR_clear), MP_ROM_PTR(&espidf_event_queue_clear_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&espidf_event_queue_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR_overflowed), MP_ROM_PTR(&espidf_event_queue_overflowed_obj) },
};
static MP_DEFINE_CONST_DICT(espidf_event_queue_locals_dict, espidf_event_queue_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    espidf_event_queue_type,
    MP_QSTR_EventQueue,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, espidf_event_queue_make_new,
    unary_op, espidf_event_queue_unary_op,
    locals_dict, &espidf_event_queue_locals_dict
    );
