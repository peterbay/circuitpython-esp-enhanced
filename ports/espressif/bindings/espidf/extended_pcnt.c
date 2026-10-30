// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Petr Vavrin
//
// SPDX-License-Identifier: MIT

// espidf.PulseCounter; see extended.h.

#include <limits.h>

#include "py/obj.h"
#include "py/objproperty.h"
#include "py/runtime.h"

#include "bindings/espidf/__init__.h"
#include "bindings/espidf/extended.h"
#include "common-hal/microcontroller/Pin.h"
#include "shared-bindings/microcontroller/Pin.h"
#include "shared-bindings/util.h"
#include "shared/runtime/context_manager_helpers.h"

// The headers below exist only where the switch brings them in.
#if CIRCUITPY_ESPIDF_PCNT

#include "driver/pulse_cnt.h"

// The pulse counter as such: up to two channels per unit, each counting the
// edges of one pin up, down or not at all, and a second pin's level keeping,
// reversing or holding that -- the building blocks of direction counting and
// quadrature decoding, which countio and rotaryio use in two fixed ways. Adds
// the glitch filter and watch points, whose crossings are queued as events.

#define PCNT_EVENTS (16)
#define PCNT_OPEN_MAX (8)

typedef struct {
    mp_obj_base_t base;
    pcnt_unit_handle_t unit;
    pcnt_channel_handle_t chans[2];
    uint8_t pins[4];
    uint8_t npins;
    int limit;
    volatile int events[PCNT_EVENTS];
    volatile uint8_t ev_head;
    volatile uint8_t ev_tail;
    volatile uint32_t ev_lost;
    // Watch points the program asked for, as against the limits used to
    // accumulate past the 16-bit counter.
    int watch[2];
    uint8_t nwatch;
} espidf_pulse_counter_obj_t;

static espidf_pulse_counter_obj_t *_open[PCNT_OPEN_MAX];

static bool on_reach(pcnt_unit_handle_t unit, const pcnt_watch_event_data_t *edata, void *user_ctx) {
    espidf_pulse_counter_obj_t *self = user_ctx;
    int value = edata->watch_point_value;
    if (value == self->limit || value == -self->limit) {
        return false;
    }
    uint8_t next = (self->ev_head + 1) % PCNT_EVENTS;
    if (next == self->ev_tail) {
        self->ev_lost++;
    } else {
        self->events[self->ev_head] = value;
        self->ev_head = next;
    }
    return false;
}

static void counter_close(espidf_pulse_counter_obj_t *self) {
    if (self->unit == NULL) {
        return;
    }
    pcnt_unit_stop(self->unit);
    pcnt_unit_disable(self->unit);
    for (int i = 0; i < 2; i++) {
        if (self->chans[i] != NULL) {
            pcnt_del_channel(self->chans[i]);
            self->chans[i] = NULL;
        }
    }
    pcnt_del_unit(self->unit);
    self->unit = NULL;
    for (int i = 0; i < self->npins; i++) {
        reset_pin_number(self->pins[i]);
    }
    self->npins = 0;
    for (size_t i = 0; i < PCNT_OPEN_MAX; i++) {
        if (_open[i] == self) {
            _open[i] = NULL;
        }
    }
}

void espidf_pcnt_reset(void) {
    for (size_t i = 0; i < PCNT_OPEN_MAX; i++) {
        if (_open[i] != NULL) {
            counter_close(_open[i]);
        }
    }
}

static espidf_pulse_counter_obj_t *get_open(mp_obj_t self_in) {
    espidf_pulse_counter_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->unit == NULL) {
        raise_deinited_error();
    }
    return self;
}

static pcnt_channel_edge_action_t edge_action(mp_obj_t obj, qstr arg) {
    qstr q = mp_obj_str_get_qstr(obj);
    if (q == MP_QSTR_up) {
        return PCNT_CHANNEL_EDGE_ACTION_INCREASE;
    } else if (q == MP_QSTR_down) {
        return PCNT_CHANNEL_EDGE_ACTION_DECREASE;
    } else if (q == MP_QSTR_none) {
        return PCNT_CHANNEL_EDGE_ACTION_HOLD;
    }
    mp_arg_error_invalid(arg);
}

static pcnt_channel_level_action_t level_action(mp_obj_t obj, qstr arg) {
    qstr q = mp_obj_str_get_qstr(obj);
    if (q == MP_QSTR_keep) {
        return PCNT_CHANNEL_LEVEL_ACTION_KEEP;
    } else if (q == MP_QSTR_reverse) {
        return PCNT_CHANNEL_LEVEL_ACTION_INVERSE;
    } else if (q == MP_QSTR_hold) {
        return PCNT_CHANNEL_LEVEL_ACTION_HOLD;
    }
    mp_arg_error_invalid(arg);
}

// A pin another object drives is listened to and left to it.
static int listen_pin(espidf_pulse_counter_obj_t *self, mp_obj_t obj, qstr arg) {
    if (obj == mp_const_none) {
        return -1;
    }
    const mcu_pin_obj_t *pin = validate_obj_is_pin(obj, arg);
    if (common_hal_mcu_pin_is_free(pin)) {
        claim_pin(pin);
        self->pins[self->npins++] = pin->number;
    }
    return pin->number;
}

enum { ARG_pin, ARG_level, ARG_rising, ARG_falling, ARG_high, ARG_low };
#define CHANNEL_ARGS \
    { MP_QSTR_pin, MP_ARG_REQUIRED | MP_ARG_OBJ }, \
    { MP_QSTR_level, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = mp_const_none } }, \
    { MP_QSTR_rising, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = MP_OBJ_NEW_QSTR(MP_QSTR_up) } }, \
    { MP_QSTR_falling, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = MP_OBJ_NEW_QSTR(MP_QSTR_none) } }, \
    { MP_QSTR_high, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = MP_OBJ_NEW_QSTR(MP_QSTR_keep) } }, \
    { MP_QSTR_low, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = MP_OBJ_NEW_QSTR(MP_QSTR_reverse) } }

static void add_channel(espidf_pulse_counter_obj_t *self, const mp_arg_val_t *args) {
    int slot = self->chans[0] == NULL ? 0 : 1;
    if (self->chans[slot] != NULL) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("Two channels at most"));
    }
    pcnt_channel_edge_action_t rising = edge_action(args[ARG_rising].u_obj, MP_QSTR_rising);
    pcnt_channel_edge_action_t falling = edge_action(args[ARG_falling].u_obj, MP_QSTR_falling);
    pcnt_channel_level_action_t high = level_action(args[ARG_high].u_obj, MP_QSTR_high);
    pcnt_channel_level_action_t low = level_action(args[ARG_low].u_obj, MP_QSTR_low);
    uint8_t npins = self->npins;
    pcnt_chan_config_t config = {
        .edge_gpio_num = listen_pin(self, args[ARG_pin].u_obj, MP_QSTR_pin),
        .level_gpio_num = listen_pin(self, args[ARG_level].u_obj, MP_QSTR_level),
    };
    esp_err_t err = pcnt_new_channel(self->unit, &config, &self->chans[slot]);
    if (err == ESP_OK) {
        err = pcnt_channel_set_edge_action(self->chans[slot], rising, falling);
    }
    if (err == ESP_OK && config.level_gpio_num >= 0) {
        err = pcnt_channel_set_level_action(self->chans[slot], high, low);
    }
    if (err != ESP_OK) {
        if (self->chans[slot] != NULL) {
            pcnt_del_channel(self->chans[slot]);
            self->chans[slot] = NULL;
        }
        while (self->npins > npins) {
            reset_pin_number(self->pins[--self->npins]);
        }
        CHECK_ESP_RESULT(err);
    }
}

//| class PulseCounter:
//|     """The PCNT peripheral: counts edges in hardware, with up to two
//|     channels. Each channel counts the edges of ``pin`` -- ``"up"``,
//|     ``"down"`` or ``"none"`` per edge -- while the level of an optional
//|     ``level`` pin ``"keep"``\\ s, ``"reverse"``\\ s or ``"hold"``\\ s that.
//|     Pins another object drives are listened to and left to it.
//|
//|     Direction counting: ``PulseCounter(step, level=direction)``. Full
//|     quadrature (4 counts per cycle)::
//|
//|         pc = PulseCounter(a, level=b, rising="down", falling="up")
//|         pc.add_channel(b, level=a, rising="up", falling="down")"""
//|
//|     def __init__(self, pin: microcontroller.Pin, *, level: Optional[microcontroller.Pin] = None, rising: str = "up", falling: str = "none", high: str = "keep", low: str = "reverse", filter_ns: int = 0, watch: Sequence[int] = ()) -> None:
//|         """
//|         :param int filter_ns: pulses shorter than this are ignored, up to
//|           about 12700 ns.
//|         :param watch: up to two counts, -32766 to 32766 and not 0, whose
//|           crossing `read` reports; 0 is always watched. They compare the
//|           hardware's 16-bit counter, which `count` accumulates past."""
//|         ...
//|
static mp_obj_t espidf_pulse_counter_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_filter_ns = ARG_low + 1, ARG_watch };
    static const mp_arg_t allowed_args[] = {
        CHANNEL_ARGS,
        { MP_QSTR_filter_ns, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_watch, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = mp_const_empty_tuple } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    mp_int_t filter_ns = mp_arg_validate_int_range(args[ARG_filter_ns].u_int, 0, 12787, MP_QSTR_filter_ns);
    size_t nwatch;
    mp_obj_t *watch;
    mp_obj_get_array(args[ARG_watch].u_obj, &nwatch, &watch);
    mp_arg_validate_length_max(nwatch, 2, MP_QSTR_watch);

    size_t slot = 0;
    while (slot < PCNT_OPEN_MAX && _open[slot] != NULL) {
        slot++;
    }
    if (slot == PCNT_OPEN_MAX) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("All pulse counter units in use"));
    }

    espidf_pulse_counter_obj_t *self = mp_obj_malloc_with_finaliser(espidf_pulse_counter_obj_t, &espidf_pulse_counter_type);
    self->unit = NULL;
    self->chans[0] = self->chans[1] = NULL;
    self->npins = 0;
    // The hardware counter is 16-bit on both chips.
    self->limit = SHRT_MAX;
    self->ev_head = self->ev_tail = 0;
    self->ev_lost = 0;
    self->nwatch = nwatch;
    for (size_t i = 0; i < nwatch; i++) {
        self->watch[i] = mp_obj_get_int(watch[i]);
        if (self->watch[i] == 0 || self->watch[i] <= -self->limit || self->watch[i] >= self->limit) {
            mp_arg_error_invalid(MP_QSTR_watch);
        }
    }

    pcnt_unit_config_t config = {
        .low_limit = -self->limit,
        .high_limit = self->limit,
        .flags.accum_count = 1,
    };
    esp_err_t err = pcnt_new_unit(&config, &self->unit);
    if (err == ESP_ERR_NOT_FOUND) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("All pulse counter units in use"));
    }
    CHECK_ESP_RESULT(err);
    _open[slot] = self;

    // Undone by close if any step raises.
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        if (filter_ns > 0) {
            pcnt_glitch_filter_config_t filter = { .max_glitch_ns = filter_ns };
            CHECK_ESP_RESULT(pcnt_unit_set_glitch_filter(self->unit, &filter));
        }
        // The limits are what accumulating needs; 0 and the program's own
        // points are what it reads.
        CHECK_ESP_RESULT(pcnt_unit_add_watch_point(self->unit, self->limit));
        CHECK_ESP_RESULT(pcnt_unit_add_watch_point(self->unit, -self->limit));
        CHECK_ESP_RESULT(pcnt_unit_add_watch_point(self->unit, 0));
        for (size_t i = 0; i < nwatch; i++) {
            CHECK_ESP_RESULT(pcnt_unit_add_watch_point(self->unit, self->watch[i]));
        }
        pcnt_event_callbacks_t cbs = { .on_reach = on_reach };
        CHECK_ESP_RESULT(pcnt_unit_register_event_callbacks(self->unit, &cbs, self));
        add_channel(self, args);
        CHECK_ESP_RESULT(pcnt_unit_enable(self->unit));
        CHECK_ESP_RESULT(pcnt_unit_clear_count(self->unit));
        CHECK_ESP_RESULT(pcnt_unit_start(self->unit));
        nlr_pop();
    } else {
        counter_close(self);
        nlr_jump(nlr.ret_val);
    }
    return MP_OBJ_FROM_PTR(self);
}

//|     def add_channel(self, pin: microcontroller.Pin, *, level: Optional[microcontroller.Pin] = None, rising: str = "up", falling: str = "none", high: str = "keep", low: str = "reverse") -> None:
//|         """Add the unit's second channel, counted into the same total."""
//|         ...
//|
static mp_obj_t espidf_pulse_counter_add_channel(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    static const mp_arg_t allowed_args[] = { CHANNEL_ARGS };
    espidf_pulse_counter_obj_t *self = get_open(pos_args[0]);
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    // A channel is added to a stopped unit.
    pcnt_unit_stop(self->unit);
    pcnt_unit_disable(self->unit);
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        add_channel(self, args);
        nlr_pop();
    } else {
        pcnt_unit_enable(self->unit);
        pcnt_unit_start(self->unit);
        nlr_jump(nlr.ret_val);
    }
    CHECK_ESP_RESULT(pcnt_unit_enable(self->unit));
    CHECK_ESP_RESULT(pcnt_unit_start(self->unit));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(espidf_pulse_counter_add_channel_obj, 2, espidf_pulse_counter_add_channel);

//|     count: int
//|     """The count, accumulated past the 16-bit hardware counter."""
//|
static mp_obj_t espidf_pulse_counter_get_count(mp_obj_t self_in) {
    int value;
    CHECK_ESP_RESULT(pcnt_unit_get_count(get_open(self_in)->unit, &value));
    return mp_obj_new_int(value);
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_pulse_counter_get_count_obj, espidf_pulse_counter_get_count);
MP_PROPERTY_GETTER(espidf_pulse_counter_count_obj, (mp_obj_t)&espidf_pulse_counter_get_count_obj);

//|     def clear(self) -> None:
//|         """Set the count to 0."""
//|         ...
//|
static mp_obj_t espidf_pulse_counter_clear(mp_obj_t self_in) {
    CHECK_ESP_RESULT(pcnt_unit_clear_count(get_open(self_in)->unit));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_pulse_counter_clear_obj, espidf_pulse_counter_clear);

//|     def read(self) -> List[int]:
//|         """Watch points the counter has reached since the last read, in
//|         order: 0 and those given as ``watch``."""
//|         ...
//|
static mp_obj_t espidf_pulse_counter_read(mp_obj_t self_in) {
    espidf_pulse_counter_obj_t *self = get_open(self_in);
    mp_obj_t list = mp_obj_new_list(0, NULL);
    while (self->ev_tail != self->ev_head) {
        mp_obj_list_append(list, MP_OBJ_NEW_SMALL_INT(self->events[self->ev_tail]));
        self->ev_tail = (self->ev_tail + 1) % PCNT_EVENTS;
    }
    return list;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_pulse_counter_read_obj, espidf_pulse_counter_read);

//|     lost: int
//|     """Watch point events dropped because `read` did not keep up."""
//|
static mp_obj_t espidf_pulse_counter_get_lost(mp_obj_t self_in) {
    return mp_obj_new_int_from_uint(get_open(self_in)->ev_lost);
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_pulse_counter_get_lost_obj, espidf_pulse_counter_get_lost);
MP_PROPERTY_GETTER(espidf_pulse_counter_lost_obj, (mp_obj_t)&espidf_pulse_counter_get_lost_obj);

//|     def deinit(self) -> None:
//|         """Release the unit, and the pins this object took."""
//|         ...
//|
static mp_obj_t espidf_pulse_counter_deinit(mp_obj_t self_in) {
    counter_close(MP_OBJ_TO_PTR(self_in));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_pulse_counter_deinit_obj, espidf_pulse_counter_deinit);

static mp_obj_t espidf_pulse_counter___exit__(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    return espidf_pulse_counter_deinit(args[0]);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(espidf_pulse_counter___exit___obj, 4, 4, espidf_pulse_counter___exit__);

static const mp_rom_map_elem_t espidf_pulse_counter_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_add_channel), MP_ROM_PTR(&espidf_pulse_counter_add_channel_obj) },
    { MP_ROM_QSTR(MP_QSTR_count), MP_ROM_PTR(&espidf_pulse_counter_count_obj) },
    { MP_ROM_QSTR(MP_QSTR_clear), MP_ROM_PTR(&espidf_pulse_counter_clear_obj) },
    { MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&espidf_pulse_counter_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_lost), MP_ROM_PTR(&espidf_pulse_counter_lost_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&espidf_pulse_counter_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&espidf_pulse_counter_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&espidf_pulse_counter___exit___obj) },
};
static MP_DEFINE_CONST_DICT(espidf_pulse_counter_locals_dict, espidf_pulse_counter_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    espidf_pulse_counter_type,
    MP_QSTR_PulseCounter,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, espidf_pulse_counter_make_new,
    locals_dict, &espidf_pulse_counter_locals_dict
    );

#endif
