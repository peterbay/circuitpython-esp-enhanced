// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Petr Vavrin
//
// SPDX-License-Identifier: MIT

// espidf.MotorPWM; see extended.h.

#include "py/obj.h"
#include "py/objproperty.h"
#include "py/runtime.h"

#include "bindings/espidf/__init__.h"
#include "bindings/espidf/extended.h"
#include "common-hal/microcontroller/Pin.h"
#include "shared-bindings/microcontroller/Pin.h"
#include "shared-bindings/util.h"
#include "shared/runtime/context_manager_helpers.h"

#include "driver/gpio.h"
#include "driver/mcpwm_prelude.h"
#include "hal/mcpwm_ll.h"

// The MCPWM peripheral, used for what pwmio cannot do: a complementary pair of
// outputs with dead time between them, the gate signals of a half bridge.
// One timer, operator, comparator and one or two generators per object.

#if CIRCUITPY_ESPIDF_MCPWM

// Fixed, so that the timers sharing a group never ask for different
// prescalers: 12.5 ns steps for duty and dead time. With a 16-bit period the
// lowest frequency is 80 MHz / 65535.
#define MCPWM_RESOLUTION_HZ (80000000)
#define MCPWM_MAX_PERIOD (65535)
#define MCPWM_SLOTS (MCPWM_LL_GROUP_NUM * MCPWM_LL_TIMERS_PER_GROUP)

typedef struct {
    mp_obj_base_t base;
    mcpwm_timer_handle_t timer;
    mcpwm_oper_handle_t oper;
    mcpwm_cmpr_handle_t cmpr;
    mcpwm_gen_handle_t gen_a;
    mcpwm_gen_handle_t gen_b;
    const mcu_pin_obj_t *pin_a;
    const mcu_pin_obj_t *pin_b;
    uint32_t period;
    uint32_t dead_ticks;
    mp_float_t duty;
} espidf_mcpwm_obj_t;

static espidf_mcpwm_obj_t *_open[MCPWM_SLOTS];

// Deletes whatever has been made so far, in reverse; safe on a part-built object.
static void mcpwm_release(espidf_mcpwm_obj_t *self) {
    if (self->timer != NULL) {
        mcpwm_timer_start_stop(self->timer, MCPWM_TIMER_STOP_EMPTY);
        mcpwm_timer_disable(self->timer);
    }
    if (self->gen_b != NULL) {
        mcpwm_del_generator(self->gen_b);
        self->gen_b = NULL;
    }
    if (self->gen_a != NULL) {
        mcpwm_del_generator(self->gen_a);
        self->gen_a = NULL;
    }
    if (self->cmpr != NULL) {
        mcpwm_del_comparator(self->cmpr);
        self->cmpr = NULL;
    }
    if (self->oper != NULL) {
        mcpwm_del_operator(self->oper);
        self->oper = NULL;
    }
    if (self->timer != NULL) {
        mcpwm_del_timer(self->timer);
        self->timer = NULL;
    }
}

static void mcpwm_close(espidf_mcpwm_obj_t *self) {
    if (self->timer == NULL) {
        return;
    }
    mcpwm_release(self);
    reset_pin_number(self->pin_a->number);
    if (self->pin_b != NULL) {
        reset_pin_number(self->pin_b->number);
    }
    for (size_t i = 0; i < MCPWM_SLOTS; i++) {
        if (_open[i] == self) {
            _open[i] = NULL;
        }
    }
}

void espidf_mcpwm_reset(void) {
    for (size_t i = 0; i < MCPWM_SLOTS; i++) {
        if (_open[i] != NULL) {
            mcpwm_close(_open[i]);
        }
    }
}

static void check_for_deinit(espidf_mcpwm_obj_t *self) {
    if (self->timer == NULL) {
        raise_deinited_error();
    }
}

static void set_duty(espidf_mcpwm_obj_t *self, mp_float_t duty) {
    if (!(duty >= 0 && duty <= 1)) {
        mp_raise_ValueError_varg(MP_ERROR_TEXT("%q must be %d-%d"), MP_QSTR_duty, 0, 1);
    }
    uint32_t ticks = (uint32_t)(duty * self->period + 0.5f);
    // The comparator matches 0..period-1. At the ends the compare event and
    // the period event coincide or never come, so 0 and 1 are forced instead.
    if (ticks == 0) {
        CHECK_ESP_RESULT(mcpwm_generator_set_force_level(self->gen_a, 0, true));
    } else if (ticks >= self->period) {
        CHECK_ESP_RESULT(mcpwm_generator_set_force_level(self->gen_a, 1, true));
    } else {
        CHECK_ESP_RESULT(mcpwm_comparator_set_compare_value(self->cmpr, ticks));
        CHECK_ESP_RESULT(mcpwm_generator_set_force_level(self->gen_a, -1, true));
    }
    self->duty = duty;
}

// Tries each group in turn; a group whose timers are all taken says NOT_FOUND.
static esp_err_t build(espidf_mcpwm_obj_t *self) {
    esp_err_t err = ESP_ERR_NOT_FOUND;
    for (int group = 0; group < MCPWM_LL_GROUP_NUM && err == ESP_ERR_NOT_FOUND; group++) {
        mcpwm_timer_config_t timer_config = {
            .group_id = group,
            .clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT,
            .resolution_hz = MCPWM_RESOLUTION_HZ,
            .count_mode = MCPWM_TIMER_COUNT_MODE_UP,
            .period_ticks = self->period,
        };
        err = mcpwm_new_timer(&timer_config, &self->timer);
        if (err != ESP_OK) {
            continue;
        }
        mcpwm_operator_config_t oper_config = { .group_id = group };
        err = mcpwm_new_operator(&oper_config, &self->oper);
        if (err != ESP_OK) {
            mcpwm_del_timer(self->timer);
            self->timer = NULL;
        }
    }
    if (err != ESP_OK) {
        return err;
    }
    err = mcpwm_operator_connect_timer(self->oper, self->timer);
    if (err != ESP_OK) {
        return err;
    }
    mcpwm_comparator_config_t cmpr_config = { .flags.update_cmp_on_tez = true };
    err = mcpwm_new_comparator(self->oper, &cmpr_config, &self->cmpr);
    if (err != ESP_OK) {
        return err;
    }
    mcpwm_generator_config_t gen_config = { .gen_gpio_num = self->pin_a->number };
    err = mcpwm_new_generator(self->oper, &gen_config, &self->gen_a);
    if (err != ESP_OK) {
        return err;
    }
    // High from the start of each period until the compare value.
    err = mcpwm_generator_set_action_on_timer_event(self->gen_a,
        MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, MCPWM_TIMER_EVENT_EMPTY, MCPWM_GEN_ACTION_HIGH));
    if (err == ESP_OK) {
        err = mcpwm_generator_set_action_on_compare_event(self->gen_a,
            MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, self->cmpr, MCPWM_GEN_ACTION_LOW));
    }
    if (err != ESP_OK) {
        return err;
    }
    if (self->pin_b != NULL) {
        gen_config.gen_gpio_num = self->pin_b->number;
        err = mcpwm_new_generator(self->oper, &gen_config, &self->gen_b);
        if (err != ESP_OK) {
            return err;
        }
        // A's rising edge delayed; B is A inverted with its own rising edge
        // (A's falling one) delayed, so the two are never high together.
        mcpwm_dead_time_config_t dead_a = { .posedge_delay_ticks = self->dead_ticks };
        err = mcpwm_generator_set_dead_time(self->gen_a, self->gen_a, &dead_a);
        if (err != ESP_OK) {
            return err;
        }
        mcpwm_dead_time_config_t dead_b = { .negedge_delay_ticks = self->dead_ticks, .flags.invert_output = true };
        err = mcpwm_generator_set_dead_time(self->gen_a, self->gen_b, &dead_b);
        if (err != ESP_OK) {
            return err;
        }
    }
    err = mcpwm_timer_enable(self->timer);
    if (err == ESP_OK) {
        err = mcpwm_timer_start_stop(self->timer, MCPWM_TIMER_START_NO_STOP);
    }
    return err;
}

//| class MotorPWM:
//|     """PWM from the MCPWM peripheral, optionally as a complementary pair with
//|     dead time: the two gate signals of a half bridge. Three at a time on the
//|     C5, six on the S3."""
//|
//|     def __init__(
//|         self,
//|         pin_a: microcontroller.Pin,
//|         pin_b: Optional[microcontroller.Pin] = None,
//|         *,
//|         frequency: int = 20000,
//|         duty: float = 0.0,
//|         dead_time_ns: int = 0,
//|     ) -> None:
//|         """
//|         :param ~microcontroller.Pin pin_a: high for ``duty`` of each period.
//|         :param ~microcontroller.Pin pin_b: the complement of ``pin_a``; with
//|           ``dead_time_ns`` both are low for that long around each edge.
//|         :param int frequency: 1221 Hz to 40 MHz; the timer counts at 80 MHz,
//|           so the duty has 80 MHz / frequency steps. For servos and other slow
//|           PWM use `pwmio`.
//|         :param float duty: 0.0 to 1.0.
//|         :param int dead_time_ns: in 12.5 ns steps; needs ``pin_b``.
//|
//|         Both pins' inputs stay enabled, so their levels can be read back."""
//|         ...
//|
static mp_obj_t espidf_mcpwm_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_pin_a, ARG_pin_b, ARG_frequency, ARG_duty, ARG_dead_time_ns };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_pin_a, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_pin_b, MP_ARG_OBJ, { .u_obj = mp_const_none } },
        { MP_QSTR_frequency, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 20000 } },
        { MP_QSTR_duty, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = MP_OBJ_NULL } },
        { MP_QSTR_dead_time_ns, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 0 } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    const mcu_pin_obj_t *pin_a = validate_obj_is_free_pin(args[ARG_pin_a].u_obj, MP_QSTR_pin_a);
    const mcu_pin_obj_t *pin_b = NULL;
    if (args[ARG_pin_b].u_obj != mp_const_none) {
        pin_b = validate_obj_is_free_pin(args[ARG_pin_b].u_obj, MP_QSTR_pin_b);
        if (pin_b == pin_a) {
            mp_raise_ValueError(MP_ERROR_TEXT("pin_a and pin_b must differ"));
        }
    }
    mp_int_t frequency = mp_arg_validate_int_range(args[ARG_frequency].u_int,
        MCPWM_RESOLUTION_HZ / MCPWM_MAX_PERIOD + 1, MCPWM_RESOLUTION_HZ / 2, MP_QSTR_frequency);
    mp_int_t dead_time_ns = mp_arg_validate_int_min(args[ARG_dead_time_ns].u_int, 0, MP_QSTR_dead_time_ns);
    if (dead_time_ns > 0 && pin_b == NULL) {
        mp_raise_ValueError(MP_ERROR_TEXT("dead_time_ns needs pin_b"));
    }
    mp_float_t duty = args[ARG_duty].u_obj == MP_OBJ_NULL ? 0 : mp_obj_get_float(args[ARG_duty].u_obj);
    // Checked here as well as in set_duty(): once the timer exists and the
    // pins are claimed, raising would leave both taken until the next reset.
    if (!(duty >= 0 && duty <= 1)) {
        mp_raise_ValueError_varg(MP_ERROR_TEXT("%q must be %d-%d"), MP_QSTR_duty, 0, 1);
    }

    size_t slot = 0;
    while (slot < MCPWM_SLOTS && _open[slot] != NULL) {
        slot++;
    }
    if (slot == MCPWM_SLOTS) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("All MCPWM timers in use"));
    }

    espidf_mcpwm_obj_t *self = mp_obj_malloc_with_finaliser(espidf_mcpwm_obj_t, &espidf_mcpwm_type);
    self->timer = NULL;
    self->oper = NULL;
    self->cmpr = NULL;
    self->gen_a = NULL;
    self->gen_b = NULL;
    self->pin_a = pin_a;
    self->pin_b = pin_b;
    self->period = MCPWM_RESOLUTION_HZ / frequency;
    self->dead_ticks = (uint32_t)((uint64_t)dead_time_ns * (MCPWM_RESOLUTION_HZ / 1000000) / 1000);
    if (self->dead_ticks >= self->period / 2 && self->dead_ticks > 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("dead_time_ns must be under half the period"));
    }

    esp_err_t err = build(self);
    if (err == ESP_OK) {
        err = gpio_input_enable(pin_a->number);
    }
    if (err == ESP_OK && pin_b != NULL) {
        err = gpio_input_enable(pin_b->number);
    }
    if (err != ESP_OK) {
        mcpwm_release(self);
        if (err == ESP_ERR_NOT_FOUND) {
            mp_raise_RuntimeError(MP_ERROR_TEXT("All MCPWM timers in use"));
        }
        raise_esp_error(err);
    }
    claim_pin(pin_a);
    if (pin_b != NULL) {
        claim_pin(pin_b);
    }
    _open[slot] = self;
    set_duty(self, duty);
    return MP_OBJ_FROM_PTR(self);
}

//|     def deinit(self) -> None:
//|         """Stop and release the pins and the timer."""
//|         ...
//|
static mp_obj_t espidf_mcpwm_deinit(mp_obj_t self_in) {
    mcpwm_close(MP_OBJ_TO_PTR(self_in));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_mcpwm_deinit_obj, espidf_mcpwm_deinit);

//|     def __enter__(self) -> MotorPWM:
//|         """No-op used by Context Managers."""
//|         ...
//|
//|     def __exit__(self) -> None:
//|         """Automatically deinitializes when exiting a context."""
//|         ...
//|
static mp_obj_t espidf_mcpwm___exit__(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    return espidf_mcpwm_deinit(args[0]);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(espidf_mcpwm___exit___obj, 4, 4, espidf_mcpwm___exit__);

//|     duty: float
//|     """Share of each period ``pin_a`` is high, 0.0 to 1.0, in steps of
//|     frequency / 80 MHz. Takes effect at the start of the next period."""
//|
//|     frequency: int
//|     """The frequency actually set: 80 MHz divided by a whole number."""
//|
//|
static mp_obj_t espidf_mcpwm_get_duty(mp_obj_t self_in) {
    espidf_mcpwm_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    return mp_obj_new_float(self->duty);
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_mcpwm_get_duty_obj, espidf_mcpwm_get_duty);

static mp_obj_t espidf_mcpwm_set_duty(mp_obj_t self_in, mp_obj_t value) {
    espidf_mcpwm_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    set_duty(self, mp_obj_get_float(value));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(espidf_mcpwm_set_duty_obj, espidf_mcpwm_set_duty);

MP_PROPERTY_GETSET(espidf_mcpwm_duty_obj,
    (mp_obj_t)&espidf_mcpwm_get_duty_obj,
    (mp_obj_t)&espidf_mcpwm_set_duty_obj);

static mp_obj_t espidf_mcpwm_get_frequency(mp_obj_t self_in) {
    espidf_mcpwm_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    return mp_obj_new_int_from_uint(MCPWM_RESOLUTION_HZ / self->period);
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_mcpwm_get_frequency_obj, espidf_mcpwm_get_frequency);

MP_PROPERTY_GETTER(espidf_mcpwm_frequency_obj, (mp_obj_t)&espidf_mcpwm_get_frequency_obj);

static const mp_rom_map_elem_t espidf_mcpwm_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&espidf_mcpwm_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&espidf_mcpwm_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&espidf_mcpwm___exit___obj) },
    { MP_ROM_QSTR(MP_QSTR_duty), MP_ROM_PTR(&espidf_mcpwm_duty_obj) },
    { MP_ROM_QSTR(MP_QSTR_frequency), MP_ROM_PTR(&espidf_mcpwm_frequency_obj) },
};
static MP_DEFINE_CONST_DICT(espidf_mcpwm_locals_dict, espidf_mcpwm_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    espidf_mcpwm_type,
    MP_QSTR_MotorPWM,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, espidf_mcpwm_make_new,
    locals_dict, &espidf_mcpwm_locals_dict
    );

#endif
