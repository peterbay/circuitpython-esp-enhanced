// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Petr Vavrin
//
// SPDX-License-Identifier: MIT

// espidf.SigmaDelta; see extended.h.

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
#if CIRCUITPY_ESPIDF_SDM

#include "driver/gpio.h"
#include "driver/sdm.h"
#include "hal/sdm_caps.h"

// Sigma-delta modulation: a pin toggled at sample_rate so that the share of
// time it is high follows `density`. Through an RC low-pass filter that is a
// cheap DAC on any pin -- the S3 has no DAC at all -- and straight into an LED
// it dims without the PWM flicker.

// Eight channels on the S3, four on the C5.
#define SDM_SLOTS (SDM_CAPS_GET(INST_NUM) * SDM_CAPS_GET(CHANS_PER_INST))

typedef struct {
    mp_obj_base_t base;
    sdm_channel_handle_t channel;
    const mcu_pin_obj_t *pin;
    uint32_t sample_rate;
    int8_t density;
} espidf_sdm_obj_t;

// Channels left open by a script are closed on soft reset; the driver keeps
// them otherwise, and the hardware has only eight.
static espidf_sdm_obj_t *_open[SDM_SLOTS];

static void sdm_close(espidf_sdm_obj_t *self) {
    if (self->channel == NULL) {
        return;
    }
    sdm_channel_disable(self->channel);
    sdm_del_channel(self->channel);
    self->channel = NULL;
    reset_pin_number(self->pin->number);
    for (size_t i = 0; i < SDM_SLOTS; i++) {
        if (_open[i] == self) {
            _open[i] = NULL;
        }
    }
}

void espidf_sdm_reset(void) {
    for (size_t i = 0; i < SDM_SLOTS; i++) {
        if (_open[i] != NULL) {
            sdm_close(_open[i]);
        }
    }
}

static void check_for_deinit(espidf_sdm_obj_t *self) {
    if (self->channel == NULL) {
        raise_deinited_error();
    }
}

//| class SigmaDelta:
//|     """A pin driven by the sigma-delta modulator: high for a share of the
//|     time set by `density`, in pulses at ``sample_rate``. Behind an RC
//|     low-pass filter, a DAC. Eight at a time on the S3, four on the C5."""
//|
//|     def __init__(self, pin: microcontroller.Pin, *, sample_rate: int = 1_000_000, density: int = 0) -> None:
//|         """
//|         :param ~microcontroller.Pin pin: any output-capable pin.
//|         :param int sample_rate: pulse rate in Hz, up to 80 MHz divided by an
//|           integer of at least 1. A higher rate is easier to filter.
//|         :param int density: -128 (always low) to 127 (almost always high);
//|           0 is half.
//|
//|         The pin's input stays enabled, so its level can be read back, for
//|         instance with `espidf.pin_status`."""
//|         ...
//|
static mp_obj_t espidf_sdm_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_pin, ARG_sample_rate, ARG_density };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_pin, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_sample_rate, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 1000000 } },
        { MP_QSTR_density, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 0 } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    const mcu_pin_obj_t *pin = validate_obj_is_free_pin(args[ARG_pin].u_obj, MP_QSTR_pin);
    mp_int_t sample_rate = mp_arg_validate_int_range(args[ARG_sample_rate].u_int, 1000, 80000000, MP_QSTR_sample_rate);
    mp_int_t density = mp_arg_validate_int_range(args[ARG_density].u_int, -128, 127, MP_QSTR_density);

    size_t slot = 0;
    while (slot < SDM_SLOTS && _open[slot] != NULL) {
        slot++;
    }
    if (slot == SDM_SLOTS) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("All sigma-delta channels in use"));
    }

    espidf_sdm_obj_t *self = mp_obj_malloc_with_finaliser(espidf_sdm_obj_t, &espidf_sdm_type);
    self->pin = pin;
    self->sample_rate = sample_rate;
    self->density = density;
    self->channel = NULL;

    sdm_config_t config = {
        .gpio_num = pin->number,
        .clk_src = SDM_CLK_SRC_DEFAULT,
        .sample_rate_hz = sample_rate,
    };
    CHECK_ESP_RESULT(sdm_new_channel(&config, &self->channel));
    esp_err_t err = sdm_channel_set_pulse_density(self->channel, density);
    if (err == ESP_OK) {
        err = sdm_channel_enable(self->channel);
    }
    if (err == ESP_OK) {
        err = gpio_input_enable(pin->number);
    }
    if (err != ESP_OK) {
        sdm_del_channel(self->channel);
        self->channel = NULL;
        raise_esp_error(err);
    }
    claim_pin(pin);
    _open[slot] = self;
    return MP_OBJ_FROM_PTR(self);
}

//|     def deinit(self) -> None:
//|         """Stop the modulator and release the pin."""
//|         ...
//|
static mp_obj_t espidf_sdm_deinit(mp_obj_t self_in) {
    sdm_close(MP_OBJ_TO_PTR(self_in));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_sdm_deinit_obj, espidf_sdm_deinit);

//|     def __enter__(self) -> SigmaDelta:
//|         """No-op used by Context Managers."""
//|         ...
//|
//|     def __exit__(self) -> None:
//|         """Automatically deinitializes when exiting a context."""
//|         ...
//|
static mp_obj_t espidf_sdm___exit__(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    return espidf_sdm_deinit(args[0]);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(espidf_sdm___exit___obj, 4, 4, espidf_sdm___exit__);

//|     density: int
//|     """-128 (always low) to 127; the pin is high for (density + 128) / 256
//|     of the time."""
//|
//|     sample_rate: int
//|     """The pulse rate in Hz, as asked for."""
//|
//|
static mp_obj_t espidf_sdm_get_density(mp_obj_t self_in) {
    espidf_sdm_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    return MP_OBJ_NEW_SMALL_INT(self->density);
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_sdm_get_density_obj, espidf_sdm_get_density);

static mp_obj_t espidf_sdm_set_density(mp_obj_t self_in, mp_obj_t value) {
    espidf_sdm_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    mp_int_t density = mp_arg_validate_int_range(mp_obj_get_int(value), -128, 127, MP_QSTR_density);
    CHECK_ESP_RESULT(sdm_channel_set_pulse_density(self->channel, density));
    self->density = density;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(espidf_sdm_set_density_obj, espidf_sdm_set_density);

MP_PROPERTY_GETSET(espidf_sdm_density_obj,
    (mp_obj_t)&espidf_sdm_get_density_obj,
    (mp_obj_t)&espidf_sdm_set_density_obj);

static mp_obj_t espidf_sdm_get_sample_rate(mp_obj_t self_in) {
    espidf_sdm_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    return mp_obj_new_int_from_uint(self->sample_rate);
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_sdm_get_sample_rate_obj, espidf_sdm_get_sample_rate);

MP_PROPERTY_GETTER(espidf_sdm_sample_rate_obj, (mp_obj_t)&espidf_sdm_get_sample_rate_obj);

static const mp_rom_map_elem_t espidf_sdm_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&espidf_sdm_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&espidf_sdm_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&espidf_sdm___exit___obj) },
    { MP_ROM_QSTR(MP_QSTR_density), MP_ROM_PTR(&espidf_sdm_density_obj) },
    { MP_ROM_QSTR(MP_QSTR_sample_rate), MP_ROM_PTR(&espidf_sdm_sample_rate_obj) },
};
static MP_DEFINE_CONST_DICT(espidf_sdm_locals_dict, espidf_sdm_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    espidf_sdm_type,
    MP_QSTR_SigmaDelta,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, espidf_sdm_make_new,
    locals_dict, &espidf_sdm_locals_dict
    );

#endif
