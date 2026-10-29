// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Petr Vavrin
//
// SPDX-License-Identifier: MIT

// espidf.PulseCapture; see extended.h.

#include "py/obj.h"
#include "py/objproperty.h"
#include "py/runtime.h"

#include "bindings/espidf/__init__.h"
#include "bindings/espidf/extended.h"
#include "common-hal/microcontroller/Pin.h"
#include "shared-bindings/microcontroller/Pin.h"
#include "shared-bindings/util.h"
#include "shared/runtime/context_manager_helpers.h"

#include "driver/mcpwm_cap.h"
#include "hal/mcpwm_ll.h"

// MCPWM capture: a free-running timer whose count the hardware latches on
// each edge of an input, so edge times come with the timer's resolution
// (12.5 ns at 80 MHz) whatever the interrupt latency. One capture timer per
// MCPWM group, shared by its three channels.

#if CIRCUITPY_ESPIDF_MCPWM

#define CAP_GROUPS (MCPWM_LL_GROUP_NUM)
#define CAP_CHANNELS (MCPWM_LL_GROUP_NUM * MCPWM_LL_CAPTURE_CHANNELS_PER_TIMER)

typedef struct {
    mp_obj_base_t base;
    mcpwm_cap_channel_handle_t chan;
    uint32_t *buf;  // Interval in timer ticks, with the level after the edge in bit 31.
    uint16_t size;
    volatile uint16_t head;
    volatile uint16_t tail;
    volatile uint32_t lost;
    // Last edges, for frequency and duty.
    volatile uint32_t last_edge;
    volatile uint32_t last_rise;
    volatile uint32_t period;
    volatile uint32_t high;
    volatile uint32_t edges;
    volatile bool seen_rise;
    int8_t group;
    uint8_t pin;
    bool owns_pin;
} espidf_pulse_capture_obj_t;

static mcpwm_cap_timer_handle_t _timers[CAP_GROUPS];
static uint8_t _timer_users[CAP_GROUPS];
static espidf_pulse_capture_obj_t *_open[CAP_CHANNELS];

static bool on_capture(mcpwm_cap_channel_handle_t chan, const mcpwm_capture_event_data_t *edata, void *user_ctx) {
    espidf_pulse_capture_obj_t *self = user_ctx;
    uint32_t now = edata->cap_value;
    bool rising = edata->cap_edge == MCPWM_CAP_EDGE_POS;
    if (self->edges != 0) {
        uint32_t interval = now - self->last_edge;
        uint16_t next = (self->head + 1) % self->size;
        if (next == self->tail) {
            self->lost++;
        } else {
            self->buf[self->head] = (interval & 0x7fffffff) | (rising ? 0x80000000 : 0);
            self->head = next;
        }
    }
    if (self->seen_rise) {
        if (rising) {
            self->period = now - self->last_rise;
        } else {
            self->high = now - self->last_rise;
        }
    }
    if (rising) {
        self->last_rise = now;
        self->seen_rise = true;
    }
    self->last_edge = now;
    self->edges++;
    return false;
}

static void release_timer(int group) {
    if (--_timer_users[group] == 0) {
        mcpwm_capture_timer_stop(_timers[group]);
        mcpwm_capture_timer_disable(_timers[group]);
        mcpwm_del_capture_timer(_timers[group]);
        _timers[group] = NULL;
    }
}

static void capture_close(espidf_pulse_capture_obj_t *self) {
    if (self->chan == NULL) {
        return;
    }
    mcpwm_capture_channel_disable(self->chan);
    mcpwm_del_capture_channel(self->chan);
    self->chan = NULL;
    release_timer(self->group);
    if (self->owns_pin) {
        reset_pin_number(self->pin);
    }
    for (size_t i = 0; i < CAP_CHANNELS; i++) {
        if (_open[i] == self) {
            _open[i] = NULL;
        }
    }
}

void espidf_mcpwm_cap_reset(void) {
    for (size_t i = 0; i < CAP_CHANNELS; i++) {
        if (_open[i] != NULL) {
            capture_close(_open[i]);
        }
    }
}

static espidf_pulse_capture_obj_t *get_open(mp_obj_t self_in) {
    espidf_pulse_capture_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->chan == NULL) {
        raise_deinited_error();
    }
    return self;
}

static uint32_t resolution(espidf_pulse_capture_obj_t *self) {
    uint32_t hz;
    CHECK_ESP_RESULT(mcpwm_capture_timer_get_resolution(_timers[self->group], &hz));
    return hz;
}

//| class PulseCapture:
//|     """Edge times of a pin, latched in hardware by MCPWM capture: each
//|     interval to the resolution of the capture timer (12.5 ns at 80 MHz),
//|     however late the interrupt. Three at a time on the C5, six on the S3.
//|
//|     The pin may be one another object already drives -- `MotorPWM`,
//|     `pwmio`, `SigmaDelta` -- and is then only listened to, which measures
//|     an output with nothing wired."""
//|
//|     def __init__(self, pin: microcontroller.Pin, *, edges: str = "both", maxlen: int = 128) -> None:
//|         """
//|         :param ~microcontroller.Pin pin: the input.
//|         :param str edges: ``"rising"``, ``"falling"`` or ``"both"``.
//|         :param int maxlen: intervals kept until `read`.
//|
//|         Each edge is latched in hardware but read by an interrupt, and a
//|         channel holds one value: edges closer than about 2-4 us give wrong
//|         intervals (`frequency` and `duty` then read None). Edges were
//|         caught up to 200 kHz with both edges at 50 % duty."""
//|         ...
//|
static mp_obj_t espidf_pulse_capture_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_pin, ARG_edges, ARG_maxlen };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_pin, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_edges, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = MP_OBJ_NEW_QSTR(MP_QSTR_both) } },
        { MP_QSTR_maxlen, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 128 } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    const mcu_pin_obj_t *pin = validate_obj_is_pin(args[ARG_pin].u_obj, MP_QSTR_pin);
    qstr edges = mp_obj_str_get_qstr(args[ARG_edges].u_obj);
    if (edges != MP_QSTR_rising && edges != MP_QSTR_falling && edges != MP_QSTR_both) {
        mp_arg_error_invalid(MP_QSTR_edges);
    }
    mp_int_t maxlen = mp_arg_validate_int_range(args[ARG_maxlen].u_int, 1, 32767, MP_QSTR_maxlen);

    size_t slot = 0;
    while (slot < CAP_CHANNELS && _open[slot] != NULL) {
        slot++;
    }
    if (slot == CAP_CHANNELS) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("All capture channels in use"));
    }

    espidf_pulse_capture_obj_t *self = mp_obj_malloc_with_finaliser(espidf_pulse_capture_obj_t, &espidf_pulse_capture_type);
    self->chan = NULL;
    // One more than kept: a full ring has one empty slot.
    self->size = maxlen + 1;
    self->buf = m_new(uint32_t, self->size);
    self->head = self->tail = 0;
    self->lost = self->edges = self->period = self->high = 0;
    self->seen_rise = false;
    self->pin = pin->number;
    self->owns_pin = common_hal_mcu_pin_is_free(pin);

    // The first group with a free channel; a group's timer is made on first use.
    esp_err_t err = ESP_ERR_NOT_FOUND;
    for (int group = 0; group < CAP_GROUPS && err != ESP_OK; group++) {
        bool made = false;
        if (_timers[group] == NULL) {
            mcpwm_capture_timer_config_t tconf = {
                .group_id = group,
                .clk_src = MCPWM_CAPTURE_CLK_SRC_DEFAULT,
            };
            if (mcpwm_new_capture_timer(&tconf, &_timers[group]) != ESP_OK) {
                continue;
            }
            mcpwm_capture_timer_enable(_timers[group]);
            mcpwm_capture_timer_start(_timers[group]);
            made = true;
        }
        mcpwm_capture_channel_config_t cconf = {
            .gpio_num = pin->number,
            .prescale = 1,
            .flags.pos_edge = edges != MP_QSTR_falling,
            .flags.neg_edge = edges != MP_QSTR_rising,
        };
        err = mcpwm_new_capture_channel(_timers[group], &cconf, &self->chan);
        if (err == ESP_OK) {
            self->group = group;
            _timer_users[group]++;
        } else if (made) {
            _timer_users[group] = 1;
            release_timer(group);
        }
    }
    if (err != ESP_OK) {
        self->chan = NULL;
        mp_raise_RuntimeError(MP_ERROR_TEXT("All capture channels in use"));
    }
    mcpwm_capture_event_callbacks_t cbs = { .on_cap = on_capture };
    err = mcpwm_capture_channel_register_event_callbacks(self->chan, &cbs, self);
    if (err == ESP_OK) {
        err = mcpwm_capture_channel_enable(self->chan);
    }
    if (err != ESP_OK) {
        mcpwm_del_capture_channel(self->chan);
        self->chan = NULL;
        release_timer(self->group);
        CHECK_ESP_RESULT(err);
    }
    if (self->owns_pin) {
        claim_pin(pin);
    }
    _open[slot] = self;
    return MP_OBJ_FROM_PTR(self);
}

//|     def read(self) -> List[Tuple[float, bool]]:
//|         """The intervals since the last read, oldest first: each the time
//|         in microseconds from the edge before, and True if this edge was
//|         rising. With one edge type that is the period."""
//|         ...
//|
static mp_obj_t espidf_pulse_capture_read(mp_obj_t self_in) {
    espidf_pulse_capture_obj_t *self = get_open(self_in);
    mp_float_t us_per_tick = (mp_float_t)1e6 / resolution(self);
    mp_obj_t list = mp_obj_new_list(0, NULL);
    while (self->tail != self->head) {
        uint32_t v = self->buf[self->tail];
        self->tail = (self->tail + 1) % self->size;
        mp_obj_t item[2] = {
            mp_obj_new_float((v & 0x7fffffff) * us_per_tick),
            mp_obj_new_bool(v & 0x80000000),
        };
        mp_obj_list_append(list, mp_obj_new_tuple(2, item));
    }
    return list;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_pulse_capture_read_obj, espidf_pulse_capture_read);

//|     frequency: Optional[float]
//|     """From the last two rising edges, in Hz; None before there are two.
//|     Needs ``edges`` to include rising."""
//|
//|     duty: Optional[float]
//|     """High share of the last whole period, 0.0 to 1.0; needs both edges."""
//|
//|     edges: int
//|     """Edges captured since the start."""
//|
//|     lost: int
//|     """Intervals dropped because `read` did not keep up."""
//|
//|     resolution: int
//|     """Capture timer ticks per second."""
//|
static mp_obj_t espidf_pulse_capture_get_frequency(mp_obj_t self_in) {
    espidf_pulse_capture_obj_t *self = get_open(self_in);
    uint32_t period = self->period;
    if (period == 0) {
        return mp_const_none;
    }
    return mp_obj_new_float((mp_float_t)resolution(self) / period);
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_pulse_capture_get_frequency_obj, espidf_pulse_capture_get_frequency);
MP_PROPERTY_GETTER(espidf_pulse_capture_frequency_obj, (mp_obj_t)&espidf_pulse_capture_get_frequency_obj);

static mp_obj_t espidf_pulse_capture_get_duty(mp_obj_t self_in) {
    espidf_pulse_capture_obj_t *self = get_open(self_in);
    uint32_t period = self->period;
    uint32_t high = self->high;
    if (period == 0 || high == 0 || high > period) {
        return mp_const_none;
    }
    return mp_obj_new_float((mp_float_t)high / period);
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_pulse_capture_get_duty_obj, espidf_pulse_capture_get_duty);
MP_PROPERTY_GETTER(espidf_pulse_capture_duty_obj, (mp_obj_t)&espidf_pulse_capture_get_duty_obj);

static mp_obj_t espidf_pulse_capture_get_edges(mp_obj_t self_in) {
    return mp_obj_new_int_from_uint(get_open(self_in)->edges);
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_pulse_capture_get_edges_obj, espidf_pulse_capture_get_edges);
MP_PROPERTY_GETTER(espidf_pulse_capture_edges_obj, (mp_obj_t)&espidf_pulse_capture_get_edges_obj);

static mp_obj_t espidf_pulse_capture_get_lost(mp_obj_t self_in) {
    return mp_obj_new_int_from_uint(get_open(self_in)->lost);
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_pulse_capture_get_lost_obj, espidf_pulse_capture_get_lost);
MP_PROPERTY_GETTER(espidf_pulse_capture_lost_obj, (mp_obj_t)&espidf_pulse_capture_get_lost_obj);

static mp_obj_t espidf_pulse_capture_get_resolution(mp_obj_t self_in) {
    return mp_obj_new_int_from_uint(resolution(get_open(self_in)));
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_pulse_capture_get_resolution_obj, espidf_pulse_capture_get_resolution);
MP_PROPERTY_GETTER(espidf_pulse_capture_resolution_obj, (mp_obj_t)&espidf_pulse_capture_get_resolution_obj);

//|     def deinit(self) -> None:
//|         """Release the channel, and the pin if this object took it."""
//|         ...
//|
static mp_obj_t espidf_pulse_capture_deinit(mp_obj_t self_in) {
    capture_close(MP_OBJ_TO_PTR(self_in));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_pulse_capture_deinit_obj, espidf_pulse_capture_deinit);

static mp_obj_t espidf_pulse_capture___exit__(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    return espidf_pulse_capture_deinit(args[0]);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(espidf_pulse_capture___exit___obj, 4, 4, espidf_pulse_capture___exit__);

static const mp_rom_map_elem_t espidf_pulse_capture_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&espidf_pulse_capture_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_frequency), MP_ROM_PTR(&espidf_pulse_capture_frequency_obj) },
    { MP_ROM_QSTR(MP_QSTR_duty), MP_ROM_PTR(&espidf_pulse_capture_duty_obj) },
    { MP_ROM_QSTR(MP_QSTR_edges), MP_ROM_PTR(&espidf_pulse_capture_edges_obj) },
    { MP_ROM_QSTR(MP_QSTR_lost), MP_ROM_PTR(&espidf_pulse_capture_lost_obj) },
    { MP_ROM_QSTR(MP_QSTR_resolution), MP_ROM_PTR(&espidf_pulse_capture_resolution_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&espidf_pulse_capture_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&espidf_pulse_capture_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&espidf_pulse_capture___exit___obj) },
};
static MP_DEFINE_CONST_DICT(espidf_pulse_capture_locals_dict, espidf_pulse_capture_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    espidf_pulse_capture_type,
    MP_QSTR_PulseCapture,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, espidf_pulse_capture_make_new,
    locals_dict, &espidf_pulse_capture_locals_dict
    );

#endif
