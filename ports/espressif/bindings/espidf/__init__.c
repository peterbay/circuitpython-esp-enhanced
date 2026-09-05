// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2020 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include "py/obj.h"
#include "py/runtime.h"
#include "py/mphal.h"


#include "bindings/espidf/__init__.h"

#include "nvs_flash.h"
#include "components/heap/include/esp_heap_caps.h"
#include <math.h>
#include <setjmp.h>
#include <string.h>
#include "esp_cpu.h"
#include "esp_timer.h"
#include "esp_pm.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "supervisor/partition_disk.h"
#include "py/mperrno.h"
#include "spi_flash_mmap.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "esp_event.h"
#if CIRCUITPY_WIFI
#include "esp_eap_client.h"
#include "esp_smartconfig.h"
#include "esp_private/wifi.h"
#endif
#include "esp_netif.h"
#include "freertos/queue.h"
#include "supervisor/port.h"
#include "soc/soc_caps.h"
#include "mbedtls/md.h"
#include "psa/crypto.h"
#include "driver/gpio.h"
#include "common-hal/microcontroller/Pin.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "supervisor/prof.h"
#include "supervisor/background_callback.h"
#include "py/objlist.h"
#include "py/objproperty.h"
#if CIRCUITPY_BLEIO_NATIVE
#include "host/ble_gap.h"
#endif

//| import builtins
//|
//| """Direct access to a few ESP-IDF details. This module *should not* include any functionality
//|    that could be implemented by other frameworks. It should only include ESP-IDF specific
//|    things."""
//|
//|

//| def heap_caps_get_total_size() -> int:
//|     """Return the total size of the ESP-IDF, which includes the CircuitPython heap."""
//|     ...
//|
//|

static mp_obj_t espidf_heap_caps_get_total_size(void) {
    return MP_OBJ_NEW_SMALL_INT(heap_caps_get_total_size(MALLOC_CAP_8BIT));
}
MP_DEFINE_CONST_FUN_OBJ_0(espidf_heap_caps_get_total_size_obj, espidf_heap_caps_get_total_size);

//| def heap_caps_get_free_size() -> int:
//|     """Return total free memory in the ESP-IDF heap."""
//|     ...
//|
//|

static mp_obj_t espidf_heap_caps_get_free_size(void) {
    return MP_OBJ_NEW_SMALL_INT(heap_caps_get_free_size(MALLOC_CAP_8BIT));
}
MP_DEFINE_CONST_FUN_OBJ_0(espidf_heap_caps_get_free_size_obj, espidf_heap_caps_get_free_size);

//| def heap_caps_get_largest_free_block() -> int:
//|     """Return the size of largest free memory block in the ESP-IDF heap."""
//|     ...
//|
//|

static mp_obj_t espidf_heap_caps_get_largest_free_block(void) {
    return MP_OBJ_NEW_SMALL_INT(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}
MP_DEFINE_CONST_FUN_OBJ_0(espidf_heap_caps_get_largest_free_block_obj, espidf_heap_caps_get_largest_free_block);

//| def erase_nvs() -> None:
//|     """Erase all data in the non-volatile storage (nvs), including data stored by with `microcontroller.nvm`
//|
//|     This is necessary when upgrading from CircuitPython 6.3.0 or earlier to CircuitPython 7.0.0, because the
//|     layout of data in nvs has changed. The old data will be lost when you perform this operation.
//|     """
//|
//|
static mp_obj_t espidf_erase_nvs(void) {
    ESP_ERROR_CHECK(nvs_flash_deinit());
    ESP_ERROR_CHECK(nvs_flash_erase());
    ESP_ERROR_CHECK(nvs_flash_init());
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(espidf_erase_nvs_obj, espidf_erase_nvs);


static void espidf_exception_print(const mp_print_t *print, mp_obj_t o_in, mp_print_kind_t kind) {
    mp_print_kind_t k = kind & ~PRINT_EXC_SUBCLASS;
    bool is_subclass = kind & PRINT_EXC_SUBCLASS;
    if (!is_subclass && (k == PRINT_EXC)) {
        mp_print_str(print, qstr_str(MP_OBJ_QSTR_VALUE(MP_ROM_QSTR(MP_QSTR_espidf))));
        mp_print_str(print, ".");
    }
    mp_obj_exception_print(print, o_in, kind);
}

//| class IDFError(builtins.OSError):
//|     """Raised when an ``ESP-IDF`` function returns an error code.
//|     `esp_err_t <https://docs.espressif.com/projects/esp-idf/en/release-v4.4/esp32/api-reference/error-codes.html>`_
//|     """
//|
//|     ...
//|
//|
MP_DEFINE_CONST_OBJ_TYPE(
    mp_type_espidf_IDFError,
    MP_QSTR_IDFError,
    MP_TYPE_FLAG_NONE,
    print, espidf_exception_print,
    make_new, mp_obj_exception_make_new,
    attr, mp_obj_exception_attr,
    parent, &mp_type_OSError
    );

//| class MemoryError(builtins.MemoryError):
//|     """Raised when an ``ESP-IDF`` memory allocation fails."""
//|
//|     ...
//|
//|
MP_NORETURN void mp_raise_espidf_MemoryError(void) {
    nlr_raise(mp_obj_new_exception(&mp_type_espidf_MemoryError));
}

MP_DEFINE_CONST_OBJ_TYPE(
    mp_type_espidf_MemoryError,
    MP_QSTR_MemoryError,
    MP_TYPE_FLAG_NONE,
    print, espidf_exception_print,
    make_new, mp_obj_exception_make_new,
    attr, mp_obj_exception_attr,
    parent, &mp_type_MemoryError
    );

//| def get_total_psram() -> int:
//|     """Returns the number of bytes of psram detected, or 0 if psram is not present or not configured"""
//|
//|
static mp_obj_t espidf_get_total_psram(void) {
    return MP_OBJ_NEW_SMALL_INT(common_hal_espidf_get_total_psram());
}
MP_DEFINE_CONST_FUN_OBJ_0(espidf_get_total_psram_obj, espidf_get_total_psram);

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

//| def get_cycle_count() -> int:
//|     """Raw CPU cycle counter. At 240 MHz one count is 4.17 ns and the counter
//|     wraps every 17.9 seconds, so only use differences of two readings taken
//|     close together. The count follows the actual clock, so pin the frequency
//|     before relying on it."""
//|
//|
static mp_obj_t espidf_get_cycle_count(void) {
    return mp_obj_new_int_from_uint(esp_cpu_get_cycle_count());
}
MP_DEFINE_CONST_FUN_OBJ_0(espidf_get_cycle_count_obj, espidf_get_cycle_count);

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
    return mp_const_empty_bytes;
    #endif
}
MP_DEFINE_CONST_FUN_OBJ_0(espidf_task_stats_obj, espidf_task_stats);

//| def prof_stats() -> dict:
//|     """Cycle counting probes as ``{name: (cycles, calls)}``. Empty unless the
//|     firmware was built with ``CIRCUITPY_PROF=1``."""
//|
//|
static mp_obj_t espidf_prof_stats(void) {
    mp_obj_t result = mp_obj_new_dict(0);
    #if CIRCUITPY_PROF
    for (size_t i = 0; i < PROF_COUNT; i++) {
        mp_obj_t pair[2] = {
            mp_obj_new_int_from_ull(prof_cycles[i]),
            mp_obj_new_int_from_uint(prof_calls[i]),
        };
        mp_obj_dict_store(result,
            mp_obj_new_str(prof_names[i], strlen(prof_names[i])),
            mp_obj_new_tuple(2, pair));
    }
    #endif
    return result;
}
MP_DEFINE_CONST_FUN_OBJ_0(espidf_prof_stats_obj, espidf_prof_stats);

//| def prof_reset() -> None:
//|     """Zero the probe counters."""
//|
//|
static mp_obj_t espidf_prof_reset(void) {
    #if CIRCUITPY_PROF
    prof_reset();
    #endif
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(espidf_prof_reset_obj, espidf_prof_reset);

//| def profiler_start(hz: int = 2000) -> bool:
//|     """Start sampling the program counter of the calling task."""
//|
//|
static mp_obj_t espidf_profiler_start(size_t n_args, const mp_obj_t *args) {
    #if CIRCUITPY_PROF
    uint32_t hz = n_args > 0 ? mp_obj_get_int(args[0]) : 2000;
    return mp_obj_new_bool(prof_sampler_start(hz));
    #else
    return mp_const_false;
    #endif
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(espidf_profiler_start_obj, 0, 1, espidf_profiler_start);

//| def profiler_stop() -> None:
//|     """Stop sampling."""
//|
//|
static mp_obj_t espidf_profiler_stop(void) {
    #if CIRCUITPY_PROF
    prof_sampler_stop();
    #endif
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(espidf_profiler_stop_obj, espidf_profiler_stop);

//| def profiler_data() -> tuple:
//|     """``(samples, waiting, lost)`` where samples is a bytes of little endian
//|     program counters, waiting counts the ticks the task was blocked rather than
//|     running, and lost counts ticks that could not be attributed."""
//|
//|
static mp_obj_t espidf_profiler_data(void) {
    #if CIRCUITPY_PROF
    size_t count, waiting, lost;
    const uint32_t *data = prof_sampler_data(&count, &waiting, &lost);
    mp_obj_t items[3] = {
        mp_obj_new_bytes((const byte *)data, count * 2 * sizeof(uint32_t)),
        mp_obj_new_int_from_uint(waiting),
        mp_obj_new_int_from_uint(lost),
    };
    return mp_obj_new_tuple(3, items);
    #else
    mp_obj_t items[3] = { mp_const_empty_bytes, MP_OBJ_NEW_SMALL_INT(0), MP_OBJ_NEW_SMALL_INT(0) };
    return mp_obj_new_tuple(3, items);
    #endif
}
MP_DEFINE_CONST_FUN_OBJ_0(espidf_profiler_data_obj, espidf_profiler_data);

//| def bench_setjmp() -> tuple:
//|     """``(libc, builtin)`` cycles per setjmp. Temporary measurement: nlr_push
//|     does one of these on every python function call."""
//|
//|
static mp_obj_t espidf_bench_setjmp(void) {
    const int rounds = 2000;
    volatile int sink = 0;

    jmp_buf lbuf;
    uint32_t t0 = esp_cpu_get_cycle_count();
    for (int i = 0; i < rounds; i++) {
        if (setjmp(lbuf) != 0) {
            sink++;
        }
    }
    uint32_t libc = (esp_cpu_get_cycle_count() - t0) / rounds;

    intptr_t bbuf[5];
    t0 = esp_cpu_get_cycle_count();
    for (int i = 0; i < rounds; i++) {
        if (__builtin_setjmp(bbuf) != 0) {
            sink++;
        }
    }
    uint32_t builtin = (esp_cpu_get_cycle_count() - t0) / rounds;

    // The other per call candidate for the ROM samples: mp_setup_code_state
    // clears the argument slots with memset, tens of bytes at a time.
    static volatile uint8_t area[64];
    t0 = esp_cpu_get_cycle_count();
    for (int i = 0; i < rounds; i++) {
        memset((void *)area, i, 32);
    }
    uint32_t memset32 = (esp_cpu_get_cycle_count() - t0) / rounds;

    // Empty loop of the same shape, so the loop itself is not counted.
    t0 = esp_cpu_get_cycle_count();
    for (int i = 0; i < rounds; i++) {
        sink++;
    }
    uint32_t empty = (esp_cpu_get_cycle_count() - t0) / rounds;

    mp_obj_t items[4] = {
        mp_obj_new_int_from_uint(libc),
        mp_obj_new_int_from_uint(builtin),
        mp_obj_new_int_from_uint(memset32),
        mp_obj_new_int_from_uint(empty),
    };
    return mp_obj_new_tuple(4, items);
}
MP_DEFINE_CONST_FUN_OBJ_0(espidf_bench_setjmp_obj, espidf_bench_setjmp);

//| def bench_div() -> tuple:
//|     """``(variable, constant, empty)`` cycles for one modulo. Temporary
//|     measurement: mp_map_lookup does one by a runtime value on every lookup."""
//|
//|
static mp_obj_t espidf_bench_div(void) {
    const int rounds = 2000;
    volatile uint32_t divisor = 37;
    volatile uint32_t sink = 0;

    uint32_t t0 = esp_cpu_get_cycle_count();
    for (uint32_t i = 0; i < rounds; i++) {
        sink += i % divisor;
    }
    uint32_t variable = (esp_cpu_get_cycle_count() - t0) / rounds;

    t0 = esp_cpu_get_cycle_count();
    for (uint32_t i = 0; i < rounds; i++) {
        sink += i % 64u;
    }
    uint32_t constant = (esp_cpu_get_cycle_count() - t0) / rounds;

    t0 = esp_cpu_get_cycle_count();
    for (uint32_t i = 0; i < rounds; i++) {
        sink += i;
    }
    uint32_t empty = (esp_cpu_get_cycle_count() - t0) / rounds;

    mp_obj_t items[3] = {
        mp_obj_new_int_from_uint(variable),
        mp_obj_new_int_from_uint(constant),
        mp_obj_new_int_from_uint(empty),
    };
    return mp_obj_new_tuple(3, items);
}
MP_DEFINE_CONST_FUN_OBJ_0(espidf_bench_div_obj, espidf_bench_div);

// CIRCUITPY-CHANGE: report a pin's actual hardware configuration and whether
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
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_pin_status_obj, espidf_pin_status);

// CIRCUITPY-CHANGE: put a vendor-specific information element into the frames the
// radio sends as an AP, so nearby devices see custom data in the beacon without
// connecting. The complement of wifi.Monitor on the receiving side.
//| def set_vendor_ie(payload: ReadableBuffer | None, *, oui: ReadableBuffer = b"\xff\xff\xff") -> None:
//|     """Attach a vendor information element to the AP's beacons, or remove it with
//|     None. ``payload`` is up to 251 bytes. ``oui`` is the three-byte vendor id.
//|     WiFi has to be running as an AP (`wifi.radio.start_ap`)."""
//|     ...
//|
//|
static mp_obj_t espidf_set_vendor_ie(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_payload, ARG_oui };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_payload, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_oui, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = mp_const_none } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    if (args[ARG_payload].u_obj == mp_const_none) {
        // Disabling does not read the element, but the IDF wants a non-null
        // pointer with a valid header anyway.
        static const vendor_ie_data_t empty = { 0xDD, 4, {0xff, 0xff, 0xff}, 0 };
        CHECK_ESP_RESULT(esp_wifi_set_vendor_ie(false, WIFI_VND_IE_TYPE_BEACON,
            WIFI_VND_IE_ID_0, &empty));
        return mp_const_none;
    }

    mp_buffer_info_t payload;
    mp_get_buffer_raise(args[ARG_payload].u_obj, &payload, MP_BUFFER_READ);
    if (payload.len > 251) {
        mp_raise_ValueError(MP_ERROR_TEXT("payload is at most 251 bytes"));
    }

    uint8_t oui[3] = { 0xff, 0xff, 0xff };
    if (args[ARG_oui].u_obj != mp_const_none) {
        mp_buffer_info_t oui_buf;
        mp_get_buffer_raise(args[ARG_oui].u_obj, &oui_buf, MP_BUFFER_READ);
        if (oui_buf.len != 3) {
            mp_raise_ValueError(MP_ERROR_TEXT("oui must be 3 bytes"));
        }
        memcpy(oui, oui_buf.buf, 3);
    }

    // Header is 6 bytes: element_id, length, oui[3], oui_type. length counts
    // everything after itself, so oui (3) + type (1) + payload.
    size_t total = sizeof(vendor_ie_data_t) + payload.len;
    vendor_ie_data_t *ie = m_malloc(total);
    ie->element_id = 0xDD;
    ie->length = 4 + payload.len;
    memcpy(ie->vendor_oui, oui, 3);
    ie->vendor_oui_type = 0;
    memcpy(ie->payload, payload.buf, payload.len);

    // Setting the same element twice returns INVALID_ARG, and the element
    // survives a soft reset, so clear it first. The result is ignored: there is
    // nothing to clear on the very first call.
    esp_wifi_set_vendor_ie(false, WIFI_VND_IE_TYPE_BEACON, WIFI_VND_IE_ID_0, NULL);
    esp_err_t err = esp_wifi_set_vendor_ie(true, WIFI_VND_IE_TYPE_BEACON,
        WIFI_VND_IE_ID_0, ie);
    m_free(ie);
    CHECK_ESP_RESULT(err);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(espidf_set_vendor_ie_obj, 1, espidf_set_vendor_ie);

// CIRCUITPY-CHANGE: check the ESP-IDF heap for corruption.
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
static MP_DEFINE_CONST_FUN_OBJ_0(espidf_check_heap_obj, espidf_check_heap);

// CIRCUITPY-CHANGE: ESP-IDF events, readable from Python.
//
// Handlers run in the event loop task, on the other core, alongside the Wi-Fi
// stack. Nothing there may touch the interpreter or block, so the handler only
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
//|     The event loop calls its handlers on another core, where the interpreter
//|     cannot be touched, so events are copied into a fixed size queue instead and
//|     read here. If the queue fills before it is drained, further events are
//|     dropped and `overflowed` becomes True.
//|
//|     Known bases are ``"WIFI_EVENT"`` and ``"IP_EVENT"``; they are also available
//|     as `espidf.WIFI_EVENT` and `espidf.IP_EVENT`."""
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

// Accepts a str as its UTF-8 bytes, or anything with a buffer.
static void espidf_get_bytes(mp_obj_t obj, qstr arg, mp_buffer_info_t *bufinfo) {
    if (mp_obj_is_str(obj)) {
        size_t len;
        const char *s = mp_obj_str_get_data(obj, &len);
        bufinfo->buf = (void *)s;
        bufinfo->len = len;
        bufinfo->typecode = 'B';
        return;
    }
    if (!mp_get_buffer(obj, bufinfo, MP_BUFFER_READ)) {
        mp_raise_TypeError_varg(MP_ERROR_TEXT("%q must be a string or buffer"), arg);
    }
}

#if CIRCUITPY_WIFI
// CIRCUITPY-CHANGE: WPA2/WPA3 Enterprise. wifi.AuthMode.ENTERPRISE already came
// back from a scan, but wifi.radio.connect() only takes a PSK, so an enterprise
// network could be seen and not joined. The credentials go in here instead of
// through connect(), because that binding is shared by every port and this is
// specific to the ESP-IDF supplicant. esp_wifi_sta_enterprise_enable() is a
// standing mode, so the existing connect() then works unchanged.

// esp_eap_client copies the identity, username and password, but for the
// certificates it keeps only the pointer (esp_eap_client.c: g_wpa_ca_cert =
// ca_cert). Those buffers therefore have to outlive the call, so the objects are
// held here until the supplicant has been told to forget them.
MP_REGISTER_ROOT_POINTER(mp_obj_t espidf_eap_certs);

static bool espidf_eap_enabled = false;

// Returns false when the argument was omitted.
static bool espidf_eap_buffer(mp_obj_t obj, qstr arg, mp_buffer_info_t *bufinfo, bool retain) {
    if (obj == mp_const_none) {
        return false;
    }
    espidf_get_bytes(obj, arg, bufinfo);
    // The IDF rejects a zero length itself, but as ESP_ERR_INVALID_ARG, which
    // surfaces as "Invalid argument" with no clue which argument.
    if (bufinfo->len == 0) {
        mp_raise_ValueError_varg(MP_ERROR_TEXT("%q must not be empty"), arg);
    }
    if (retain) {
        if (MP_STATE_VM(espidf_eap_certs) == MP_OBJ_NULL) {
            MP_STATE_VM(espidf_eap_certs) = mp_obj_new_list(0, NULL);
        }
        mp_obj_list_append(MP_STATE_VM(espidf_eap_certs), obj);
    }
    return true;
}

static void espidf_eap_forget(void) {
    esp_eap_client_clear_ca_cert();
    esp_eap_client_clear_certificate_and_key();
    esp_eap_client_clear_identity();
    esp_eap_client_clear_username();
    esp_eap_client_clear_password();
    MP_STATE_VM(espidf_eap_certs) = MP_OBJ_NULL;
}

//| def eap_enable(
//|     *,
//|     identity: Optional[str] = None,
//|     username: Optional[str] = None,
//|     password: Optional[str] = None,
//|     ca_cert: Optional[ReadableBuffer] = None,
//|     client_cert: Optional[ReadableBuffer] = None,
//|     client_key: Optional[ReadableBuffer] = None,
//|     client_key_password: Optional[str] = None,
//|     ttls_phase2: Optional[int] = None,
//| ) -> None:
//|     """Supply WPA2/WPA3 Enterprise credentials and put the station into
//|     enterprise mode. Afterwards join with ``wifi.radio.connect(ssid)`` and no
//|     password.
//|
//|     PEAP and TTLS need ``username`` and ``password``, and usually ``identity``
//|     as the anonymous outer identity. TLS needs ``client_cert`` and
//|     ``client_key``. ``ca_cert`` is optional but without it the server is not
//|     authenticated. ``ttls_phase2`` is one of the ``espidf.TTLS_PHASE2_*``
//|     values, MSCHAPV2 being the common one.
//|
//|     Certificates are kept by reference, so they stay alive until
//|     `eap_disable`."""
//|     ...
static mp_obj_t espidf_eap_enable(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_identity, ARG_username, ARG_password, ARG_ca_cert, ARG_client_cert,
           ARG_client_key, ARG_client_key_password, ARG_ttls_phase2 };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_identity, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = mp_const_none } },
        { MP_QSTR_username, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = mp_const_none } },
        { MP_QSTR_password, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = mp_const_none } },
        { MP_QSTR_ca_cert, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = mp_const_none } },
        { MP_QSTR_client_cert, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = mp_const_none } },
        { MP_QSTR_client_key, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = mp_const_none } },
        { MP_QSTR_client_key_password, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = mp_const_none } },
        { MP_QSTR_ttls_phase2, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = mp_const_none } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    // A client certificate is useless without its key and the reverse is worse:
    // esp_eap_client_set_certificate_and_key takes both together.
    bool have_cert = args[ARG_client_cert].u_obj != mp_const_none;
    bool have_key = args[ARG_client_key].u_obj != mp_const_none;
    if (have_cert != have_key) {
        mp_raise_ValueError(MP_ERROR_TEXT("client_cert and client_key go together"));
    }

    mp_buffer_info_t buf, buf2, buf3;
    if (espidf_eap_buffer(args[ARG_identity].u_obj, MP_QSTR_identity, &buf, false)) {
        CHECK_ESP_RESULT(esp_eap_client_set_identity(buf.buf, buf.len));
    }
    if (espidf_eap_buffer(args[ARG_username].u_obj, MP_QSTR_username, &buf, false)) {
        CHECK_ESP_RESULT(esp_eap_client_set_username(buf.buf, buf.len));
    }
    if (espidf_eap_buffer(args[ARG_password].u_obj, MP_QSTR_password, &buf, false)) {
        CHECK_ESP_RESULT(esp_eap_client_set_password(buf.buf, buf.len));
    }
    if (espidf_eap_buffer(args[ARG_ca_cert].u_obj, MP_QSTR_ca_cert, &buf, true)) {
        CHECK_ESP_RESULT(esp_eap_client_set_ca_cert(buf.buf, buf.len));
    }
    if (have_cert) {
        espidf_eap_buffer(args[ARG_client_cert].u_obj, MP_QSTR_client_cert, &buf, true);
        espidf_eap_buffer(args[ARG_client_key].u_obj, MP_QSTR_client_key, &buf2, true);
        bool have_pw = espidf_eap_buffer(args[ARG_client_key_password].u_obj,
            MP_QSTR_client_key_password, &buf3, true);
        CHECK_ESP_RESULT(esp_eap_client_set_certificate_and_key(buf.buf, buf.len,
            buf2.buf, buf2.len, have_pw ? buf3.buf : NULL, have_pw ? buf3.len : 0));
    }
    if (args[ARG_ttls_phase2].u_obj != mp_const_none) {
        mp_int_t phase2 = mp_arg_validate_int_range(mp_obj_get_int(args[ARG_ttls_phase2].u_obj),
            ESP_EAP_TTLS_PHASE2_EAP, ESP_EAP_TTLS_PHASE2_CHAP, MP_QSTR_ttls_phase2);
        CHECK_ESP_RESULT(esp_eap_client_set_ttls_phase2_method(phase2));
    }

    esp_err_t err = esp_wifi_sta_enterprise_enable();
    if (err != ESP_OK) {
        espidf_eap_forget();
        CHECK_ESP_RESULT(err);
    }
    espidf_eap_enabled = true;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(espidf_eap_enable_obj, 0, espidf_eap_enable);

//| def eap_disable() -> None:
//|     """Leave enterprise mode and forget the credentials. Safe to call when it
//|     was never enabled."""
//|     ...
static mp_obj_t espidf_eap_disable(void) {
    if (espidf_eap_enabled) {
        esp_wifi_sta_enterprise_disable();
        espidf_eap_enabled = false;
    }
    // Only after the supplicant has been told to stop, since it holds the
    // certificate pointers rather than copies.
    espidf_eap_forget();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(espidf_eap_disable_obj, espidf_eap_disable);

void espidf_eap_reset(void) {
    if (espidf_eap_enabled) {
        esp_wifi_sta_enterprise_disable();
        espidf_eap_enabled = false;
    }
    esp_eap_client_clear_ca_cert();
    esp_eap_client_clear_certificate_and_key();
    esp_eap_client_clear_identity();
    esp_eap_client_clear_username();
    esp_eap_client_clear_password();
    MP_STATE_VM(espidf_eap_certs) = MP_OBJ_NULL;
}

// CIRCUITPY-CHANGE: SmartConfig, so a phone can hand the board an SSID and
// password over the air instead of settings.toml being edited.
//
// The result is 114 bytes, more than EventQueue carries, so it is kept whole in a
// static here and read back through smartconfig_result(). The other SC_EVENT ids
// have no payload and can be watched with an EventQueue if the progress matters.

static bool espidf_sc_running = false;
static esp_event_handler_instance_t espidf_sc_instance;
static volatile bool espidf_sc_got;
static smartconfig_event_got_ssid_pswd_t espidf_sc_result;

// Runs in the event loop task.
static void espidf_sc_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == SC_EVENT && id == SC_EVENT_GOT_SSID_PSWD && data != NULL) {
        memcpy(&espidf_sc_result, data, sizeof(espidf_sc_result));
        espidf_sc_got = true;
        port_wake_main_task();
    }
}

//| def smartconfig_start(sc_type: int = SC_TYPE_ESPTOUCH) -> None:
//|     """Start listening for credentials from a phone running EspTouch or AirKiss.
//|
//|     The station has to be running first (``wifi.radio.start_station()``). Poll
//|     `smartconfig_result` until it returns something, connect with it, and only
//|     then call `smartconfig_stop` — the acknowledgement to the phone is sent once
//|     the board has an address, so stopping earlier makes the phone report a
//|     failure even though the board joined."""
//|     ...
static mp_obj_t espidf_smartconfig_start(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_sc_type };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_sc_type, MP_ARG_INT, { .u_int = SC_TYPE_ESPTOUCH } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    if (espidf_sc_running) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("SmartConfig is already running"));
    }
    mp_int_t sc_type = mp_arg_validate_int_range(args[ARG_sc_type].u_int,
        SC_TYPE_ESPTOUCH, SC_TYPE_ESPTOUCH_V2, MP_QSTR_sc_type);

    esp_err_t loop_err = esp_event_loop_create_default();
    if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) {
        CHECK_ESP_RESULT(loop_err);
    }

    espidf_sc_got = false;
    memset(&espidf_sc_result, 0, sizeof(espidf_sc_result));

    CHECK_ESP_RESULT(esp_smartconfig_set_type(sc_type));
    CHECK_ESP_RESULT(esp_event_handler_instance_register(SC_EVENT, ESP_EVENT_ANY_ID,
        &espidf_sc_handler, NULL, &espidf_sc_instance));
    smartconfig_start_config_t cfg = SMARTCONFIG_START_CONFIG_DEFAULT();
    esp_err_t err = esp_smartconfig_start(&cfg);
    if (err != ESP_OK) {
        esp_event_handler_instance_unregister(SC_EVENT, ESP_EVENT_ANY_ID, espidf_sc_instance);
        CHECK_ESP_RESULT(err);
    }
    espidf_sc_running = true;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(espidf_smartconfig_start_obj, 0, espidf_smartconfig_start);

//| def smartconfig_result() -> Optional[Tuple[str, str, Optional[bytes]]]:
//|     """``(ssid, password, bssid)`` once a phone has sent them, otherwise None.
//|     ``bssid`` is None unless the phone named a particular access point."""
//|     ...
static mp_obj_t espidf_smartconfig_result(void) {
    if (!espidf_sc_got) {
        return mp_const_none;
    }
    // Both fields are documented as null terminated, but a full length one fills
    // the array with no room for the terminator, so the length is bounded.
    size_t ssid_len = strnlen((const char *)espidf_sc_result.ssid, sizeof(espidf_sc_result.ssid));
    size_t pw_len = strnlen((const char *)espidf_sc_result.password, sizeof(espidf_sc_result.password));
    mp_obj_t items[3] = {
        mp_obj_new_str((const char *)espidf_sc_result.ssid, ssid_len),
        mp_obj_new_str((const char *)espidf_sc_result.password, pw_len),
        espidf_sc_result.bssid_set
            ? mp_obj_new_bytes(espidf_sc_result.bssid, sizeof(espidf_sc_result.bssid))
            : mp_const_none,
    };
    return mp_obj_new_tuple(3, items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(espidf_smartconfig_result_obj, espidf_smartconfig_result);

//| def smartconfig_stop() -> None:
//|     """Stop listening and free what SmartConfig allocated. Safe to call when it
//|     was never started."""
//|     ...
static mp_obj_t espidf_smartconfig_stop(void) {
    if (espidf_sc_running) {
        esp_smartconfig_stop();
        esp_event_handler_instance_unregister(SC_EVENT, ESP_EVENT_ANY_ID, espidf_sc_instance);
        espidf_sc_running = false;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(espidf_smartconfig_stop_obj, espidf_smartconfig_stop);

void espidf_smartconfig_reset(void) {
    if (espidf_sc_running) {
        esp_smartconfig_stop();
        esp_event_handler_instance_unregister(SC_EVENT, ESP_EVENT_ANY_ID, espidf_sc_instance);
        espidf_sc_running = false;
    }
    espidf_sc_got = false;
}
#endif // CIRCUITPY_WIFI

// CIRCUITPY-CHANGE: the crypto the ESP-IDF already has. aesio is tiny-AES-c, a
// portable software implementation, and there is no HMAC or PBKDF2 in the build
// at all -- doing those in Python costs two interpreter round trips per hash
// iteration, which is what makes a key derivation take seconds. mbedtls is
// already linked here for TLS and CONFIG_MBEDTLS_HARDWARE_AES/SHA are on, so
// exposing it is nearly free in flash.
//
// The tag is carried inside the ciphertext rather than returned beside it, so
// decryption cannot be asked for without it. Forgetting to check the tag is the
// usual way authenticated encryption gets misused.

//| def pbkdf2(
//|     password: str | ReadableBuffer,
//|     salt: str | ReadableBuffer,
//|     iterations: int,
//|     key_length: int = 32,
//| ) -> bytes:
//|     """Derive a key from a password with PBKDF2-HMAC-SHA256.
//|
//|     The same computation done in Python costs about 1.2 ms per iteration; here it
//|     is a few microseconds, so iteration counts that actually slow an attacker down
//|     become practical."""
//|     ...
static mp_obj_t espidf_pbkdf2(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_password, ARG_salt, ARG_iterations, ARG_key_length };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_password, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_salt, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_iterations, MP_ARG_REQUIRED | MP_ARG_INT },
        { MP_QSTR_key_length, MP_ARG_INT, {.u_int = 32} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_buffer_info_t password, salt;
    espidf_get_bytes(args[ARG_password].u_obj, MP_QSTR_password, &password);
    espidf_get_bytes(args[ARG_salt].u_obj, MP_QSTR_salt, &salt);
    // An iteration count of zero derives nothing, and the cap keeps a typo from
    // locking the board up for hours with no way to interrupt it.
    mp_int_t iterations = mp_arg_validate_int_range(args[ARG_iterations].u_int,
        1, 10000000, MP_QSTR_iterations);
    mp_int_t key_length = mp_arg_validate_int_range(args[ARG_key_length].u_int,
        1, 1024, MP_QSTR_key_length);

    // CIRCUITPY-CHANGE: mbedtls 4 moved pkcs5.h behind mbedtls/private, so the
    // derivation runs through PSA. PBKDF2 requires its inputs in this order:
    // cost, salt, password.
    if (psa_crypto_init() != PSA_SUCCESS) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("key derivation failed"));
    }
    psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
    psa_status_t status = psa_key_derivation_setup(&op, PSA_ALG_PBKDF2_HMAC(PSA_ALG_SHA_256));
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_input_integer(&op, PSA_KEY_DERIVATION_INPUT_COST,
            (uint64_t)iterations);
    }
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT,
            salt.buf, salt.len);
    }
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_PASSWORD,
            password.buf, password.len);
    }

    vstr_t vstr;
    vstr_init_len(&vstr, key_length);
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_output_bytes(&op, (uint8_t *)vstr.buf, key_length);
    }
    psa_key_derivation_abort(&op);
    if (status != PSA_SUCCESS) {
        vstr_clear(&vstr);
        mp_raise_RuntimeError(MP_ERROR_TEXT("key derivation failed"));
    }
    return mp_obj_new_bytes_from_vstr(&vstr);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(espidf_pbkdf2_obj, 3, espidf_pbkdf2);

//| def hmac_sha256(key: str | ReadableBuffer, data: str | ReadableBuffer) -> bytes:
//|     """HMAC-SHA256 of ``data`` under ``key``. Returns 32 bytes."""
//|     ...
static mp_obj_t espidf_hmac_sha256(mp_obj_t key_in, mp_obj_t data_in) {
    mp_buffer_info_t key, data;
    espidf_get_bytes(key_in, MP_QSTR_key, &key);
    espidf_get_bytes(data_in, MP_QSTR_data, &data);

    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (info == NULL) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("SHA256 unavailable"));
    }
    vstr_t vstr;
    vstr_init_len(&vstr, 32);
    int ret = mbedtls_md_hmac(info, key.buf, key.len, data.buf, data.len,
        (unsigned char *)vstr.buf);
    if (ret != 0) {
        vstr_clear(&vstr);
        mp_raise_RuntimeError(MP_ERROR_TEXT("HMAC failed"));
    }
    return mp_obj_new_bytes_from_vstr(&vstr);
}
static MP_DEFINE_CONST_FUN_OBJ_2(espidf_hmac_sha256_obj, espidf_hmac_sha256);

#define ESPIDF_GCM_TAG_LEN (16)

// CIRCUITPY-CHANGE: mbedtls 4 moved gcm.h behind mbedtls/private, so this goes
// through PSA. The key is held in the PSA key store only for the one call and
// destroyed before returning.
static mbedtls_svc_key_id_t espidf_gcm_import_key(mp_obj_t key_in, psa_key_usage_t usage) {
    mp_buffer_info_t key;
    espidf_get_bytes(key_in, MP_QSTR_key, &key);
    if (key.len != 16 && key.len != 24 && key.len != 32) {
        mp_raise_ValueError(MP_ERROR_TEXT("Key must be 16, 24, or 32 bytes long"));
    }
    #if !defined(SOC_AES_SUPPORT_AES_192)
    // Only the original ESP32 and the S2 have 192 bit keys in the AES peripheral.
    // Without this the failure surfaces as an opaque import error.
    if (key.len == 24) {
        mp_raise_ValueError(MP_ERROR_TEXT("AES-192 is not supported by this chip"));
    }
    #endif
    if (psa_crypto_init() != PSA_SUCCESS) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("AES setkey failed"));
    }
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attributes, usage);
    psa_set_key_algorithm(&attributes, PSA_ALG_GCM);
    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    mbedtls_svc_key_id_t key_id = 0;
    if (psa_import_key(&attributes, key.buf, key.len, &key_id) != PSA_SUCCESS) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("AES setkey failed"));
    }
    return key_id;
}

//| def aes_gcm_encrypt(
//|     key: ReadableBuffer,
//|     nonce: ReadableBuffer,
//|     data: str | ReadableBuffer,
//|     associated_data: str | ReadableBuffer = b"",
//| ) -> bytes:
//|     """Encrypt with AES-GCM. Returns the ciphertext with its 16 byte
//|     authentication tag appended, which is what `aes_gcm_decrypt` expects.
//|
//|     ``key`` is 16 or 32 bytes; the AES peripheral on this chip has no 192 bit
//|     key. ``nonce`` must never repeat for a given key;
//|     12 random bytes from `os.urandom` is the usual choice. ``associated_data`` is
//|     authenticated but not encrypted."""
//|     ...
static mp_obj_t espidf_aes_gcm_encrypt(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_key, ARG_nonce, ARG_data, ARG_associated_data };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_key, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_nonce, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_data, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_associated_data, MP_ARG_OBJ, {.u_obj = mp_const_none} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_buffer_info_t nonce, data, aad;
    espidf_get_bytes(args[ARG_nonce].u_obj, MP_QSTR_nonce, &nonce);
    espidf_get_bytes(args[ARG_data].u_obj, MP_QSTR_data, &data);
    aad.buf = NULL;
    aad.len = 0;
    if (args[ARG_associated_data].u_obj != mp_const_none) {
        espidf_get_bytes(args[ARG_associated_data].u_obj, MP_QSTR_associated_data, &aad);
    }
    if (nonce.len < 1) {
        mp_raise_ValueError(MP_ERROR_TEXT("nonce must not be empty"));
    }

    mbedtls_svc_key_id_t key_id = espidf_gcm_import_key(args[ARG_key].u_obj,
        PSA_KEY_USAGE_ENCRYPT);

    vstr_t vstr;
    vstr_init_len(&vstr, data.len + ESPIDF_GCM_TAG_LEN);
    // psa_aead_encrypt appends the tag to the ciphertext itself, which is the
    // layout aes_gcm_decrypt expects.
    size_t out_len = 0;
    psa_status_t status = psa_aead_encrypt(key_id, PSA_ALG_GCM,
        nonce.buf, nonce.len, aad.buf, aad.len, data.buf, data.len,
        (uint8_t *)vstr.buf, vstr.len, &out_len);
    psa_destroy_key(key_id);
    if (status != PSA_SUCCESS || out_len != vstr.len) {
        vstr_clear(&vstr);
        mp_raise_RuntimeError(MP_ERROR_TEXT("encryption failed"));
    }
    return mp_obj_new_bytes_from_vstr(&vstr);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(espidf_aes_gcm_encrypt_obj, 3, espidf_aes_gcm_encrypt);

//| def aes_gcm_decrypt(
//|     key: ReadableBuffer,
//|     nonce: ReadableBuffer,
//|     data: ReadableBuffer,
//|     associated_data: str | ReadableBuffer = b"",
//| ) -> bytes:
//|     """Decrypt what `aes_gcm_encrypt` produced, checking the appended tag.
//|
//|     Raises `ValueError` if the tag does not match, which means a wrong key, a
//|     wrong nonce, different associated data, or tampering. Nothing is returned in
//|     that case, so unauthenticated plaintext never reaches the caller."""
//|     ...
static mp_obj_t espidf_aes_gcm_decrypt(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_key, ARG_nonce, ARG_data, ARG_associated_data };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_key, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_nonce, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_data, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_associated_data, MP_ARG_OBJ, {.u_obj = mp_const_none} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_buffer_info_t nonce, data, aad;
    espidf_get_bytes(args[ARG_nonce].u_obj, MP_QSTR_nonce, &nonce);
    espidf_get_bytes(args[ARG_data].u_obj, MP_QSTR_data, &data);
    aad.buf = NULL;
    aad.len = 0;
    if (args[ARG_associated_data].u_obj != mp_const_none) {
        espidf_get_bytes(args[ARG_associated_data].u_obj, MP_QSTR_associated_data, &aad);
    }
    if (nonce.len < 1) {
        mp_raise_ValueError(MP_ERROR_TEXT("nonce must not be empty"));
    }
    if (data.len < ESPIDF_GCM_TAG_LEN) {
        mp_raise_ValueError(MP_ERROR_TEXT("data is too short to hold a tag"));
    }
    size_t body = data.len - ESPIDF_GCM_TAG_LEN;

    mbedtls_svc_key_id_t key_id = espidf_gcm_import_key(args[ARG_key].u_obj,
        PSA_KEY_USAGE_DECRYPT);

    vstr_t vstr;
    vstr_init_len(&vstr, body);
    // PSA reads the tag from the tail of the ciphertext, so the whole buffer
    // goes in rather than the body and the tag separately.
    size_t out_len = 0;
    psa_status_t status = psa_aead_decrypt(key_id, PSA_ALG_GCM,
        nonce.buf, nonce.len, aad.buf, aad.len, data.buf, data.len,
        (uint8_t *)vstr.buf, body, &out_len);
    psa_destroy_key(key_id);
    if (status != PSA_SUCCESS || out_len != body) {
        // The plaintext is discarded rather than returned, so a caller that
        // ignores the exception still cannot act on unauthenticated data.
        memset(vstr.buf, 0, body);
        vstr_clear(&vstr);
        mp_raise_ValueError(MP_ERROR_TEXT("authentication failed"));
    }
    return mp_obj_new_bytes_from_vstr(&vstr);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(espidf_aes_gcm_decrypt_obj, 3, espidf_aes_gcm_decrypt);

// CIRCUITPY-CHANGE: named key/value storage, the way the ESP-IDF actually keeps
// it. microcontroller.nvm gives one flat 8 kB array addressed by offset; this is
// the layer under it, with typed values under names, wear levelling and atomic
// writes. Uses a namespace other than the "CPY" that nvm claims.
typedef struct {
    mp_obj_base_t base;
    nvs_handle_t handle;
    char ns_name[NVS_NS_NAME_MAX_SIZE];
    bool open;
} espidf_nvs_obj_t;

//| class NVS:
//|     """A namespace in the ESP-IDF non-volatile storage. Keys are strings up to
//|     15 characters. Values are integers, strings or bytes. Writes are committed
//|     immediately."""
//|
//|     def __init__(self, namespace: str) -> None:
//|         """Open, creating it if needed."""
//|         ...
static mp_obj_t espidf_nvs_make_new(const mp_obj_type_t *type, size_t n_args,
    size_t n_kw, const mp_obj_t *all_args) {
    mp_arg_check_num(n_args, n_kw, 1, 1, false);
    const char *ns = mp_obj_str_get_str(all_args[0]);

    // nvm's get_nvs_handle does this init dance; do it here too in case NVS was
    // never touched yet.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    CHECK_ESP_RESULT(err);

    if (strlen(ns) >= NVS_NS_NAME_MAX_SIZE) {
        mp_raise_ValueError(MP_ERROR_TEXT("namespace too long"));
    }
    espidf_nvs_obj_t *self = mp_obj_malloc(espidf_nvs_obj_t, &espidf_nvs_type);
    CHECK_ESP_RESULT(nvs_open(ns, NVS_READWRITE, &self->handle));
    strcpy(self->ns_name, ns);
    self->open = true;
    return MP_OBJ_FROM_PTR(self);
}

static nvs_handle_t espidf_nvs_get(espidf_nvs_obj_t *self) {
    if (!self->open) {
        mp_raise_ValueError(MP_ERROR_TEXT("NVS is deinitialized"));
    }
    return self->handle;
}

//|     def __getitem__(self, key: str) -> int | str | bytes:
//|         """Read a value. Raises KeyError if the key is not set. The returned type
//|         is whatever was stored."""
//|         ...
static mp_obj_t espidf_nvs_subscr(mp_obj_t self_in, mp_obj_t key_in, mp_obj_t value) {
    espidf_nvs_obj_t *self = MP_OBJ_TO_PTR(self_in);
    nvs_handle_t handle = espidf_nvs_get(self);
    const char *key = mp_obj_str_get_str(key_in);

    if (value == MP_OBJ_SENTINEL) {
        // load: an integer wins over a blob under the same key, but this module
        // only ever writes one type per key, so a lookup of each is enough.
        int64_t i64;
        if (nvs_get_i64(handle, key, &i64) == ESP_OK) {
            return mp_obj_new_int_from_ll(i64);
        }
        size_t size = 0;
        if (nvs_get_str(handle, key, NULL, &size) == ESP_OK) {
            vstr_t vstr;
            vstr_init_len(&vstr, size - 1); // size counts the null
            nvs_get_str(handle, key, vstr.buf, &size);
            return mp_obj_new_str_from_vstr(&vstr);
        }
        if (nvs_get_blob(handle, key, NULL, &size) == ESP_OK) {
            vstr_t vstr;
            vstr_init_len(&vstr, size);
            nvs_get_blob(handle, key, vstr.buf, &size);
            return mp_obj_new_bytes_from_vstr(&vstr);
        }
        mp_raise_type_arg(&mp_type_KeyError, key_in);
    } else if (value == MP_OBJ_NULL) {
        // delete
        esp_err_t err = nvs_erase_key(handle, key);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            mp_raise_type_arg(&mp_type_KeyError, key_in);
        }
        CHECK_ESP_RESULT(err);
        CHECK_ESP_RESULT(nvs_commit(handle));
        return mp_const_none;
    } else {
        // store. Erase first, so switching the type of a key does not leave the
        // old value shadowing the new one.
        nvs_erase_key(handle, key);
        if (mp_obj_is_int(value)) {
            CHECK_ESP_RESULT(nvs_set_i64(handle, key, mp_obj_get_ll(value)));
        } else if (mp_obj_is_str(value)) {
            CHECK_ESP_RESULT(nvs_set_str(handle, key, mp_obj_str_get_str(value)));
        } else {
            mp_buffer_info_t bufinfo;
            mp_get_buffer_raise(value, &bufinfo, MP_BUFFER_READ);
            CHECK_ESP_RESULT(nvs_set_blob(handle, key, bufinfo.buf, bufinfo.len));
        }
        CHECK_ESP_RESULT(nvs_commit(handle));
        return mp_const_none;
    }
}

//|     def keys(self) -> List[str]:
//|         """Every key in this namespace."""
//|         ...
static mp_obj_t espidf_nvs_keys(mp_obj_t self_in) {
    espidf_nvs_obj_t *self = MP_OBJ_TO_PTR(self_in);
    espidf_nvs_get(self);
    mp_obj_t list = mp_obj_new_list(0, NULL);
    nvs_iterator_t it = NULL;
    esp_err_t res = nvs_entry_find(NVS_DEFAULT_PART_NAME, self->ns_name, NVS_TYPE_ANY, &it);
    while (res == ESP_OK) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        mp_obj_list_append(list, mp_obj_new_str(info.key, strlen(info.key)));
        res = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    return list;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_nvs_keys_obj, espidf_nvs_keys);

//|     def deinit(self) -> None:
//|         """Close the namespace."""
//|         ...
static mp_obj_t espidf_nvs_deinit(mp_obj_t self_in) {
    espidf_nvs_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->open) {
        nvs_close(self->handle);
        self->open = false;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_nvs_deinit_obj, espidf_nvs_deinit);

static const mp_rom_map_elem_t espidf_nvs_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_keys), MP_ROM_PTR(&espidf_nvs_keys_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&espidf_nvs_deinit_obj) },
};
static MP_DEFINE_CONST_DICT(espidf_nvs_locals_dict, espidf_nvs_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    espidf_nvs_type,
    MP_QSTR_NVS,
    MP_TYPE_FLAG_NONE,
    make_new, espidf_nvs_make_new,
    subscr, espidf_nvs_subscr,
    locals_dict, &espidf_nvs_locals_dict
    );

#if CIRCUITPY_BLEIO_NATIVE
// CIRCUITPY-CHANGE: prefer a BLE PHY. _bleio hardcodes 1M; this asks nimble to
// prefer another for new connections. On the S3, Coded PHY is Bluetooth 5 long
// range, about 4x the distance at an eighth of the rate.
//| def ble_prefer_phy(phy: int) -> None:
//|     """Set the preferred PHY for future BLE connections. 1 for 1M (the
//|     default), 2 for 2M, 4 for Coded / long range. Existing connections are not
//|     affected, and the peer has to agree."""
//|     ...
//|
//|
static mp_obj_t espidf_ble_prefer_phy(mp_obj_t phy_in) {
    mp_int_t phy = mp_obj_get_int(phy_in);
    if (phy != 1 && phy != 2 && phy != 4) {
        mp_raise_ValueError(MP_ERROR_TEXT("phy must be 1, 2 or 4"));
    }
    // The masks happen to equal the values: 1M=0x01, 2M=0x02, Coded=0x04.
    int rc = ble_gap_set_prefered_default_le_phy((uint8_t)phy, (uint8_t)phy);
    if (rc != 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("could not set PHY"));
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_ble_prefer_phy_obj, espidf_ble_prefer_phy);
#endif

// CIRCUITPY-CHANGE: transmit a raw 802.11 frame.
//
// This is a test and development tool for your own network and hardware. The
// radio will send whatever bytes you hand it, so it is also what is used to
// spoof frames and knock other devices off a network, which is why CircuitPython
// does not expose it. Use it on gear you own and are allowed to test.
//| def wifi_raw_tx(frame: ReadableBuffer, *, channel: int | None = None) -> None:
//|     """Send a raw 802.11 frame. The FCS is appended by the hardware, so do not
//|     include it. WiFi must be started (`wifi.radio.enabled = True`); set
//|     ``channel`` to move there first.
//|
//|     For testing on hardware you own and are authorised to test."""
//|     ...
//|
//|
//| def wifi_sleep_min_active_time(milliseconds: int) -> None:
//|     """How long the radio stays awake after a packet before it is allowed to
//|     sleep again, with `wifi.PowerManagement.MIN` or ``MAX`` in effect. Every
//|     further packet restarts the window, so a peer that talks more often than
//|     this keeps the radio up and never waits for the next DTIM beacon.
//|
//|     The build sets this once from CONFIG_ESP_WIFI_SLP_DEFAULT_MIN_ACTIVE_TIME,
//|     which menuconfig caps at 60 ms. Raising it at run time trades current for
//|     latency without going all the way to `wifi.PowerManagement.NONE`: a server
//|     answering a poll every 200 ms can hold the radio up across the gap while it
//|     is busy and drop back to a few tens of milliseconds when it is not.
//|
//|     Applies to the whole radio, not one socket."""
//|     ...
//|
//|
#if CIRCUITPY_WIFI
static mp_obj_t espidf_wifi_sleep_min_active_time(mp_obj_t ms_in) {
    // The driver takes microseconds. An hour is far past any useful setting and
    // keeps the multiplication below inside 32 bits.
    mp_int_t ms = mp_arg_validate_int_range(mp_obj_get_int(ms_in), 1, 3600000,
        MP_QSTR_milliseconds);
    esp_wifi_set_sleep_min_active_time((uint32_t)ms * 1000);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_wifi_sleep_min_active_time_obj, espidf_wifi_sleep_min_active_time);
#endif

static mp_obj_t espidf_wifi_raw_tx(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_frame, ARG_channel };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_frame, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_channel, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = mp_const_none } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(args[ARG_frame].u_obj, &bufinfo, MP_BUFFER_READ);
    if (bufinfo.len < 24 || bufinfo.len > 1500) {
        mp_raise_ValueError(MP_ERROR_TEXT("frame must be 24 to 1500 bytes"));
    }

    if (args[ARG_channel].u_obj != mp_const_none) {
        mp_int_t channel = mp_obj_get_int(args[ARG_channel].u_obj);
        if (channel < 1 || channel > 14) {
            mp_raise_ValueError(MP_ERROR_TEXT("channel must be 1 to 14"));
        }
        CHECK_ESP_RESULT(esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE));
    }

    // en_sys_seq false: leave the sequence number as given in the frame.
    CHECK_ESP_RESULT(esp_wifi_80211_tx(WIFI_IF_STA, bufinfo.buf, bufinfo.len, false));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(espidf_wifi_raw_tx_obj, 1, espidf_wifi_raw_tx);

// CIRCUITPY-CHANGE: channel state information from the WiFi radio.
//
// For every frame the radio receives, the hardware reports how the channel
// distorted it. The pattern changes when something in the room moves, which is
// what makes it usable for presence and motion detection.
//
// Frames only arrive when the radio has a reason to receive them, so this needs
// either a connection or wifi.Monitor running alongside.
//
// The ESP-IDF callback runs in the WiFi task and its buffer is only valid for the
// duration of the call, so the record is copied into a ring buffer that Python
// drains. Dropping is preferred to blocking the radio.

#if CIRCUITPY_ESPIDF_CSI

// The 64-bit field goes first so the rest packs without padding holes.
typedef struct {
    int64_t timestamp;
    uint16_t length;
    uint16_t rx_seq;
    uint8_t mac[6];
    int8_t rssi;
    int8_t noise_floor;
    uint8_t channel;
    uint8_t sig_mode;
    uint8_t rx_state;
    uint8_t cwb;
    uint8_t first_word_invalid;
    int8_t data[ESPIDF_CSI_MAX_BYTES];
} espidf_csi_record_t;

// Metadata slots filled by readinto(). int32 throughout so an array('i') reads
// them without unpacking, and the layout stays fixed as fields are added.
#define ESPIDF_CSI_META_INTS (16)

typedef struct {
    mp_obj_base_t base;
    espidf_csi_record_t *records;
    size_t size;
    volatile size_t head;
    volatile size_t tail;
    volatile size_t lost;
    uint8_t source[6];
    bool filtered;
    bool running;
} espidf_csi_obj_t;

// The callback needs to find the object without going through the heap.
static espidf_csi_obj_t *espidf_csi_singleton;

static void espidf_csi_cb(void *ctx, wifi_csi_info_t *info) {
    espidf_csi_obj_t *self = espidf_csi_singleton;
    if (self == NULL || !self->running || info == NULL || info->buf == NULL) {
        return;
    }
    // Channel state from two transmitters describes two different paths and
    // averaging them is meaningless, so an unwanted source is dropped before it
    // costs a queue slot.
    if (self->filtered && memcmp(info->mac, self->source, 6) != 0) {
        return;
    }
    size_t next = (self->head + 1) % self->size;
    if (next == self->tail) {
        self->lost++;
        return;
    }
    espidf_csi_record_t *rec = &self->records[self->head];
    rec->timestamp = esp_timer_get_time();
    rec->rssi = info->rx_ctrl.rssi;
    rec->noise_floor = info->rx_ctrl.noise_floor;
    rec->channel = info->rx_ctrl.channel;
    rec->rx_state = info->rx_ctrl.rx_state;
    // The receive metadata differs between generations as much as the
    // acquisition config does. Wi-Fi 6 parts name the format field
    // cur_bb_format and number it differently, and carry no bandwidth flag at
    // all -- a nonzero secondary channel is what says the frame was 40 MHz.
    #if defined(CONFIG_SOC_WIFI_HE_SUPPORT)
    rec->sig_mode = info->rx_ctrl.cur_bb_format;
    rec->cwb = info->rx_ctrl.second != 0;
    #else
    rec->sig_mode = info->rx_ctrl.sig_mode;
    rec->cwb = info->rx_ctrl.cwb;
    #endif
    rec->first_word_invalid = info->first_word_invalid ? 1 : 0;
    rec->rx_seq = info->rx_seq;
    memcpy(rec->mac, info->mac, 6);
    size_t len = info->len;
    if (len > ESPIDF_CSI_MAX_BYTES) {
        len = ESPIDF_CSI_MAX_BYTES;
    }
    rec->length = len;
    memcpy(rec->data, info->buf, len);
    self->head = next;
}

// Shared by readinto() and readinto_amplitude(): both hand back the same slots.
static void espidf_csi_fill_meta(espidf_csi_record_t *rec, int32_t *meta) {
    meta[0] = rec->rssi;
    meta[1] = rec->noise_floor;
    meta[2] = rec->channel;
    meta[3] = rec->sig_mode;
    meta[4] = rec->rx_state;
    meta[5] = rec->rx_seq;
    meta[6] = rec->cwb;
    meta[7] = rec->first_word_invalid;
    meta[8] = (int32_t)(uint32_t)(rec->timestamp & 0xFFFFFFFF);
    meta[9] = (int32_t)(rec->timestamp >> 32);
    for (size_t i = 0; i < 6; i++) {
        meta[10 + i] = rec->mac[i];
    }
}

// Checked before the queue is looked at, so that a buffer of the wrong size
// fails the same way whether or not a frame happens to be waiting. Validating
// after the empty-queue check would leave the mistake latent until the timing
// changed.
static bool espidf_csi_check_meta(mp_obj_t meta_in, mp_buffer_info_t *meta) {
    if (meta_in == mp_const_none) {
        return false;
    }
    mp_get_buffer_raise(meta_in, meta, MP_BUFFER_WRITE);
    if (meta->len < ESPIDF_CSI_META_INTS * sizeof(int32_t)) {
        mp_raise_ValueError(MP_ERROR_TEXT("meta must hold 16 32-bit ints"));
    }
    return true;
}

// The caller may pass any writable buffer, including a memoryview slice that
// starts off a word boundary, so the slots are built aligned and copied.
static void espidf_csi_store_meta(mp_buffer_info_t *meta, espidf_csi_record_t *rec) {
    int32_t slots[ESPIDF_CSI_META_INTS];
    espidf_csi_fill_meta(rec, slots);
    memcpy(meta->buf, slots, sizeof(slots));
}

//| class CSI:
//|     """Channel state information for received WiFi frames.
//|
//|     Where RSSI is one number per frame, this is the radio's estimate of the
//|     channel on each OFDM subcarrier. Being frequency resolved, it registers a
//|     change in the multipath around the antenna even when the received power
//|     does not move, which is what makes presence and motion detection possible.
//|
//|     Only frames the station itself receives are reported, and only while
//|     something is arriving, so this needs a connection and traffic on it.
//|     Pinging the gateway in a loop is the usual way to get a steady rate.
//|     A running `wifi.Monitor` suppresses CSI entirely rather than feeding it:
//|     measured on ESP-IDF v6.0.1, promiscuous mode yields no records at all,
//|     on the connected channel or any other. Only one instance at a time.
//|
//|     Records accumulate in a queue outside the heap and are dropped rather than
//|     stalling the radio, so `lost()` is worth watching. `packet()` is the
//|     convenient reader; `readinto()` and `readinto_amplitude()` allocate nothing
//|     and are the ones that keep up at a useful frame rate."""
//|
//|     def __init__(
//|         self,
//|         queue: int = 16,
//|         *,
//|         source: ReadableBuffer | None = None,
//|         lltf: bool = True,
//|         htltf: bool = True,
//|         stbc_htltf2: bool = True,
//|         ltf_merge: bool = True,
//|         channel_filter: bool = True,
//|         shift: int | None = None,
//|         dump_ack: bool = False,
//|         he_su: bool = True,
//|         he_mu: bool = True,
//|         he_dcm: bool = True,
//|         he_beamformed: bool = True,
//|     ) -> None:
//|         """Start collecting. ``queue`` is how many records are buffered before
//|         the oldest ones start being dropped.
//|
//|         ``source`` is a six byte MAC address. Frames from anything else are
//|         discarded in the callback without taking a queue slot. Channel state
//|         from two transmitters describes two different paths, so anything that
//|         compares records over time wants this set.
//|
//|         ``lltf``, ``htltf`` and ``stbc_htltf2`` select which training fields
//|         are reported. Each adds to the record, and only the first
//|         ``ESPIDF_CSI_MAX_BYTES`` are kept, so leaving all three on truncates a
//|         HT frame. ``lltf`` alone gives the same 128 bytes for HT and non-HT
//|         frames, which is what makes records comparable.
//|
//|         ``shift`` is the scaling: `None` lets the radio scale automatically,
//|         a number fixes it instead. The range depends on the part -- 0 to 15
//|         where CSI is described the pre-Wi-Fi-6 way, 0 to 8 on Wi-Fi 6 parts
//|         with MAC version 3 (C5, C61) and 0 to 3 on MAC version 2 (C6).
//|
//|         Wi-Fi 6 parts describe acquisition differently and not every argument
//|         reaches the hardware on every chip:
//|
//|         ===================  ==========================  =================
//|         argument             pre-Wi-Fi-6 (ESP32, S2,     Wi-Fi 6 (C5, C6,
//|                              S3, C3, C2)                 C61)
//|         ===================  ==========================  =================
//|         ``lltf``             L-LTF                       L-LTF
//|         ``htltf``            HT-LTF                      HT-LTF, both 20 and
//|                                                          40 MHz
//|         ``stbc_htltf2``      STBC HT-LTF2                ignored
//|         ``ltf_merge``        averages L-LTF and HT-LTF   ignored
//|         ``channel_filter``   smooths adjacent carriers   ignored
//|         ``dump_ack``         ACK frames                  ACK frames
//|         ``he_su``            ignored                     HE-LTF, single user
//|         ``he_mu``            ignored                     HE-LTF, multi user
//|         ``he_dcm``           ignored                     HE-LTF, DCM
//|         ``he_beamformed``    ignored                     HE-LTF, beamformed
//|         ===================  ==========================  =================
//|
//|         An HE record holds far more than an HT one -- 242 tones against 64 --
//|         so ``ESPIDF_CSI_MAX_BYTES`` defaults higher on those parts."""
//|         ...
static mp_obj_t espidf_csi_make_new(const mp_obj_type_t *type, size_t n_args,
    size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_queue, ARG_source, ARG_lltf, ARG_htltf, ARG_stbc_htltf2,
           ARG_ltf_merge, ARG_channel_filter, ARG_shift, ARG_dump_ack,
           ARG_he_su, ARG_he_mu, ARG_he_dcm, ARG_he_beamformed };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_queue, MP_ARG_INT, { .u_int = 16 } },
        { MP_QSTR_source, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = mp_const_none } },
        { MP_QSTR_lltf, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = true } },
        { MP_QSTR_htltf, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = true } },
        { MP_QSTR_stbc_htltf2, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = true } },
        { MP_QSTR_ltf_merge, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = true } },
        { MP_QSTR_channel_filter, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = true } },
        { MP_QSTR_shift, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = mp_const_none } },
        { MP_QSTR_dump_ack, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = false } },
        { MP_QSTR_he_su, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = true } },
        { MP_QSTR_he_mu, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = true } },
        { MP_QSTR_he_dcm, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = true } },
        { MP_QSTR_he_beamformed, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = true } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    if (espidf_csi_singleton != NULL) {
        mp_raise_ValueError(MP_ERROR_TEXT("CSI is already running"));
    }
    mp_int_t queue = args[ARG_queue].u_int;
    if (queue < 2 || queue > 64) {
        mp_raise_ValueError(MP_ERROR_TEXT("queue must be 2 to 64"));
    }
    #if defined(CONFIG_SOC_WIFI_HE_SUPPORT)
    bool any_ltf = args[ARG_lltf].u_bool || args[ARG_htltf].u_bool ||
        args[ARG_he_su].u_bool || args[ARG_he_mu].u_bool ||
        args[ARG_he_dcm].u_bool || args[ARG_he_beamformed].u_bool;
    #else
    bool any_ltf = args[ARG_lltf].u_bool || args[ARG_htltf].u_bool ||
        args[ARG_stbc_htltf2].u_bool;
    #endif
    if (!any_ltf) {
        mp_raise_ValueError(MP_ERROR_TEXT("at least one training field is needed"));
    }

    uint8_t source[6];
    bool filtered = args[ARG_source].u_obj != mp_const_none;
    if (filtered) {
        mp_buffer_info_t source_buf;
        mp_get_buffer_raise(args[ARG_source].u_obj, &source_buf, MP_BUFFER_READ);
        if (source_buf.len != 6) {
            mp_raise_ValueError(MP_ERROR_TEXT("source must be 6 bytes"));
        }
        memcpy(source, source_buf.buf, 6);
    }

    // Scaling is spelled differently on Wi-Fi 6 parts: a manual-scale flag plus
    // a 0-15 shift before, one field afterwards, four bits wide on MAC version 3
    // and two before it. Validated here, ahead of the allocation below.
    #if defined(CONFIG_SOC_WIFI_HE_SUPPORT)
    #if defined(CONFIG_SOC_WIFI_MAC_VERSION_NUM) && CONFIG_SOC_WIFI_MAC_VERSION_NUM >= 3
    #define ESPIDF_CSI_SHIFT_MAX (8)
    #else
    #define ESPIDF_CSI_SHIFT_MAX (3)
    #endif
    #else
    #define ESPIDF_CSI_SHIFT_MAX (15)
    #endif
    bool manu_scale = args[ARG_shift].u_obj != mp_const_none;
    mp_int_t shift = 0;
    if (manu_scale) {
        shift = mp_arg_validate_int_range(mp_obj_get_int(args[ARG_shift].u_obj), 0,
            ESPIDF_CSI_SHIFT_MAX, MP_QSTR_shift);
    }

    espidf_csi_obj_t *self = mp_obj_malloc(espidf_csi_obj_t, &espidf_csi_type);
    // Outside the heap: the callback runs in the WiFi task and must not race a
    // collection, and the buffer is too big to want moving around anyway.
    self->records = heap_caps_calloc(queue, sizeof(espidf_csi_record_t), MALLOC_CAP_8BIT);
    if (self->records == NULL) {
        mp_raise_espidf_MemoryError();
    }
    self->size = queue;
    self->head = 0;
    self->tail = 0;
    self->lost = 0;
    self->filtered = filtered;
    if (filtered) {
        memcpy(self->source, source, 6);
    }
    self->running = true;
    espidf_csi_singleton = self;

    // Wi-Fi 6 parts describe acquisition with an entirely different structure:
    // wifi_csi_config_t is wifi_csi_acquire_config_t there, and of the fields
    // below only dump_ack_en exists in both. The Python arguments stay the same
    // so a script ports unchanged; the docstring says which of them the hardware
    // can act on.
    #if defined(CONFIG_SOC_WIFI_HE_SUPPORT)
    // The STBC selector is left at zero, which means the same thing on both MAC
    // versions -- take the complete first HE-LTF. Only its name differs
    // (acquire_csi_he_stbc against acquire_csi_he_stbc_mode), so not naming it
    // keeps one branch instead of two.
    wifi_csi_config_t csi_config = {
        .enable = 1,
        .acquire_csi_legacy = args[ARG_lltf].u_bool,
        .acquire_csi_ht20 = args[ARG_htltf].u_bool,
        .acquire_csi_ht40 = args[ARG_htltf].u_bool,
        .acquire_csi_su = args[ARG_he_su].u_bool,
        .acquire_csi_mu = args[ARG_he_mu].u_bool,
        .acquire_csi_dcm = args[ARG_he_dcm].u_bool,
        .acquire_csi_beamformed = args[ARG_he_beamformed].u_bool,
        .val_scale_cfg = (uint32_t)shift,
        .dump_ack_en = args[ARG_dump_ack].u_bool,
    };
    #else
    wifi_csi_config_t csi_config = {
        .lltf_en = args[ARG_lltf].u_bool,
        .htltf_en = args[ARG_htltf].u_bool,
        .stbc_htltf2_en = args[ARG_stbc_htltf2].u_bool,
        .ltf_merge_en = args[ARG_ltf_merge].u_bool,
        .channel_filter_en = args[ARG_channel_filter].u_bool,
        .manu_scale = manu_scale,
        .shift = (uint8_t)shift,
        .dump_ack_en = args[ARG_dump_ack].u_bool,
    };
    #endif
    esp_err_t err = esp_wifi_set_csi_config(&csi_config);
    if (err == ESP_OK) {
        err = esp_wifi_set_csi_rx_cb(espidf_csi_cb, NULL);
    }
    if (err == ESP_OK) {
        err = esp_wifi_set_csi(true);
    }
    if (err != ESP_OK) {
        espidf_csi_singleton = NULL;
        self->running = false;
        free(self->records);
        self->records = NULL;
        CHECK_ESP_RESULT(err);
    }
    return MP_OBJ_FROM_PTR(self);
}

//|     def queued(self) -> int:
//|         """How many records are waiting."""
//|         ...
static mp_obj_t espidf_csi_queued(mp_obj_t self_in) {
    espidf_csi_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->records == NULL) {
        return MP_OBJ_NEW_SMALL_INT(0);
    }
    size_t head = self->head, tail = self->tail;
    return MP_OBJ_NEW_SMALL_INT((head + self->size - tail) % self->size);
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_csi_queued_obj, espidf_csi_queued);

//|     def lost(self) -> int:
//|         """How many records were dropped because the queue was full."""
//|         ...
static mp_obj_t espidf_csi_lost(mp_obj_t self_in) {
    espidf_csi_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_int_from_uint(self->lost);
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_csi_lost_obj, espidf_csi_lost);

//|     def packet(self) -> dict | None:
//|         """Take the oldest record, or None when there is nothing.
//|
//|         The keys are ``rssi``, ``noise_floor``, ``channel``, ``sig_mode``,
//|         ``rx_state``, ``rx_seq``, ``cwb``, ``first_word_invalid``, ``mac``,
//|         ``timestamp`` and ``data``, the last one being the signed
//|         per-subcarrier values as ``[imaginary, real]`` pairs.
//|
//|         ``sig_mode`` says what the frame was and so how ``data`` is laid out,
//|         but the radio reports it on two different scales and they do not
//|         agree. On parts without Wi-Fi 6 it is the ``sig_mode`` field: 0 for
//|         non-HT, 1 for HT, 3 for VHT. On Wi-Fi 6 parts it is instead
//|         ``cur_bb_format``: 0 for 11b, 1 for 11g or 11a, 2 for HT, 3 for VHT,
//|         4 for HE SU, 5 for HE MU, 6 for HE ER SU, 7 for HE TB, 11 for VHT MU.
//|         Note that 3 means VHT on both but 1 does not mean the same thing, so
//|         code that has to run on either has to know which part it is on.
//|
//|         ``rx_state`` is nonzero for a frame the radio received with errors,
//|         which is worth discarding. ``first_word_invalid`` means the leading
//|         four bytes of ``data`` are not real measurements. ``cwb`` is 0 for a
//|         20 MHz frame and 1 for 40 MHz; on Wi-Fi 6 parts, which carry no such
//|         flag, it is derived from the secondary channel being set.
//|
//|         This allocates a dict and a bytes on every call. `readinto()` is the
//|         one to use when frames are arriving quickly."""
//|         ...
static mp_obj_t espidf_csi_packet(mp_obj_t self_in) {
    espidf_csi_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->records == NULL || self->head == self->tail) {
        return mp_const_none;
    }
    espidf_csi_record_t *rec = &self->records[self->tail];
    mp_obj_t result = mp_obj_new_dict(11);
    mp_obj_dict_store(result, MP_ROM_QSTR(MP_QSTR_rssi), MP_OBJ_NEW_SMALL_INT(rec->rssi));
    mp_obj_dict_store(result, MP_ROM_QSTR(MP_QSTR_noise_floor), MP_OBJ_NEW_SMALL_INT(rec->noise_floor));
    mp_obj_dict_store(result, MP_ROM_QSTR(MP_QSTR_channel), MP_OBJ_NEW_SMALL_INT(rec->channel));
    mp_obj_dict_store(result, MP_ROM_QSTR(MP_QSTR_sig_mode), MP_OBJ_NEW_SMALL_INT(rec->sig_mode));
    mp_obj_dict_store(result, MP_ROM_QSTR(MP_QSTR_rx_state), MP_OBJ_NEW_SMALL_INT(rec->rx_state));
    mp_obj_dict_store(result, MP_ROM_QSTR(MP_QSTR_rx_seq), MP_OBJ_NEW_SMALL_INT(rec->rx_seq));
    mp_obj_dict_store(result, MP_ROM_QSTR(MP_QSTR_cwb), MP_OBJ_NEW_SMALL_INT(rec->cwb));
    mp_obj_dict_store(result, MP_ROM_QSTR(MP_QSTR_first_word_invalid),
        mp_obj_new_bool(rec->first_word_invalid));
    mp_obj_dict_store(result, MP_ROM_QSTR(MP_QSTR_mac), mp_obj_new_bytes(rec->mac, 6));
    mp_obj_dict_store(result, MP_ROM_QSTR(MP_QSTR_timestamp), mp_obj_new_int_from_ll(rec->timestamp));
    mp_obj_dict_store(result, MP_ROM_QSTR(MP_QSTR_data),
        mp_obj_new_bytes((const byte *)rec->data, rec->length));
    self->tail = (self->tail + 1) % self->size;
    return result;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_csi_packet_obj, espidf_csi_packet);

//|     def readinto(
//|         self, data: WriteableBuffer, meta: WriteableBuffer | None = None
//|     ) -> int | None:
//|         """Copy the oldest record into ``data`` and return how many bytes were
//|         written, or None when the queue is empty. Nothing is allocated, so this
//|         keeps up where `packet()` starts feeding the collector.
//|
//|         Only as much as fits is copied. A return value equal to ``len(data)``
//|         means the record may have been longer.
//|
//|         ``meta``, if given, must hold at least sixteen 32-bit ints, and an
//|         ``array('i')`` reads back directly:
//|
//|         ==========  ====================================================
//|         index       value
//|         ==========  ====================================================
//|         0           rssi
//|         1           noise_floor
//|         2           channel
//|         3           sig_mode
//|         4           rx_state
//|         5           rx_seq
//|         6           cwb, 0 for 20 MHz and 1 for 40 MHz
//|         7           first_word_invalid
//|         8           timestamp, low 32 bits
//|         9           timestamp, high 32 bits
//|         10 to 15    the six bytes of the source MAC
//|         ==========  ====================================================
//|
//|         The timestamp reassembles as ``(meta[9] << 32) | (meta[8] &
//|         0xFFFFFFFF)`` in microseconds."""
//|         ...
static mp_obj_t espidf_csi_readinto(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_data, ARG_meta };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_data, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_meta, MP_ARG_OBJ, { .u_obj = mp_const_none } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_buffer_info_t data;
    mp_get_buffer_raise(args[ARG_data].u_obj, &data, MP_BUFFER_WRITE);
    mp_buffer_info_t meta;
    bool has_meta = espidf_csi_check_meta(args[ARG_meta].u_obj, &meta);

    espidf_csi_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    if (self->records == NULL || self->head == self->tail) {
        return mp_const_none;
    }
    espidf_csi_record_t *rec = &self->records[self->tail];
    size_t len = rec->length;
    if (len > data.len) {
        len = data.len;
    }
    memcpy(data.buf, rec->data, len);
    if (has_meta) {
        espidf_csi_store_meta(&meta, rec);
    }
    self->tail = (self->tail + 1) % self->size;
    return MP_OBJ_NEW_SMALL_INT(len);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(espidf_csi_readinto_obj, 2, espidf_csi_readinto);

//|     def readinto_amplitude(
//|         self,
//|         out: WriteableBuffer,
//|         meta: WriteableBuffer | None = None,
//|         skip: int = 0,
//|     ) -> int | None:
//|         """Take the oldest record, turn each ``[imaginary, real]`` pair into a
//|         magnitude, and write those into ``out`` as native floats. Returns how
//|         many were written, or None when the queue is empty.
//|
//|         ``out`` is meant to be a ``ulab`` array of the default float type, whose
//|         buffer this fills in place, leaving it ready for ``numpy`` without a
//|         conversion step. The alternative is sixty-odd Python level float
//|         operations per frame, which is what actually costs the time.
//|
//|         ``skip`` drops that many leading bytes of the record. Pass 4 when
//|         ``first_word_invalid`` is set, since that covers two pairs.
//|
//|         ``meta`` is filled exactly as in `readinto()`."""
//|         ...
static mp_obj_t espidf_csi_readinto_amplitude(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_out, ARG_meta, ARG_skip };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_out, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_meta, MP_ARG_OBJ, { .u_obj = mp_const_none } },
        { MP_QSTR_skip, MP_ARG_INT, { .u_int = 0 } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_int_t skip = mp_arg_validate_int_range(args[ARG_skip].u_int, 0, ESPIDF_CSI_MAX_BYTES,
        MP_QSTR_skip);

    mp_buffer_info_t out;
    mp_get_buffer_raise(args[ARG_out].u_obj, &out, MP_BUFFER_WRITE);
    // A memoryview can start part way into its base, and Xtensa wants floats
    // aligned, so refuse rather than fault.
    if (((uintptr_t)out.buf % sizeof(mp_float_t)) != 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("out must be word aligned"));
    }
    mp_buffer_info_t meta;
    bool has_meta = espidf_csi_check_meta(args[ARG_meta].u_obj, &meta);

    espidf_csi_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    if (self->records == NULL || self->head == self->tail) {
        return mp_const_none;
    }
    espidf_csi_record_t *rec = &self->records[self->tail];
    size_t pairs = rec->length > (size_t)skip ? (rec->length - skip) / 2 : 0;
    if (pairs > out.len / sizeof(mp_float_t)) {
        pairs = out.len / sizeof(mp_float_t);
    }
    const int8_t *iq = rec->data + skip;
    mp_float_t *dest = (mp_float_t *)out.buf;
    for (size_t i = 0; i < pairs; i++) {
        mp_float_t im = iq[2 * i];
        mp_float_t re = iq[2 * i + 1];
        dest[i] = MICROPY_FLOAT_C_FUN(sqrt)(im * im + re * re);
    }
    if (has_meta) {
        espidf_csi_store_meta(&meta, rec);
    }
    self->tail = (self->tail + 1) % self->size;
    return MP_OBJ_NEW_SMALL_INT(pairs);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(espidf_csi_readinto_amplitude_obj, 2, espidf_csi_readinto_amplitude);

//|     def deinit(self) -> None:
//|         """Stop collecting and release the queue."""
//|         ...
static mp_obj_t espidf_csi_deinit(mp_obj_t self_in) {
    espidf_csi_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->running) {
        self->running = false;
        esp_wifi_set_csi(false);
        esp_wifi_set_csi_rx_cb(NULL, NULL);
    }
    espidf_csi_singleton = NULL;
    if (self->records != NULL) {
        free(self->records);
        self->records = NULL;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_csi_deinit_obj, espidf_csi_deinit);

// Called on soft reset: the object is about to go away, but the radio would keep
// calling into it.
void espidf_csi_reset(void) {
    if (espidf_csi_singleton != NULL) {
        espidf_csi_singleton->running = false;
        esp_wifi_set_csi(false);
        esp_wifi_set_csi_rx_cb(NULL, NULL);
        free(espidf_csi_singleton->records);
        espidf_csi_singleton->records = NULL;
        espidf_csi_singleton = NULL;
    }
}

static const mp_rom_map_elem_t espidf_csi_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_queued), MP_ROM_PTR(&espidf_csi_queued_obj) },
    { MP_ROM_QSTR(MP_QSTR_lost), MP_ROM_PTR(&espidf_csi_lost_obj) },
    { MP_ROM_QSTR(MP_QSTR_packet), MP_ROM_PTR(&espidf_csi_packet_obj) },
    { MP_ROM_QSTR(MP_QSTR_readinto), MP_ROM_PTR(&espidf_csi_readinto_obj) },
    { MP_ROM_QSTR(MP_QSTR_readinto_amplitude), MP_ROM_PTR(&espidf_csi_readinto_amplitude_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&espidf_csi_deinit_obj) },
};
static MP_DEFINE_CONST_DICT(espidf_csi_locals_dict, espidf_csi_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    espidf_csi_type,
    MP_QSTR_CSI,
    MP_TYPE_FLAG_NONE,
    make_new, espidf_csi_make_new,
    locals_dict, &espidf_csi_locals_dict
    );

#else

void espidf_csi_reset(void) {
}

#endif // CIRCUITPY_ESPIDF_CSI

// CIRCUITPY-CHANGE: raw access to the flash partitions.
//
// The point of this is mmap(): it maps the partition into the data address space
// and hands back a memoryview over it, so a few hundred kilobytes of tables can be
// read without a single byte on the heap.
//
// Writing is guarded only against the partition the firmware is running from.
// Everything else is fair game and it is possible to destroy the filesystem with
// it, so know which partition you asked for.

typedef struct {
    mp_obj_base_t base;
    const esp_partition_t *part;
    esp_partition_mmap_handle_t map_handle;
    bool mapped;
} espidf_partition_obj_t;

static const esp_partition_t *espidf_partition_check(espidf_partition_obj_t *self) {
    if (self->part == NULL) {
        mp_raise_ValueError(MP_ERROR_TEXT("Partition is deinitialized"));
    }
    return self->part;
}

static void espidf_partition_check_writable(const esp_partition_t *part) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running != NULL && part->address == running->address) {
        mp_raise_ValueError(MP_ERROR_TEXT("cannot write the running partition"));
    }
}

//| class Partition:
//|     """One entry of the flash partition table.
//|
//|     In this build ``ota_1`` is 2 MB and unused, because OTA is not compiled in,
//|     so it is the obvious place to keep large read-only data."""
//|
//|     def __init__(self, label: str) -> None:
//|         """Look the partition up by its label. Raises ValueError if there is no
//|         such partition."""
//|         ...
static mp_obj_t espidf_partition_make_new(const mp_obj_type_t *type, size_t n_args,
    size_t n_kw, const mp_obj_t *all_args) {
    mp_arg_check_num(n_args, n_kw, 1, 1, false);
    const char *label = mp_obj_str_get_str(all_args[0]);

    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_ANY,
        ESP_PARTITION_SUBTYPE_ANY, label);
    if (part == NULL) {
        mp_raise_ValueError(MP_ERROR_TEXT("no such partition"));
    }

    espidf_partition_obj_t *self = mp_obj_malloc(espidf_partition_obj_t, &espidf_partition_type);
    self->part = part;
    self->mapped = false;
    return MP_OBJ_FROM_PTR(self);
}

//|     def read(self, offset: int, length: int) -> bytes:
//|         """Read from the partition."""
//|         ...
static mp_obj_t espidf_partition_read(mp_obj_t self_in, mp_obj_t offset_in, mp_obj_t length_in) {
    espidf_partition_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const esp_partition_t *part = espidf_partition_check(self);
    size_t offset = mp_obj_get_int(offset_in);
    size_t length = mp_obj_get_int(length_in);
    if (offset > part->size || length > part->size - offset) {
        mp_raise_ValueError(MP_ERROR_TEXT("read past the end of the partition"));
    }
    vstr_t vstr;
    vstr_init_len(&vstr, length);
    CHECK_ESP_RESULT(esp_partition_read(part, offset, vstr.buf, length));
    return mp_obj_new_bytes_from_vstr(&vstr);
}
static MP_DEFINE_CONST_FUN_OBJ_3(espidf_partition_read_obj, espidf_partition_read);

//|     def write(self, offset: int, data: ReadableBuffer) -> None:
//|         """Write to the partition. The area has to be erased first: flash can only
//|         turn ones into zeros."""
//|         ...
static mp_obj_t espidf_partition_write(mp_obj_t self_in, mp_obj_t offset_in, mp_obj_t data_in) {
    espidf_partition_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const esp_partition_t *part = espidf_partition_check(self);
    espidf_partition_check_writable(part);
    size_t offset = mp_obj_get_int(offset_in);
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(data_in, &bufinfo, MP_BUFFER_READ);
    if (offset > part->size || bufinfo.len > part->size - offset) {
        mp_raise_ValueError(MP_ERROR_TEXT("write past the end of the partition"));
    }
    CHECK_ESP_RESULT(esp_partition_write(part, offset, bufinfo.buf, bufinfo.len));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(espidf_partition_write_obj, espidf_partition_write);

//|     def erase(self, offset: int = 0, length: int | None = None) -> None:
//|         """Erase part of the partition, whole 4096 byte sectors only. Erases all
//|         of it when called without arguments."""
//|         ...
static mp_obj_t espidf_partition_erase(size_t n_args, const mp_obj_t *args) {
    espidf_partition_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    const esp_partition_t *part = espidf_partition_check(self);
    espidf_partition_check_writable(part);
    size_t offset = n_args > 1 ? (size_t)mp_obj_get_int(args[1]) : 0;
    size_t length = n_args > 2 ? (size_t)mp_obj_get_int(args[2]) : part->size - offset;
    if ((offset % SPI_FLASH_SEC_SIZE) != 0 || (length % SPI_FLASH_SEC_SIZE) != 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("offset and length must be a multiple of 4096"));
    }
    if (offset > part->size || length > part->size - offset) {
        mp_raise_ValueError(MP_ERROR_TEXT("erase past the end of the partition"));
    }
    CHECK_ESP_RESULT(esp_partition_erase_range(part, offset, length));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(espidf_partition_erase_obj, 1, 3, espidf_partition_erase);

//|     def mmap(self) -> memoryview:
//|         """Map the partition into the address space and return a read only
//|         memoryview over it. Nothing is copied and nothing lands on the heap, so
//|         this is the way to read tables that would not fit there.
//|
//|         The mapping is released by `deinit`."""
//|         ...
static mp_obj_t espidf_partition_mmap(mp_obj_t self_in) {
    espidf_partition_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const esp_partition_t *part = espidf_partition_check(self);
    if (self->mapped) {
        mp_raise_ValueError(MP_ERROR_TEXT("already mapped"));
    }
    const void *ptr = NULL;
    CHECK_ESP_RESULT(esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA,
        &ptr, &self->map_handle));
    self->mapped = true;
    // Without MP_OBJ_ARRAY_TYPECODE_FLAG_RW the memoryview refuses to be written,
    // which is what we want: the mapping really is read only.
    return mp_obj_new_memoryview('B', part->size, (void *)ptr);
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_partition_mmap_obj, espidf_partition_mmap);

//|     def deinit(self) -> None:
//|         """Release the mapping, if there is one. Any memoryview `mmap` returned
//|         must not be used afterwards."""
//|         ...
static mp_obj_t espidf_partition_deinit(mp_obj_t self_in) {
    espidf_partition_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->mapped) {
        esp_partition_munmap(self->map_handle);
        self->mapped = false;
    }
    self->part = NULL;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_partition_deinit_obj, espidf_partition_deinit);

static mp_obj_t espidf_partition_get_label(mp_obj_t self_in) {
    espidf_partition_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const esp_partition_t *part = espidf_partition_check(self);
    return mp_obj_new_str(part->label, strlen(part->label));
}
MP_DEFINE_CONST_FUN_OBJ_1(espidf_partition_get_label_obj, espidf_partition_get_label);
MP_PROPERTY_GETTER(espidf_partition_label_obj, (mp_obj_t)&espidf_partition_get_label_obj);

static mp_obj_t espidf_partition_get_size(mp_obj_t self_in) {
    espidf_partition_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_int_from_uint(espidf_partition_check(self)->size);
}
MP_DEFINE_CONST_FUN_OBJ_1(espidf_partition_get_size_obj, espidf_partition_get_size);
MP_PROPERTY_GETTER(espidf_partition_size_obj, (mp_obj_t)&espidf_partition_get_size_obj);

static mp_obj_t espidf_partition_get_address(mp_obj_t self_in) {
    espidf_partition_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_int_from_uint(espidf_partition_check(self)->address);
}
MP_DEFINE_CONST_FUN_OBJ_1(espidf_partition_get_address_obj, espidf_partition_get_address);
MP_PROPERTY_GETTER(espidf_partition_address_obj, (mp_obj_t)&espidf_partition_get_address_obj);

static const mp_rom_map_elem_t espidf_partition_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&espidf_partition_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&espidf_partition_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_erase), MP_ROM_PTR(&espidf_partition_erase_obj) },
    { MP_ROM_QSTR(MP_QSTR_mmap), MP_ROM_PTR(&espidf_partition_mmap_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&espidf_partition_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR_label), MP_ROM_PTR(&espidf_partition_label_obj) },
    { MP_ROM_QSTR(MP_QSTR_size), MP_ROM_PTR(&espidf_partition_size_obj) },
    { MP_ROM_QSTR(MP_QSTR_address), MP_ROM_PTR(&espidf_partition_address_obj) },
};
static MP_DEFINE_CONST_DICT(espidf_partition_locals_dict, espidf_partition_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    espidf_partition_type,
    MP_QSTR_Partition,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, espidf_partition_make_new,
    locals_dict, &espidf_partition_locals_dict
    );

//| def partitions() -> List[Tuple[str, int, int, int, int]]:
//|     """Every entry of the partition table as
//|     ``(label, type, subtype, address, size)``."""
//|     ...
//|
//|
static mp_obj_t espidf_partitions(void) {
    mp_obj_t list = mp_obj_new_list(0, NULL);
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY,
        ESP_PARTITION_SUBTYPE_ANY, NULL);
    while (it != NULL) {
        const esp_partition_t *part = esp_partition_get(it);
        mp_obj_t entry[5] = {
            mp_obj_new_str(part->label, strlen(part->label)),
            MP_OBJ_NEW_SMALL_INT(part->type),
            MP_OBJ_NEW_SMALL_INT(part->subtype),
            mp_obj_new_int_from_uint(part->address),
            mp_obj_new_int_from_uint(part->size),
        };
        mp_obj_list_append(list, mp_obj_new_tuple(5, entry));
        it = esp_partition_next(it);
    }
    esp_partition_iterator_release(it);
    return list;
}
static MP_DEFINE_CONST_FUN_OBJ_0(espidf_partitions_obj, espidf_partitions);

//| def running_partition() -> str:
//|     """The label of the partition the firmware is running from. Writing to it
//|     is refused, so this is the one to avoid when picking a partition for data.
//|     Detect it with this, never by trying to erase."""
//|     ...
//|
//|
static mp_obj_t espidf_running_partition(void) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL) {
        return mp_const_none;
    }
    return mp_obj_new_str(running->label, strlen(running->label));
}
static MP_DEFINE_CONST_FUN_OBJ_0(espidf_running_partition_obj, espidf_running_partition);

#if CIRCUITPY_PARTITION_DISK
// CIRCUITPY-CHANGE: mount a partition's filesystem so that USB can serve it too.
//| def expose_partition(
//|     label: str, path: str = "/data", *, usb_writable: bool = False, format: bool = False
//| ) -> None:
//|     """Mount the FAT filesystem on a partition at ``path``, and make it a second
//|     USB drive.
//|
//|     Call this from ``boot.py``: CircuitPython presents itself to the host after
//|     boot.py runs, so that is the only place the set of USB drives can be
//|     decided. Calling it from ``code.py`` mounts the filesystem but the host will
//|     not see the drive.
//|
//|     ``usb_writable`` picks the direction, because only one side may write a FAT
//|     filesystem at a time. False (the default) lets CircuitPython write — for
//|     logging — and the host sees it read-only. True lets the host write and
//|     CircuitPython sees it read-only.
//|
//|     With ``format`` the partition is formatted first, destroying what was there.
//|     Do that once, then mount without it.
//|
//|     Raises ValueError if there is no such partition, and OSError if it holds no
//|     filesystem yet (format it once)."""
//|     ...
//|
//|
static mp_obj_t espidf_expose_partition(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_label, ARG_path, ARG_usb_writable, ARG_format };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_label, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_path, MP_ARG_OBJ, { .u_obj = MP_ROM_NONE } },
        { MP_QSTR_usb_writable, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = false } },
        { MP_QSTR_format, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = false } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    const char *label = mp_obj_str_get_str(args[ARG_label].u_obj);
    const char *path = args[ARG_path].u_obj == MP_ROM_NONE
        ? "/data" : mp_obj_str_get_str(args[ARG_path].u_obj);

    partition_disk_result_t result;
    if (args[ARG_format].u_bool) {
        if (!partition_disk_format(label, &result)) {
            if (result == PARTITION_DISK_NO_PARTITION) {
                mp_raise_ValueError(MP_ERROR_TEXT("no such partition"));
            }
            mp_raise_OSError(MP_EIO);
        }
    }

    if (!partition_disk_mount(label, path, args[ARG_usb_writable].u_bool, &result)) {
        switch (result) {
            case PARTITION_DISK_NO_PARTITION:
                mp_raise_ValueError(MP_ERROR_TEXT("no such partition"));
            case PARTITION_DISK_BAD_PATH:
                mp_raise_ValueError(MP_ERROR_TEXT("path must be absolute and short"));
            case PARTITION_DISK_ALREADY_MOUNTED:
                mp_raise_ValueError(MP_ERROR_TEXT("already mounted"));
            case PARTITION_DISK_NO_FILESYSTEM:
            default:
                // No filesystem: the caller should pass format=True once.
                mp_raise_OSError(MP_ENODEV);
        }
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(espidf_expose_partition_obj, 1, espidf_expose_partition);
#endif

// CIRCUITPY-CHANGE: automatic frequency scaling and light sleep.
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
static MP_DEFINE_CONST_FUN_OBJ_KW(espidf_power_management_obj, 0, espidf_power_management);

// CIRCUITPY-CHANGE: esp_timer, exposed as a class.
//
// The ESP-IDF callback runs in the timer's own task, where nothing may touch the
// interpreter. So it only queues a background callback, which CircuitPython runs
// on the Python thread between two bytecodes. That means the callback is not an
// interrupt: it cannot pre-empt Python code, and it is delayed by anything that
// blocks without running background tasks.
//
// A running timer is held by a list that the collector traces, so it stays alive
// without the program keeping a reference. stop() and deinit() drop it again.

typedef struct {
    mp_obj_base_t base;
    esp_timer_handle_t handle;
    mp_obj_t callback;
    background_callback_t background;
    uint64_t period_us;
    bool repeat;
    bool running;
} espidf_timer_obj_t;

// A running timer must not be collected while the ESP-IDF still holds a pointer
// to it, so it is kept in a list the collector traces.
MP_REGISTER_ROOT_POINTER(mp_obj_t espidf_active_timers);

// The handles are kept a second time, outside the heap. Soft reset has to take
// the ESP-IDF timers down, and by then the list above may already be gone: the
// first version of this walked it during reset and kept firing into freed memory
// afterwards.
#define ESPIDF_MAX_TIMERS (8)
static esp_timer_handle_t espidf_live_handles[ESPIDF_MAX_TIMERS];

// Claims a slot and returns it, or raises. The handle is written into the slot
// once it exists; a reserved but unfilled slot is simply NULL again on the next
// call, so a raise between the two is harmless.
static size_t espidf_handle_reserve(void) {
    for (size_t i = 0; i < ESPIDF_MAX_TIMERS; i++) {
        if (espidf_live_handles[i] == NULL) {
            return i;
        }
    }
    mp_raise_RuntimeError(MP_ERROR_TEXT("too many timers"));
}

static void espidf_handle_untrack(esp_timer_handle_t handle) {
    for (size_t i = 0; i < ESPIDF_MAX_TIMERS; i++) {
        if (espidf_live_handles[i] == handle) {
            espidf_live_handles[i] = NULL;
            return;
        }
    }
}

static void espidf_timer_track(espidf_timer_obj_t *self) {
    if (MP_STATE_VM(espidf_active_timers) == MP_OBJ_NULL) {
        MP_STATE_VM(espidf_active_timers) = mp_obj_new_list(0, NULL);
    }
    mp_obj_list_append(MP_STATE_VM(espidf_active_timers), MP_OBJ_FROM_PTR(self));
}

static void espidf_timer_untrack(espidf_timer_obj_t *self) {
    mp_obj_t list_obj = MP_STATE_VM(espidf_active_timers);
    if (list_obj == MP_OBJ_NULL) {
        return;
    }
    size_t len;
    mp_obj_t *items;
    mp_obj_list_get(list_obj, &len, &items);
    for (size_t i = 0; i < len; i++) {
        if (items[i] == MP_OBJ_FROM_PTR(self)) {
            mp_obj_list_remove(list_obj, items[i]);
            return;
        }
    }
}

// Called on soft reset. Only the handles kept outside the heap are touched here,
// because the objects they belong to may already be gone.
void espidf_timer_reset(void) {
    for (size_t i = 0; i < ESPIDF_MAX_TIMERS; i++) {
        if (espidf_live_handles[i] != NULL) {
            esp_timer_stop(espidf_live_handles[i]);
            esp_timer_delete(espidf_live_handles[i]);
            espidf_live_handles[i] = NULL;
        }
    }
    MP_STATE_VM(espidf_active_timers) = MP_OBJ_NULL;
}

static void espidf_timer_run(void *data) {
    espidf_timer_obj_t *self = data;
    if (self->callback == mp_const_none) {
        return;
    }
    if (!self->repeat) {
        self->running = false;
        espidf_timer_untrack(self);
    }
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_call_function_0(self->callback);
        nlr_pop();
    } else {
        // Nothing above this can handle it, so report it the way an unhandled
        // exception in the main program is reported and keep the timer going.
        mp_obj_print_exception(&mp_plat_print, MP_OBJ_FROM_PTR(nlr.ret_val));
    }
}

static void espidf_timer_isr(void *arg) {
    espidf_timer_obj_t *self = arg;
    background_callback_add(&self->background, espidf_timer_run, self);
}

//| class Timer:
//|     """A periodic or one-shot timer backed by the ESP-IDF esp_timer.
//|
//|     The callback does not run in interrupt context. It is queued and runs on the
//|     Python thread between two bytecodes, so it cannot interrupt running code and
//|     it is delayed while something blocks without servicing background tasks."""
//|
//|     def __init__(self, callback: Callable[[], None], period: float, *, repeat: bool = True) -> None:
//|         """Create a timer and start it. ``period`` is in seconds and is rounded to
//|         whole microseconds."""
//|         ...
static mp_obj_t espidf_timer_make_new(const mp_obj_type_t *type, size_t n_args,
    size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_callback, ARG_period, ARG_repeat };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_callback, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_period, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_repeat, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = true } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    if (!mp_obj_is_callable(args[ARG_callback].u_obj)) {
        mp_raise_TypeError(MP_ERROR_TEXT("callback must be callable"));
    }
    mp_float_t period = mp_obj_get_float(args[ARG_period].u_obj);
    if (!(period > 0)) {
        mp_raise_ValueError(MP_ERROR_TEXT("period must be positive"));
    }
    uint64_t period_us = (uint64_t)(period * 1000000 + (mp_float_t)0.5);
    if (period_us == 0) {
        period_us = 1;
    }

    espidf_timer_obj_t *self = mp_obj_malloc(espidf_timer_obj_t, &espidf_timer_type);
    self->callback = args[ARG_callback].u_obj;
    self->period_us = period_us;
    self->repeat = args[ARG_repeat].u_bool;
    self->running = false;
    self->handle = NULL;

    esp_timer_create_args_t timer_args = {
        .callback = espidf_timer_isr,
        .arg = self,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "CircuitPython espidf.Timer",
    };
    // Take the slot before creating anything, so that running out of slots does
    // not leave an ESP-IDF timer behind with nothing referring to it.
    size_t slot = espidf_handle_reserve();

    CHECK_ESP_RESULT(esp_timer_create(&timer_args, &self->handle));
    espidf_live_handles[slot] = self->handle;
    espidf_timer_track(self);
    self->running = true;
    esp_err_t err = self->repeat
        ? esp_timer_start_periodic(self->handle, period_us)
        : esp_timer_start_once(self->handle, period_us);
    if (err != ESP_OK) {
        self->running = false;
        espidf_timer_untrack(self);
        esp_timer_delete(self->handle);
        espidf_handle_untrack(self->handle);
        self->handle = NULL;
        CHECK_ESP_RESULT(err);
    }
    return MP_OBJ_FROM_PTR(self);
}

//|     def stop(self) -> None:
//|         """Stop the timer. It can be started again with ``start()``."""
//|         ...
static mp_obj_t espidf_timer_stop(mp_obj_t self_in) {
    espidf_timer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->handle != NULL && self->running) {
        esp_timer_stop(self->handle);
        self->running = false;
        espidf_timer_untrack(self);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_timer_stop_obj, espidf_timer_stop);

//|     def start(self) -> None:
//|         """Start the timer again with the period it was created with."""
//|         ...
static mp_obj_t espidf_timer_start(mp_obj_t self_in) {
    espidf_timer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->handle == NULL) {
        mp_raise_ValueError(MP_ERROR_TEXT("Timer is deinitialized"));
    }
    if (!self->running) {
        espidf_timer_track(self);
        self->running = true;
        esp_err_t err = self->repeat
            ? esp_timer_start_periodic(self->handle, self->period_us)
            : esp_timer_start_once(self->handle, self->period_us);
        if (err != ESP_OK) {
            self->running = false;
            espidf_timer_untrack(self);
            CHECK_ESP_RESULT(err);
        }
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_timer_start_obj, espidf_timer_start);

//|     def deinit(self) -> None:
//|         """Stop the timer and release it."""
//|         ...
static mp_obj_t espidf_timer_deinit(mp_obj_t self_in) {
    espidf_timer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->handle != NULL) {
        esp_timer_stop(self->handle);
        esp_timer_delete(self->handle);
        espidf_handle_untrack(self->handle);
        self->handle = NULL;
        self->running = false;
        self->callback = mp_const_none;
        espidf_timer_untrack(self);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_timer_deinit_obj, espidf_timer_deinit);

//|     active: bool
//|     """Whether the timer is currently running. Read-only."""
static mp_obj_t espidf_timer_get_active(mp_obj_t self_in) {
    espidf_timer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_bool(self->running);
}
MP_DEFINE_CONST_FUN_OBJ_1(espidf_timer_get_active_obj, espidf_timer_get_active);
MP_PROPERTY_GETTER(espidf_timer_active_obj, (mp_obj_t)&espidf_timer_get_active_obj);

static const mp_rom_map_elem_t espidf_timer_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&espidf_timer_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_start), MP_ROM_PTR(&espidf_timer_start_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&espidf_timer_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR_active), MP_ROM_PTR(&espidf_timer_active_obj) },
};
static MP_DEFINE_CONST_DICT(espidf_timer_locals_dict, espidf_timer_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    espidf_timer_type,
    MP_QSTR_Timer,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, espidf_timer_make_new,
    locals_dict, &espidf_timer_locals_dict
    );

static const mp_rom_map_elem_t espidf_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_espidf) },

    { MP_ROM_QSTR(MP_QSTR_heap_caps_get_total_size), MP_ROM_PTR(&espidf_heap_caps_get_total_size_obj)},
    { MP_ROM_QSTR(MP_QSTR_heap_caps_get_free_size), MP_ROM_PTR(&espidf_heap_caps_get_free_size_obj)},
    { MP_ROM_QSTR(MP_QSTR_heap_caps_get_largest_free_block), MP_ROM_PTR(&espidf_heap_caps_get_largest_free_block_obj)},

    { MP_ROM_QSTR(MP_QSTR_erase_nvs), MP_ROM_PTR(&espidf_erase_nvs_obj)},

    { MP_ROM_QSTR(MP_QSTR_get_total_psram), MP_ROM_PTR(&espidf_get_total_psram_obj)},

    { MP_ROM_QSTR(MP_QSTR_get_time_us), MP_ROM_PTR(&espidf_get_time_us_obj)},
    { MP_ROM_QSTR(MP_QSTR_get_cycle_count), MP_ROM_PTR(&espidf_get_cycle_count_obj)},
    { MP_ROM_QSTR(MP_QSTR_task_stats), MP_ROM_PTR(&espidf_task_stats_obj)},
    { MP_ROM_QSTR(MP_QSTR_prof_stats), MP_ROM_PTR(&espidf_prof_stats_obj)},
    { MP_ROM_QSTR(MP_QSTR_prof_reset), MP_ROM_PTR(&espidf_prof_reset_obj)},
    { MP_ROM_QSTR(MP_QSTR_profiler_start), MP_ROM_PTR(&espidf_profiler_start_obj)},
    { MP_ROM_QSTR(MP_QSTR_profiler_stop), MP_ROM_PTR(&espidf_profiler_stop_obj)},
    { MP_ROM_QSTR(MP_QSTR_profiler_data), MP_ROM_PTR(&espidf_profiler_data_obj)},
    { MP_ROM_QSTR(MP_QSTR_bench_setjmp), MP_ROM_PTR(&espidf_bench_setjmp_obj)},
    { MP_ROM_QSTR(MP_QSTR_bench_div), MP_ROM_PTR(&espidf_bench_div_obj)},

    { MP_ROM_QSTR(MP_QSTR_Timer), MP_ROM_PTR(&espidf_timer_type) },
    { MP_ROM_QSTR(MP_QSTR_Partition), MP_ROM_PTR(&espidf_partition_type) },
    { MP_ROM_QSTR(MP_QSTR_partitions), MP_ROM_PTR(&espidf_partitions_obj) },
    { MP_ROM_QSTR(MP_QSTR_running_partition), MP_ROM_PTR(&espidf_running_partition_obj) },
    #if CIRCUITPY_PARTITION_DISK
    { MP_ROM_QSTR(MP_QSTR_expose_partition), MP_ROM_PTR(&espidf_expose_partition_obj) },
    #endif
    { MP_ROM_QSTR(MP_QSTR_power_management), MP_ROM_PTR(&espidf_power_management_obj) },
    { MP_ROM_QSTR(MP_QSTR_check_heap), MP_ROM_PTR(&espidf_check_heap_obj) },
    { MP_ROM_QSTR(MP_QSTR_pin_status), MP_ROM_PTR(&espidf_pin_status_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_vendor_ie), MP_ROM_PTR(&espidf_set_vendor_ie_obj) },
    { MP_ROM_QSTR(MP_QSTR_NVS), MP_ROM_PTR(&espidf_nvs_type) },
    { MP_ROM_QSTR(MP_QSTR_EventQueue), MP_ROM_PTR(&espidf_event_queue_type) },
    #if CIRCUITPY_WIFI
    { MP_ROM_QSTR(MP_QSTR_eap_enable), MP_ROM_PTR(&espidf_eap_enable_obj) },
    { MP_ROM_QSTR(MP_QSTR_eap_disable), MP_ROM_PTR(&espidf_eap_disable_obj) },
    { MP_ROM_QSTR(MP_QSTR_TTLS_PHASE2_EAP), MP_ROM_INT(ESP_EAP_TTLS_PHASE2_EAP) },
    { MP_ROM_QSTR(MP_QSTR_TTLS_PHASE2_MSCHAPV2), MP_ROM_INT(ESP_EAP_TTLS_PHASE2_MSCHAPV2) },
    { MP_ROM_QSTR(MP_QSTR_TTLS_PHASE2_MSCHAP), MP_ROM_INT(ESP_EAP_TTLS_PHASE2_MSCHAP) },
    { MP_ROM_QSTR(MP_QSTR_TTLS_PHASE2_PAP), MP_ROM_INT(ESP_EAP_TTLS_PHASE2_PAP) },
    { MP_ROM_QSTR(MP_QSTR_TTLS_PHASE2_CHAP), MP_ROM_INT(ESP_EAP_TTLS_PHASE2_CHAP) },
    { MP_ROM_QSTR(MP_QSTR_smartconfig_start), MP_ROM_PTR(&espidf_smartconfig_start_obj) },
    { MP_ROM_QSTR(MP_QSTR_smartconfig_result), MP_ROM_PTR(&espidf_smartconfig_result_obj) },
    { MP_ROM_QSTR(MP_QSTR_smartconfig_stop), MP_ROM_PTR(&espidf_smartconfig_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_SC_TYPE_ESPTOUCH), MP_ROM_INT(SC_TYPE_ESPTOUCH) },
    { MP_ROM_QSTR(MP_QSTR_SC_TYPE_AIRKISS), MP_ROM_INT(SC_TYPE_AIRKISS) },
    { MP_ROM_QSTR(MP_QSTR_SC_TYPE_ESPTOUCH_AIRKISS), MP_ROM_INT(SC_TYPE_ESPTOUCH_AIRKISS) },
    { MP_ROM_QSTR(MP_QSTR_SC_TYPE_ESPTOUCH_V2), MP_ROM_INT(SC_TYPE_ESPTOUCH_V2) },
    { MP_ROM_QSTR(MP_QSTR_SC_EVENT), MP_ROM_QSTR(MP_QSTR_SC_EVENT) },
    #endif
    { MP_ROM_QSTR(MP_QSTR_ANY_ID), MP_ROM_INT(ESP_EVENT_ANY_ID) },
    #if CIRCUITPY_WIFI
    { MP_ROM_QSTR(MP_QSTR_WIFI_EVENT), MP_ROM_QSTR(MP_QSTR_WIFI_EVENT) },
    { MP_ROM_QSTR(MP_QSTR_IP_EVENT), MP_ROM_QSTR(MP_QSTR_IP_EVENT) },
    #endif
    { MP_ROM_QSTR(MP_QSTR_pbkdf2), MP_ROM_PTR(&espidf_pbkdf2_obj) },
    { MP_ROM_QSTR(MP_QSTR_hmac_sha256), MP_ROM_PTR(&espidf_hmac_sha256_obj) },
    { MP_ROM_QSTR(MP_QSTR_aes_gcm_encrypt), MP_ROM_PTR(&espidf_aes_gcm_encrypt_obj) },
    { MP_ROM_QSTR(MP_QSTR_aes_gcm_decrypt), MP_ROM_PTR(&espidf_aes_gcm_decrypt_obj) },
    { MP_ROM_QSTR(MP_QSTR_wifi_raw_tx), MP_ROM_PTR(&espidf_wifi_raw_tx_obj) },
    #if CIRCUITPY_WIFI
    { MP_ROM_QSTR(MP_QSTR_wifi_sleep_min_active_time), MP_ROM_PTR(&espidf_wifi_sleep_min_active_time_obj) },
    #endif
    #if CIRCUITPY_BLEIO_NATIVE
    { MP_ROM_QSTR(MP_QSTR_ble_prefer_phy), MP_ROM_PTR(&espidf_ble_prefer_phy_obj) },
    #endif
    #if CIRCUITPY_ESPIDF_CSI
    { MP_ROM_QSTR(MP_QSTR_CSI), MP_ROM_PTR(&espidf_csi_type) },
    #endif

    { MP_ROM_QSTR(MP_QSTR_IDFError), MP_ROM_PTR(&mp_type_espidf_IDFError) },
    { MP_ROM_QSTR(MP_QSTR_MemoryError),      MP_ROM_PTR(&mp_type_espidf_MemoryError) },
};

static MP_DEFINE_CONST_DICT(espidf_module_globals, espidf_module_globals_table);

const mp_obj_module_t espidf_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&espidf_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_espidf, espidf_module);
