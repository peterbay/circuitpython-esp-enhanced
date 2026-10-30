// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Petr Vavrin
//
// SPDX-License-Identifier: MIT

// espidf.pwm_fade() and espidf.pwm_fading(); see extended.h.

#include "py/mphal.h"
#include "py/obj.h"
#include "py/runtime.h"

#include "bindings/espidf/__init__.h"
#include "bindings/espidf/extended.h"
#include "shared-bindings/pwmio/PWMOut.h"
#include "shared-bindings/util.h"

// The headers below exist only where the switch brings them in.
#if CIRCUITPY_ESPIDF_LEDC_FADE

#include "driver/ledc.h"

// The LEDC's hardware fade on a pwmio.PWMOut: the peripheral steps the duty
// towards a target by itself, so a dimming LED changes smoothly without the
// program setting it again and again. PWMOut reads its duty back from the
// hardware, so duty_cycle follows the fade.

static bool _installed;
static volatile bool _fading[LEDC_CHANNEL_MAX];

static bool on_fade_end(const ledc_cb_param_t *param, void *user_arg) {
    if (param->event == LEDC_FADE_END_EVT && param->channel < LEDC_CHANNEL_MAX) {
        _fading[param->channel] = false;
    }
    return false;
}

static pwmio_pwmout_obj_t *get_pwm(mp_obj_t obj) {
    pwmio_pwmout_obj_t *pwm = MP_OBJ_TO_PTR(mp_arg_validate_type(obj, &pwmio_pwmout_type, MP_QSTR_pwm));
    if (common_hal_pwmio_pwmout_deinited(pwm)) {
        raise_deinited_error();
    }
    return pwm;
}

//| def pwm_fade(pwm: pwmio.PWMOut, duty_cycle: int, duration: float, *, wait: bool = False) -> None:
//|     """Fade ``pwm`` from its duty cycle to ``duty_cycle`` (0 to 65535) over
//|     ``duration`` seconds, in hardware: linear steps of the duty, which the
//|     LEDC times itself. `pwmio.PWMOut.duty_cycle` follows it. A fade still
//|     running is stopped first; setting ``duty_cycle`` during a fade does not
//|     stop it, so call `pwm_fade` with a duration of 0 instead.
//|
//|     :param bool wait: return when the fade has ended."""
//|     ...
//|
static mp_obj_t espidf_pwm_fade(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_pwm, ARG_duty_cycle, ARG_duration, ARG_wait };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_pwm, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_duty_cycle, MP_ARG_REQUIRED | MP_ARG_INT },
        { MP_QSTR_duration, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_wait, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = false } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    pwmio_pwmout_obj_t *pwm = get_pwm(args[ARG_pwm].u_obj);
    mp_int_t duty = mp_arg_validate_int_range(args[ARG_duty_cycle].u_int, 0, 0xffff, MP_QSTR_duty_cycle);
    mp_float_t duration = mp_arg_validate_obj_float_non_negative(args[ARG_duration].u_obj, 0, MP_QSTR_duration);
    mp_int_t ms = (mp_int_t)(duration * 1000);
    mp_arg_validate_int_max(ms, 60 * 60 * 1000, MP_QSTR_duration);

    if (!_installed) {
        CHECK_ESP_RESULT(ledc_fade_func_install(0));
        _installed = true;
    }
    ledc_channel_t channel = pwm->chan_handle.channel;
    if (_fading[channel]) {
        ledc_fade_stop(LEDC_LOW_SPEED_MODE, channel);
        _fading[channel] = false;
    }
    ledc_cbs_t cbs = { .fade_cb = on_fade_end };
    CHECK_ESP_RESULT(ledc_cb_register(LEDC_LOW_SPEED_MODE, channel, &cbs, NULL));
    // PWMOut keeps the duty in its timer's resolution, as here.
    uint32_t target = (uint32_t)duty >> (16 - pwm->duty_resolution);
    if (ms == 0) {
        CHECK_ESP_RESULT(ledc_set_duty(LEDC_LOW_SPEED_MODE, channel, target));
        CHECK_ESP_RESULT(ledc_update_duty(LEDC_LOW_SPEED_MODE, channel));
        return mp_const_none;
    }
    CHECK_ESP_RESULT(ledc_set_fade_with_time(LEDC_LOW_SPEED_MODE, channel, target, ms));
    _fading[channel] = true;
    esp_err_t err = ledc_fade_start(LEDC_LOW_SPEED_MODE, channel, LEDC_FADE_NO_WAIT);
    if (err != ESP_OK) {
        _fading[channel] = false;
        CHECK_ESP_RESULT(err);
    }
    if (args[ARG_wait].u_bool) {
        while (_fading[channel]) {
            RUN_BACKGROUND_TASKS;
            if (mp_hal_is_interrupted()) {
                break;
            }
        }
    }
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_KW(espidf_pwm_fade_obj, 3, espidf_pwm_fade);

//| def pwm_fading(pwm: pwmio.PWMOut) -> bool:
//|     """True while a `pwm_fade` on ``pwm`` is running."""
//|     ...
//|
static mp_obj_t espidf_pwm_fading(mp_obj_t pwm_in) {
    return mp_obj_new_bool(_fading[get_pwm(pwm_in)->chan_handle.channel]);
}
MP_DEFINE_CONST_FUN_OBJ_1(espidf_pwm_fading_obj, espidf_pwm_fading);

void espidf_ledc_fade_reset(void) {
    for (int channel = 0; channel < LEDC_CHANNEL_MAX; channel++) {
        if (_fading[channel]) {
            ledc_fade_stop(LEDC_LOW_SPEED_MODE, channel);
            _fading[channel] = false;
        }
    }
}

#endif
