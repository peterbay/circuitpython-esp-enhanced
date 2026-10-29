// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Petr Vavrin
//
// SPDX-License-Identifier: MIT

// espidf.LPCore; see extended.h.

#include "py/obj.h"
#include "py/objarray.h"
#include "py/objproperty.h"
#include "py/runtime.h"

#include "bindings/espidf/__init__.h"
#include "bindings/espidf/extended.h"
#include "shared-bindings/util.h"
#include "shared/runtime/context_manager_helpers.h"

#include "supervisor/port.h"

// The headers below exist only on chips that have the peripheral.
#if CIRCUITPY_ESPIDF_LPCORE

#include "esp_sleep.h"
#include "esp_timer.h"
#include "hal/pmu_ll.h"
#include "soc/soc.h"
#include "ulp_lp_core.h"

// The LP core: a small RISC-V core in the low-power domain that keeps running
// while the main cores sleep, woken by the LP timer. Its program is built with
// the ESP-IDF (an LP core app embedded with ulp_embed_binary()) and handed
// over as a binary; it shares variables with Python through LP RAM, whose
// first CONFIG_ULP_COPROC_RESERVE_MEM bytes hold the program and its data.
// This is espulp's job on the S2 and S3, whose ULPs this chip does not have;
// a class of its own keeps the upstream module untouched.

typedef struct {
    mp_obj_base_t base;
    bool loaded;
    bool running;
} espidf_lpcore_obj_t;

static espidf_lpcore_obj_t *_lpcore;

static void lpcore_close(espidf_lpcore_obj_t *self) {
    if (!self->loaded) {
        return;
    }
    // Also when this object did not start it: one attached after a reload.
    ulp_lp_core_stop();
    self->running = false;
    self->loaded = false;
    if (_lpcore == self) {
        _lpcore = NULL;
    }
}

// Leaves the LP core running, like espulp does with the S2's and S3's ULP:
// carrying on while CircuitPython is not -- through a reload, or in deep
// sleep -- is the point of it. Only deinit() and stop() stop it, and a new
// program loaded over it.
void espidf_lpcore_reset(void) {
    _lpcore = NULL;
}

static void check_for_deinit(espidf_lpcore_obj_t *self) {
    if (!self->loaded) {
        raise_deinited_error();
    }
}

//| class LPCore:
//|     """The LP core, a small RISC-V core that runs while the main cores
//|     sleep. ESP32-C5 only here.
//|
//|     The program is an LP core app built with the ESP-IDF (``ulp_embed_binary``
//|     in a component's CMakeLists.txt; the binary is
//|     ``build/esp-idf/main/lp_core_main/lp_core_main.bin``), and must have been
//|     built with the same ``CONFIG_ULP_COPROC_RESERVE_MEM`` as this firmware.
//|     Its global variables are in `memory`, at the addresses the build lists in
//|     ``lp_core_main.ld`` less `address`.
//|
//|     A running program carries on when the Python program ends, through a
//|     reload and in deep sleep, as espulp's ULP does on the S2 and S3; only
//|     `stop`, `deinit` (also at the end of a ``with`` block) or loading a new
//|     program stop it."""
//|
//|     address: int
//|     """Where LP RAM, and so `memory`, starts."""
//|
//|     def __init__(self, program: Optional[ReadableBuffer] = None) -> None:
//|         """Load ``program`` into LP RAM; it does not run until `run`. With no
//|         program, attach to what is already there, e.g. a program still
//|         running from before a deep sleep, to read its memory."""
//|         ...
//|
static mp_obj_t espidf_lpcore_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    mp_arg_check_num(n_args, n_kw, 0, 1, false);
    mp_buffer_info_t program = { .len = 0 };
    if (n_args == 1 && all_args[0] != mp_const_none) {
        mp_get_buffer_raise(all_args[0], &program, MP_BUFFER_READ);
        mp_arg_validate_length_range(program.len, 1, CONFIG_ULP_COPROC_RESERVE_MEM, MP_QSTR_program);
    }
    if (_lpcore != NULL) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("LPCore in use"));
    }
    // No finaliser: collecting the object must not stop the core.
    espidf_lpcore_obj_t *self = mp_obj_malloc(espidf_lpcore_obj_t, &espidf_lpcore_type);
    self->loaded = false;
    self->running = false;
    if (program.len) {
        CHECK_ESP_RESULT(ulp_lp_core_load_binary(program.buf, program.len));
    }
    self->loaded = true;
    _lpcore = self;
    return MP_OBJ_FROM_PTR(self);
}

//|     def run(self, *, period_us: Optional[int] = None) -> None:
//|         """Start the program: once, or with ``period_us`` again every that
//|         many microseconds, woken by the LP timer, until `stop`. Each run
//|         starts at ``main()``; globals keep their values between runs."""
//|         ...
//|
static mp_obj_t espidf_lpcore_run(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_self, ARG_period_us };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_self, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_period_us, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = mp_const_none } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    espidf_lpcore_obj_t *self = MP_OBJ_TO_PTR(args[ARG_self].u_obj);
    check_for_deinit(self);

    ulp_lp_core_cfg_t cfg = { .wakeup_source = ULP_LP_CORE_WAKEUP_SOURCE_HP_CPU };
    if (args[ARG_period_us].u_obj != mp_const_none) {
        cfg.wakeup_source = ULP_LP_CORE_WAKEUP_SOURCE_LP_TIMER;
        cfg.lp_timer_sleep_duration_us =
            mp_arg_validate_int_min(mp_obj_get_int(args[ARG_period_us].u_obj), 1, MP_QSTR_period_us);
    }
    if (self->running) {
        ulp_lp_core_stop();
    }
    CHECK_ESP_RESULT(ulp_lp_core_run(&cfg));
    self->running = true;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(espidf_lpcore_run_obj, 1, espidf_lpcore_run);

//|     def stop(self) -> None:
//|         """Stop the LP core and its timer wake-ups."""
//|         ...
//|
static mp_obj_t espidf_lpcore_stop(mp_obj_t self_in) {
    espidf_lpcore_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    ulp_lp_core_stop();
    self->running = false;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_lpcore_stop_obj, espidf_lpcore_stop);

//|     memory: memoryview
//|     """The reserved part of LP RAM, holding the program and its globals,
//|     read- and writeable. Use struct.unpack_from / pack_into at an offset of
//|     the variable's address minus `address`."""
//|
//|
static mp_obj_t espidf_lpcore_get_memory(mp_obj_t self_in) {
    espidf_lpcore_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    return mp_obj_new_memoryview('B' | MP_OBJ_ARRAY_TYPECODE_FLAG_RW, CONFIG_ULP_COPROC_RESERVE_MEM,
        (void *)SOC_RTC_DRAM_LOW);
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_lpcore_get_memory_obj, espidf_lpcore_get_memory);
MP_PROPERTY_GETTER(espidf_lpcore_memory_obj, (mp_obj_t)&espidf_lpcore_get_memory_obj);

// A property rather than a constant in the table: 0x50000000 does not fit a
// small int, which is all MP_ROM_INT can hold.
static mp_obj_t espidf_lpcore_get_address(mp_obj_t self_in) {
    return mp_obj_new_int_from_uint(SOC_RTC_DRAM_LOW);
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_lpcore_get_address_obj, espidf_lpcore_get_address);
MP_PROPERTY_GETTER(espidf_lpcore_address_obj, (mp_obj_t)&espidf_lpcore_get_address_obj);

static mp_obj_t espidf_lpcore_deinit(mp_obj_t self_in) {
    lpcore_close(MP_OBJ_TO_PTR(self_in));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_lpcore_deinit_obj, espidf_lpcore_deinit);

static mp_obj_t espidf_lpcore___exit__(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    return espidf_lpcore_deinit(args[0]);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(espidf_lpcore___exit___obj, 4, 4, espidf_lpcore___exit__);

static const mp_rom_map_elem_t espidf_lpcore_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_run), MP_ROM_PTR(&espidf_lpcore_run_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&espidf_lpcore_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_memory), MP_ROM_PTR(&espidf_lpcore_memory_obj) },
    { MP_ROM_QSTR(MP_QSTR_address), MP_ROM_PTR(&espidf_lpcore_address_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&espidf_lpcore_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&espidf_lpcore___exit___obj) },
};
static MP_DEFINE_CONST_DICT(espidf_lpcore_locals_dict, espidf_lpcore_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    espidf_lpcore_type,
    MP_QSTR_LPCore,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, espidf_lpcore_make_new,
    locals_dict, &espidf_lpcore_locals_dict
    );

// --- LPCoreAlarm -------------------------------------------------------------
//
// Hooked into the alarm module the way espulp's ULPAlarm is. In deep sleep the
// LP core program wakes the chip with ulp_lp_core_wakeup_main_processor(), with
// the ULP wake-up source enabled here. Awake -- light sleep, and the deep sleep
// CircuitPython only pretends while USB is connected -- that call does
// nothing, so the program also raises the LP-to-HP software interrupt with
// ulp_lp_core_sw_intr_to_hp_trigger(), and a timer polls its raw status and
// wakes the main task: the PMU interrupt line is the IDF's own.

typedef struct {
    mp_obj_base_t base;
} espidf_lpcorealarm_obj_t;

// What alarm.wake_alarm holds after a wake-up: outside the heap, which is new
// after a deep sleep.
static const espidf_lpcorealarm_obj_t _wake_alarm = { { &espidf_lpcorealarm_type } };

static bool _alarm_set;
static volatile bool _woke;
static esp_timer_handle_t _poll_timer;

#define LPCORE_ALARM_POLL_US (5000)

static void poll_sw_intr(void *arg) {
    (void)arg;
    if (PMU.hp_ext.int_raw.sw) {
        pmu_ll_hp_clear_sw_intr_status(&PMU);
        _woke = true;
        port_wake_main_task();
    }
}

static void stop_polling(void) {
    if (_poll_timer != NULL) {
        esp_timer_stop(_poll_timer);
        esp_timer_delete(_poll_timer);
        _poll_timer = NULL;
    }
}

void espidf_lpcorealarm_set_alarm(bool deep_sleep, size_t n_alarms, const mp_obj_t *alarms) {
    (void)deep_sleep;
    bool found = false;
    for (size_t i = 0; i < n_alarms; i++) {
        if (mp_obj_is_type(alarms[i], &espidf_lpcorealarm_type)) {
            if (found) {
                mp_raise_ValueError_varg(MP_ERROR_TEXT("Only one %q can be set"), MP_QSTR_LPCoreAlarm);
            }
            found = true;
        }
    }
    if (!found) {
        return;
    }
    // An interrupt raised before the alarm was set must not count.
    pmu_ll_hp_clear_sw_intr_status(&PMU);
    _woke = false;
    // Also for deep sleep: with USB connected CircuitPython only pretends, and
    // stays awake. A real deep sleep stops the timer in prepare_for_deep_sleep.
    if (_poll_timer == NULL) {
        esp_timer_create_args_t args = { .callback = poll_sw_intr, .name = "lpcore_alarm" };
        CHECK_ESP_RESULT(esp_timer_create(&args, &_poll_timer));
        CHECK_ESP_RESULT(esp_timer_start_periodic(_poll_timer, LPCORE_ALARM_POLL_US));
    }
    _alarm_set = true;
}

void espidf_lpcorealarm_prepare_for_deep_sleep(void) {
    if (!_alarm_set) {
        return;
    }
    stop_polling();
    esp_sleep_enable_ulp_wakeup();
}

bool espidf_lpcorealarm_woke_this_cycle(void) {
    return _woke;
}

void espidf_lpcorealarm_reset(void) {
    stop_polling();
    _woke = false;
    _alarm_set = false;
}

mp_obj_t espidf_lpcorealarm_record_wake_alarm(void) {
    return MP_OBJ_FROM_PTR(&_wake_alarm);
}

mp_obj_t espidf_lpcorealarm_find_triggered_alarm(size_t n_alarms, const mp_obj_t *alarms) {
    for (size_t i = 0; i < n_alarms; i++) {
        if (mp_obj_is_type(alarms[i], &espidf_lpcorealarm_type)) {
            return alarms[i];
        }
    }
    return mp_const_none;
}

//| class LPCoreAlarm:
//|     """An alarm the LP core program raises, for `alarm.light_sleep_until_alarms`
//|     and `alarm.exit_and_deep_sleep_until_alarms`. The program wakes the main
//|     cores with both ``ulp_lp_core_wakeup_main_processor()`` (from deep
//|     sleep) and ``ulp_lp_core_sw_intr_to_hp_trigger()`` (while they are
//|     awake, as in light sleep or the deep sleep CircuitPython pretends while
//|     USB is connected). ESP32-C5 only."""
//|
//|     def __init__(self) -> None:
//|         """Create an alarm for the LP core; the program is `LPCore`'s."""
//|         ...
//|
//|
static mp_obj_t espidf_lpcorealarm_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    mp_arg_check_num(n_args, n_kw, 0, 0, false);
    return mp_obj_malloc(espidf_lpcorealarm_obj_t, &espidf_lpcorealarm_type);
}

MP_DEFINE_CONST_OBJ_TYPE(
    espidf_lpcorealarm_type,
    MP_QSTR_LPCoreAlarm,
    MP_TYPE_FLAG_NONE,
    make_new, espidf_lpcorealarm_make_new
    );

#endif
