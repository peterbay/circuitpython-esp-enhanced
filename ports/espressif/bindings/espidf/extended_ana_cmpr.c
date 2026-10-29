// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Petr Vavrin
//
// SPDX-License-Identifier: MIT

// espidf.Comparator; see extended.h.

#include "py/obj.h"
#include "py/objproperty.h"
#include "py/runtime.h"

#include "bindings/espidf/__init__.h"
#include "bindings/espidf/extended.h"
#include "common-hal/microcontroller/Pin.h"
#include "shared-bindings/digitalio/Pull.h"
#include "shared-bindings/util.h"
#include "shared/runtime/context_manager_helpers.h"

#include "driver/ana_cmpr.h"
#include "driver/gpio.h"
#include "soc/ana_cmpr_pins.h"

// The analog comparator: one input pin compared in hardware against a
// reference, either a fraction of the supply or a second pin, with an
// interrupt on each crossing -- a threshold watched without the ADC or the CPU.
// The pins are fixed by the silicon: on the C5 the input is GPIO9 and the
// external reference GPIO8.

#if CIRCUITPY_ESPIDF_ANA_CMPR

typedef struct {
    mp_obj_base_t base;
    ana_cmpr_handle_t unit;
    volatile uint32_t crossings;
    // The side of the reference the input last crossed to: 1 above, 0 below,
    // -1 not known yet. The C5 cannot read the comparator's output directly
    // (ana_cmpr_get_output_level says not supported), only report crossings.
    volatile int8_t level;
    uint8_t reference_tenths;
    bool external;
    digitalio_pull_t pull;
} espidf_ana_cmpr_obj_t;

// One unit on the C5.
static espidf_ana_cmpr_obj_t *_open;

static bool IRAM_ATTR on_cross(ana_cmpr_handle_t unit, const ana_cmpr_cross_event_data_t *edata, void *user_ctx) {
    espidf_ana_cmpr_obj_t *self = user_ctx;
    self->crossings++;
    if (edata->cross_type == ANA_CMPR_CROSS_POS) {
        self->level = 1;
    } else if (edata->cross_type == ANA_CMPR_CROSS_NEG) {
        self->level = 0;
    }
    return false;
}

static void ana_cmpr_close(espidf_ana_cmpr_obj_t *self) {
    if (self->unit == NULL) {
        return;
    }
    ana_cmpr_disable(self->unit);
    ana_cmpr_del_unit(self->unit);
    self->unit = NULL;
    reset_pin_number(ANA_CMPR0_SRC_GPIO);
    if (self->external) {
        reset_pin_number(ANA_CMPR0_EXT_REF_GPIO);
    }
    if (_open == self) {
        _open = NULL;
    }
}

void espidf_ana_cmpr_reset(void) {
    if (_open != NULL) {
        ana_cmpr_close(_open);
    }
}

static void check_for_deinit(espidf_ana_cmpr_obj_t *self) {
    if (self->unit == NULL) {
        raise_deinited_error();
    }
}

static uint8_t validate_reference(mp_obj_t value) {
    mp_float_t ref = mp_obj_get_float(value);
    if (!(ref >= 0 && ref <= 0.7f)) {
        mp_raise_ValueError(MP_ERROR_TEXT("reference must be 0.0-0.7"));
    }
    return (uint8_t)(ref * 10 + 0.5f);
}

// The driver configures the pad as analog, which turns its pulls off; they
// still work in that state, so the asked-for one is put back afterwards.
static void apply_pull(digitalio_pull_t pull) {
    gpio_num_t pin = ANA_CMPR0_SRC_GPIO;
    CHECK_ESP_RESULT(pull == PULL_UP ? gpio_pullup_en(pin) : gpio_pullup_dis(pin));
    CHECK_ESP_RESULT(pull == PULL_DOWN ? gpio_pulldown_en(pin) : gpio_pulldown_dis(pin));
}

//| class Comparator:
//|     """The analog comparator: an input pin compared in hardware against a
//|     reference, counting each crossing in an interrupt. The pins are fixed:
//|     on the ESP32-C5 the input is GPIO9 and the external reference GPIO8.
//|     ESP32-C5 only (the S3 has none)."""
//|
//|     def __init__(
//|         self,
//|         *,
//|         reference: float = 0.5,
//|         external_reference: bool = False,
//|         pull: Optional[digitalio.Pull] = None,
//|     ) -> None:
//|         """
//|         :param float reference: the threshold as a fraction of the supply,
//|           0.0 to 0.7 in steps of 0.1. Ignored with ``external_reference``.
//|         :param bool external_reference: compare against the voltage on the
//|           reference pin instead.
//|         :param ~digitalio.Pull pull: a weak pull on the input, e.g. for an
//|           open-collector sensor."""
//|         ...
//|
static mp_obj_t espidf_ana_cmpr_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_reference, ARG_external_reference, ARG_pull };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_reference, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = MP_OBJ_NULL } },
        { MP_QSTR_external_reference, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = false } },
        { MP_QSTR_pull, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = mp_const_none } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    uint8_t tenths = args[ARG_reference].u_obj == MP_OBJ_NULL ? 5 : validate_reference(args[ARG_reference].u_obj);
    bool external = args[ARG_external_reference].u_bool;
    digitalio_pull_t pull = validate_pull(args[ARG_pull].u_obj, MP_QSTR_pull);

    if (_open != NULL) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("Comparator in use"));
    }
    if (!pin_number_is_free(ANA_CMPR0_SRC_GPIO) || (external && !pin_number_is_free(ANA_CMPR0_EXT_REF_GPIO))) {
        mp_raise_ValueError(MP_ERROR_TEXT("Comparator pins in use"));
    }

    espidf_ana_cmpr_obj_t *self = mp_obj_malloc_with_finaliser(espidf_ana_cmpr_obj_t, &espidf_ana_cmpr_type);
    self->unit = NULL;
    self->crossings = 0;
    self->level = -1;
    self->reference_tenths = tenths;
    self->external = external;
    self->pull = pull;

    ana_cmpr_config_t config = {
        .unit = 0,
        .clk_src = ANA_CMPR_CLK_SRC_DEFAULT,
        .ref_src = external ? ANA_CMPR_REF_SRC_EXTERNAL : ANA_CMPR_REF_SRC_INTERNAL,
        .cross_type = ANA_CMPR_CROSS_ANY,
    };
    CHECK_ESP_RESULT(ana_cmpr_new_unit(&config, &self->unit));
    esp_err_t err = ESP_OK;
    if (!external) {
        ana_cmpr_internal_ref_config_t ref = { .ref_volt = ANA_CMPR_REF_VOLT_0_PCT_VDD + tenths };
        err = ana_cmpr_set_internal_reference(self->unit, &ref);
    }
    if (err == ESP_OK) {
        ana_cmpr_event_callbacks_t callbacks = { .on_cross = on_cross };
        err = ana_cmpr_register_event_callbacks(self->unit, &callbacks, self);
    }
    if (err == ESP_OK) {
        err = ana_cmpr_enable(self->unit);
    }
    if (err != ESP_OK) {
        ana_cmpr_del_unit(self->unit);
        self->unit = NULL;
        raise_esp_error(err);
    }
    claim_pin_number(ANA_CMPR0_SRC_GPIO);
    if (external) {
        claim_pin_number(ANA_CMPR0_EXT_REF_GPIO);
    }
    _open = self;
    apply_pull(pull);
    return MP_OBJ_FROM_PTR(self);
}

//|     def deinit(self) -> None:
//|         """Stop comparing and release the pins."""
//|         ...
//|
static mp_obj_t espidf_ana_cmpr_deinit(mp_obj_t self_in) {
    ana_cmpr_close(MP_OBJ_TO_PTR(self_in));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_ana_cmpr_deinit_obj, espidf_ana_cmpr_deinit);

//|     def __enter__(self) -> Comparator:
//|         """No-op used by Context Managers."""
//|         ...
//|
//|     def __exit__(self) -> None:
//|         """Automatically deinitializes when exiting a context."""
//|         ...
//|
static mp_obj_t espidf_ana_cmpr___exit__(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    return espidf_ana_cmpr_deinit(args[0]);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(espidf_ana_cmpr___exit___obj, 4, 4, espidf_ana_cmpr___exit__);

//|     value: Optional[bool]
//|     """True if the input last crossed the reference upwards, False if
//|     downwards, None until it has crossed at all. The C5 reports crossings
//|     but cannot be asked for the comparator's current output, so a level
//|     that has not changed since the comparator started is not known."""
//|
static mp_obj_t espidf_ana_cmpr_get_value(mp_obj_t self_in) {
    espidf_ana_cmpr_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    int8_t level = self->level;
    return level < 0 ? mp_const_none : mp_obj_new_bool(level);
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_ana_cmpr_get_value_obj, espidf_ana_cmpr_get_value);
MP_PROPERTY_GETTER(espidf_ana_cmpr_value_obj, (mp_obj_t)&espidf_ana_cmpr_get_value_obj);

//|     crossings: int
//|     """How many times the input has crossed the reference, either way, since
//|     the comparator started or this was last set (only to 0)."""
//|
static mp_obj_t espidf_ana_cmpr_get_crossings(mp_obj_t self_in) {
    espidf_ana_cmpr_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    return mp_obj_new_int_from_uint(self->crossings);
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_ana_cmpr_get_crossings_obj, espidf_ana_cmpr_get_crossings);

static mp_obj_t espidf_ana_cmpr_set_crossings(mp_obj_t self_in, mp_obj_t value) {
    espidf_ana_cmpr_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    mp_arg_validate_int_range(mp_obj_get_int(value), 0, 0, MP_QSTR_crossings);
    self->crossings = 0;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(espidf_ana_cmpr_set_crossings_obj, espidf_ana_cmpr_set_crossings);

MP_PROPERTY_GETSET(espidf_ana_cmpr_crossings_obj,
    (mp_obj_t)&espidf_ana_cmpr_get_crossings_obj,
    (mp_obj_t)&espidf_ana_cmpr_set_crossings_obj);

//|     reference: Optional[float]
//|     """The internal threshold as a fraction of the supply, in steps of 0.1;
//|     None with an external reference."""
//|
static mp_obj_t espidf_ana_cmpr_get_reference(mp_obj_t self_in) {
    espidf_ana_cmpr_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    return self->external ? mp_const_none : mp_obj_new_float(self->reference_tenths / 10.0f);
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_ana_cmpr_get_reference_obj, espidf_ana_cmpr_get_reference);

static mp_obj_t espidf_ana_cmpr_set_reference(mp_obj_t self_in, mp_obj_t value) {
    espidf_ana_cmpr_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    if (self->external) {
        mp_raise_ValueError(MP_ERROR_TEXT("Comparator uses an external reference"));
    }
    uint8_t tenths = validate_reference(value);
    ana_cmpr_internal_ref_config_t ref = { .ref_volt = ANA_CMPR_REF_VOLT_0_PCT_VDD + tenths };
    CHECK_ESP_RESULT(ana_cmpr_set_internal_reference(self->unit, &ref));
    self->reference_tenths = tenths;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(espidf_ana_cmpr_set_reference_obj, espidf_ana_cmpr_set_reference);

MP_PROPERTY_GETSET(espidf_ana_cmpr_reference_obj,
    (mp_obj_t)&espidf_ana_cmpr_get_reference_obj,
    (mp_obj_t)&espidf_ana_cmpr_set_reference_obj);

//|     pull: Optional[digitalio.Pull]
//|     """The weak pull on the input."""
//|
//|
static mp_obj_t espidf_ana_cmpr_get_pull(mp_obj_t self_in) {
    espidf_ana_cmpr_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    return self->pull == PULL_UP ? MP_OBJ_FROM_PTR(&digitalio_pull_up_obj) :
           self->pull == PULL_DOWN ? MP_OBJ_FROM_PTR(&digitalio_pull_down_obj) : mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_ana_cmpr_get_pull_obj, espidf_ana_cmpr_get_pull);

static mp_obj_t espidf_ana_cmpr_set_pull(mp_obj_t self_in, mp_obj_t value) {
    espidf_ana_cmpr_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    digitalio_pull_t pull = validate_pull(value, MP_QSTR_pull);
    apply_pull(pull);
    self->pull = pull;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(espidf_ana_cmpr_set_pull_obj, espidf_ana_cmpr_set_pull);

MP_PROPERTY_GETSET(espidf_ana_cmpr_pull_obj,
    (mp_obj_t)&espidf_ana_cmpr_get_pull_obj,
    (mp_obj_t)&espidf_ana_cmpr_set_pull_obj);

static const mp_rom_map_elem_t espidf_ana_cmpr_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&espidf_ana_cmpr_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&espidf_ana_cmpr_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&espidf_ana_cmpr___exit___obj) },
    { MP_ROM_QSTR(MP_QSTR_value), MP_ROM_PTR(&espidf_ana_cmpr_value_obj) },
    { MP_ROM_QSTR(MP_QSTR_crossings), MP_ROM_PTR(&espidf_ana_cmpr_crossings_obj) },
    { MP_ROM_QSTR(MP_QSTR_reference), MP_ROM_PTR(&espidf_ana_cmpr_reference_obj) },
    { MP_ROM_QSTR(MP_QSTR_pull), MP_ROM_PTR(&espidf_ana_cmpr_pull_obj) },
};
static MP_DEFINE_CONST_DICT(espidf_ana_cmpr_locals_dict, espidf_ana_cmpr_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    espidf_ana_cmpr_type,
    MP_QSTR_Comparator,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, espidf_ana_cmpr_make_new,
    locals_dict, &espidf_ana_cmpr_locals_dict
    );

#endif
