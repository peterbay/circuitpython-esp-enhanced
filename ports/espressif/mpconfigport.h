// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2015 Glenn Ruben Bakke
// SPDX-FileCopyrightText: Copyright (c) 2019 Dan Halbert for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#pragma once

// Enable for debugging.
// #define CIRCUITPY_VERBOSE_BLE               (1)

#define MICROPY_NLR_THUMB                   (0)

#define MICROPY_USE_INTERNAL_PRINTF         (0)
#define MICROPY_PY_SYS_PLATFORM             "Espressif"

#define CIRCUITPY_DIGITALIO_HAVE_INPUT_ONLY (1)

#include "py/circuitpy_mpconfig.h"

// The VM runs RUN_BACKGROUND_TASKS on every jump, every loop iteration and every
// method call. That is a call into flash to background_callback_run_all(), which
// unconditionally calls port_background_task() before it even looks at the queue.
// port_background_task() is empty on this port (background.c), so test the queue
// inline and only pay for the call when there is actually work to run. Same
// latency, same semantics, one load instead of two out of line calls.
struct background_callback;
extern volatile struct background_callback *volatile callback_head;
#undef MICROPY_VM_HOOK_LOOP
#undef MICROPY_VM_HOOK_RETURN
#define MICROPY_VM_HOOK_LOOP if (callback_head != NULL) { RUN_BACKGROUND_TASKS; }
#define MICROPY_VM_HOOK_RETURN if (callback_head != NULL) { RUN_BACKGROUND_TASKS; }

#define MICROPY_NLR_SETJMP                  (1)
// Xtensa has register windows, so the libc setjmp flushes the whole register file
// on every nlr_push, which is every python function call. Measured 305 cycles
// against 6 for the compiler's own non-local goto, which defers the flush to the
// jump. See py/nlr.h.
#define MICROPY_NLR_SETJMP_BUILTIN          (1)

// Builtin names cost two map lookups, the first of which always misses. Measured
// on this board: reading a plain global 411 ns, reading "len" 2141 ns.
#define MICROPY_OPT_LOAD_GLOBAL_CACHE       (1)

// A method call costs 1033 cycles against 537 for a plain call, and the gap is
// mostly the six calls the general path walks to reach two map lookups.
#define MICROPY_OPT_LOAD_METHOD_FAST_PATH   (1)

// Reaching a bytecode function through mp_call_function_n_kw costs a frame, a
// type lookup and an indirect jump the branch predictor cannot follow.
#define MICROPY_OPT_CALL_FUN_BC_FAST_PATH   (1)

// A bytearray store costs 410 cycles against 102 for a list index.
#define MICROPY_OPT_BYTEARRAY_SUBSCR_FAST_PATH (1)

// Building a str costs 5605 cycles against 1200 for the same bytes, and the
// whole difference is the qstr pool search. See py/objstr.c.
#define MICROPY_OPT_STR_NO_INTERN           (1)

// s[3] costs 712 cycles and s[0] 1782, the difference being where the character
// sits in the qstr pools. 256 bytes of RAM removes the search entirely.
#define MICROPY_OPT_SINGLE_CHAR_QSTR_CACHE  (1)

// len(x) costs 422 cycles, most of it reaching the C function at all.
#define MICROPY_OPT_CALL_BUILTIN_FAST_PATH  (1)

// One pass of an empty loop costs 260 cycles, and every loop pays it.
#define MICROPY_OPT_ITERNEXT_FAST_PATH      (1)

// d["x"] costs 422 cycles, of which the generic subscript path is most of it.
#define MICROPY_OPT_DICT_SUBSCR_FAST_PATH   (1)

// "if a_list:" costs 235 cycles against 102 for "if an_int:".
#define MICROPY_OPT_TRUTH_FAST_PATH         (1)

#define CIRCUITPY_DEFAULT_STACK_SIZE        0x6000

// PSRAM can require more stack space for GC.
// CIRCUITPY-CHANGE, note only: this value is where the collector falls off a
// cliff. gc_mark_subtree pushes one entry per unmarked child while scanning a
// parent, so a container of more than this many objects overflows on the first
// parent it touches, and gc_deal_with_stack_overflow then rescans the used region
// looking for marked blocks whose children were never followed. Timing
// gc.collect() from Python against a flat list of N small objects: 1.53 us each
// at N=120, 4.29 us at N=128, and flat at ~3.5 us above that.
// Raising it to 512 was built and measured. It moves the cliff rather than
// removing it: collections of 128-384 live children got 1.6-1.9x faster, nothing
// above 512 improved, those were a consistent 4-8% slower, and it cost 3072 B of
// RAM. Left at 128; the fix worth having is a cheaper overflow path, not a bigger
// stack.
#define MICROPY_ALLOC_GC_STACK_SIZE         (128)

// Nearly all boards have this because it is used to enter the ROM bootloader.
#ifndef CIRCUITPY_BOOT_BUTTON
  #if defined(CONFIG_IDF_TARGET_ESP32C2) || defined(CONFIG_IDF_TARGET_ESP32C3) || defined(CONFIG_IDF_TARGET_ESP32C6) || defined(CONFIG_IDF_TARGET_ESP32H2) || defined(CONFIG_IDF_TARGET_ESP32C61)
    #define CIRCUITPY_BOOT_BUTTON (&pin_GPIO9)
  #elif !defined(CONFIG_IDF_TARGET_ESP32)
    #define CIRCUITPY_BOOT_BUTTON (&pin_GPIO0)
  #endif
#endif

#define CIRCUITPY_INTERNAL_NVM_START_ADDR (0x9000)

// 20kB is statically allocated to nvs, but when overwriting an existing
// item, it's temporarily necessary to store both the old and new copies.
// Additionally, there is some overhad for the names and values of items
// in nvs, and alignment to 4kB flash erase boundaries may give better
// performance characteristics (h/t @tannewt). This implies we should select an
// 8kB size for CircuitPython'ns NVM.
#ifndef CIRCUITPY_INTERNAL_NVM_SIZE
#define CIRCUITPY_INTERNAL_NVM_SIZE (8 * 1024)
#endif

// Define to (1) in mpconfigboard.h if the board has a defined I2C port that
// lacks pull up resistors (Espressif's HMI Devkit), and the internal pull-up
// resistors will be enabled for all busio.I2C objects. This is only to
// compensate for design decisions that are out of the control of the authors
// of CircuitPython and is not an endorsement of running without appropriate
// external pull up resistors.
#ifndef CIRCUITPY_I2C_ALLOW_INTERNAL_PULL_UP
#define CIRCUITPY_I2C_ALLOW_INTERNAL_PULL_UP (0)
#endif

// Protect the background queue with a lock because both cores may modify it.
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
extern portMUX_TYPE background_task_mutex;
#define CALLBACK_CRITICAL_BEGIN (taskENTER_CRITICAL(&background_task_mutex))
#define CALLBACK_CRITICAL_END (taskEXIT_CRITICAL(&background_task_mutex))

// 20 dBm is the default and the highest max tx power.
// Allow a different value to be specified for boards that have trouble with using the maximum power.
#ifndef CIRCUITPY_WIFI_DEFAULT_TX_POWER
#define CIRCUITPY_WIFI_DEFAULT_TX_POWER (20)
#endif

// CIRCUITPY-CHANGE: how much of each CSI record is kept. The radio can report
// more than this for wide channels; the tail is dropped.
//
// 128 covers HT20 and HT40 on the pre-Wi-Fi-6 parts, measured on an ESP32-S3.
// An HE20 record carries 242 tones rather than 64, so Wi-Fi 6 parts need room
// for roughly four times as much. The figure below is sized from the tone count
// and has not been checked against hardware.
#ifndef ESPIDF_CSI_MAX_BYTES
// defined() rather than a bare test: the build runs with -Wundef -Werror, and
// the macro is simply absent on parts without Wi-Fi 6.
#if defined(CONFIG_SOC_WIFI_HE_SUPPORT)
#define ESPIDF_CSI_MAX_BYTES (512)
#else
#define ESPIDF_CSI_MAX_BYTES (128)
#endif
#endif

#ifndef CIRCUITPY_ESP32P4_SWAP_LSFS
#define CIRCUITPY_ESP32P4_SWAP_LSFS (0)
#endif

