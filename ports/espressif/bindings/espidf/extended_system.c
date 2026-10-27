// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Petr Vavrin
//
// SPDX-License-Identifier: MIT

// Timing, heap, pin and power functions for the espidf module; see extended.h.

#include <string.h>

#include "py/obj.h"
#include "py/runtime.h"

#include "bindings/espidf/__init__.h"
#include "bindings/espidf/extended.h"
#include "common-hal/microcontroller/Pin.h"

#include "driver/gpio.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

//| def cycles() -> int:
//|     """Return the CPU's cycle counter.
//|
//|     One count is one CPU clock period, so at 240 MHz the resolution is about
//|     4 ns. `time.monotonic_ns()` is built from the supervisor tick, which is
//|     1/32768 of a second, so it cannot resolve anything shorter than 30.5 us;
//|     this can. Use it to measure a single operation instead of a loop of
//|     several thousand.
//|
//|     The counter is 32 bits and wraps about every 17.9 seconds at 240 MHz, so
//|     mask the difference::
//|
//|         import espidf, microcontroller
//|         start = espidf.cycles()
//|         work()
//|         elapsed = (espidf.cycles() - start) & 0xFFFFFFFF
//|         seconds = elapsed / microcontroller.cpu.frequency
//|
//|     Reading it from Python costs roughly 1500 cycles, and an empty timed
//|     region roughly 1100, so subtract an empty measurement taken the same way.
//|     Below about a hundred cycles nothing can be seen from Python.
//|
//|     The count is per core, and it keeps running while other tasks do, so it
//|     measures elapsed time on the CPU rather than time spent in the code
//|     being timed."""
//|     ...
//|
//|

static mp_obj_t espidf_cycles(void) {
    return mp_obj_new_int_from_uint(esp_cpu_get_cycle_count());
}
MP_DEFINE_CONST_FUN_OBJ_0(espidf_cycles_obj, espidf_cycles);

//| def get_time_us() -> int:
//|     """Microseconds since boot, from the same timer the port already uses for
//|     scheduling. Unlike ``time.monotonic_ns()``, whose resolution here is one
//|     32768 Hz tick (about 30.5 us), this is good to a microsecond, which is what
//|     makes it usable for timing individual operations."""
//|
//|
static mp_obj_t espidf_get_time_us(void) {
    return mp_obj_new_int_from_ll(esp_timer_get_time());
}
MP_DEFINE_CONST_FUN_OBJ_0(espidf_get_time_us_obj, espidf_get_time_us);

//| def task_stats() -> str:
//|     """FreeRTOS run time statistics: one line per task with its share of the
//|     CPU since boot. Empty when the firmware was built without run time stats."""
//|
//|
static mp_obj_t espidf_task_stats(void) {
    #if defined(CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS) && defined(CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS)
    // 48 bytes per task is what the formatter needs for the longest line it writes.
    const size_t size = 48 * (uxTaskGetNumberOfTasks() + 2);
    char *buf = m_malloc(size);
    buf[0] = '\0';
    vTaskGetRunTimeStats(buf);
    mp_obj_t result = mp_obj_new_str(buf, strlen(buf));
    m_free(buf);
    return result;
    #else
    return MP_OBJ_NEW_QSTR(MP_QSTR_);
    #endif
}
MP_DEFINE_CONST_FUN_OBJ_0(espidf_task_stats_obj, espidf_task_stats);

// check the ESP-IDF heap for corruption.
//| def check_heap() -> bool:
//|     """Walk the whole ESP-IDF heap and return True if every block is intact.
//|     A write past the end of an allocation shows up here right away, instead of
//|     as a hard fault somewhere unrelated later."""
//|     ...
//|
//|
static mp_obj_t espidf_check_heap(void) {
    return mp_obj_new_bool(heap_caps_check_integrity_all(true));
}
MP_DEFINE_CONST_FUN_OBJ_0(espidf_check_heap_obj, espidf_check_heap);

// report a pin's actual hardware configuration and whether
// CircuitPython has it claimed.
//| def pin_status(number: int) -> dict:
//|     """Read what a GPIO is actually doing, straight from the pin's registers.
//|
//|     The keys are ``number``, ``free`` (True when CircuitPython has not claimed
//|     it), ``input``, ``output``, ``pull_up``, ``pull_down``, ``level`` (the input
//|     register; meaningful only when ``input`` is True, because a pure output has
//|     its input buffer disabled and then this reads 0 whatever the pad is driving),
//|     ``drive`` (strength 0..3), ``function`` (the IOMUX function
//|     index; a plain GPIO is a fixed value, anything else means a peripheral has
//|     it) and ``out_signal`` (the peripheral signal routed to it, or a sentinel
//|     when it is a plain GPIO).
//|
//|     ``free`` says whether CircuitPython's own allocator considers the pin
//|     available; the rest says what the silicon is set to regardless of who did
//|     it. CircuitPython does not record which object claimed a pin, so this cannot
//|     name the culprit, but the function and out_signal usually make it obvious
//|     which peripheral took it."""
//|     ...
//|
//|
static mp_obj_t espidf_pin_status(mp_obj_t number_in) {
    mp_int_t number = mp_obj_get_int(number_in);
    if (number < 0 || number >= GPIO_NUM_MAX) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid pin number"));
    }
    gpio_io_config_t cfg;
    esp_err_t err = gpio_get_io_config(number, &cfg);
    if (err == ESP_ERR_INVALID_ARG) {
        // A number inside the range that is not a usable GPIO on this chip
        // (reserved, or a flash/PSRAM pin).
        mp_raise_ValueError(MP_ERROR_TEXT("not a usable GPIO on this chip"));
    }
    CHECK_ESP_RESULT(err);

    mp_obj_t d = mp_obj_new_dict(11);
    mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_number), MP_OBJ_NEW_SMALL_INT(number));
    mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_free),
        mp_obj_new_bool(pin_number_is_free(number)));
    mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_input), mp_obj_new_bool(cfg.ie));
    mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_output), mp_obj_new_bool(cfg.oe));
    mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_pull_up), mp_obj_new_bool(cfg.pu));
    mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_pull_down), mp_obj_new_bool(cfg.pd));
    mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_level),
        MP_OBJ_NEW_SMALL_INT(gpio_get_level(number)));
    mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_drive), MP_OBJ_NEW_SMALL_INT(cfg.drv));
    mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_function), mp_obj_new_int_from_uint(cfg.fun_sel));
    mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_out_signal), mp_obj_new_int_from_uint(cfg.sig_out));
    mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_sleep_mode), mp_obj_new_bool(cfg.slp_sel));
    return d;
}
MP_DEFINE_CONST_FUN_OBJ_1(espidf_pin_status_obj, espidf_pin_status);

// automatic frequency scaling and light sleep.
//| def power_management(
//|     min_frequency: int | None = None,
//|     max_frequency: int | None = None,
//|     light_sleep: bool = False,
//| ) -> Tuple[int, int, bool]:
//|     """Configure dynamic frequency scaling and return the settings in effect as
//|     ``(min_frequency, max_frequency, light_sleep)``, in MHz. Called without
//|     arguments it only reports.
//|
//|     The clock drops to ``min_frequency`` whenever nothing holds a power
//|     management lock, which is what makes this worth having on battery. With
//|     ``light_sleep`` the idle task stops the CPU altogether until the next
//|     interrupt.
//|
//|     Light sleep drops the USB connection, so a board being developed over USB
//|     should leave it off."""
//|     ...
//|
//|
static mp_obj_t espidf_power_management(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_min_frequency, ARG_max_frequency, ARG_light_sleep };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_min_frequency, MP_ARG_OBJ, { .u_obj = mp_const_none } },
        { MP_QSTR_max_frequency, MP_ARG_OBJ, { .u_obj = mp_const_none } },
        { MP_QSTR_light_sleep, MP_ARG_OBJ, { .u_obj = mp_const_none } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    esp_pm_config_t config;
    CHECK_ESP_RESULT(esp_pm_get_configuration(&config));

    bool change = false;
    if (args[ARG_min_frequency].u_obj != mp_const_none) {
        config.min_freq_mhz = mp_obj_get_int(args[ARG_min_frequency].u_obj);
        change = true;
    }
    if (args[ARG_max_frequency].u_obj != mp_const_none) {
        config.max_freq_mhz = mp_obj_get_int(args[ARG_max_frequency].u_obj);
        change = true;
    }
    if (args[ARG_light_sleep].u_obj != mp_const_none) {
        config.light_sleep_enable = mp_obj_is_true(args[ARG_light_sleep].u_obj);
        change = true;
    }
    if (change) {
        CHECK_ESP_RESULT(esp_pm_configure(&config));
        CHECK_ESP_RESULT(esp_pm_get_configuration(&config));
    }

    mp_obj_t out[3] = {
        MP_OBJ_NEW_SMALL_INT(config.min_freq_mhz),
        MP_OBJ_NEW_SMALL_INT(config.max_freq_mhz),
        mp_obj_new_bool(config.light_sleep_enable),
    };
    return mp_obj_new_tuple(3, out);
}
MP_DEFINE_CONST_FUN_OBJ_KW(espidf_power_management_obj, 0, espidf_power_management);
