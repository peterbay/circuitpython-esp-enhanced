// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-License-Identifier: MIT

#include "supervisor/prof.h"

#if CIRCUITPY_PROF

#include "esp_cpu.h"

uint64_t prof_cycles[PROF_COUNT];
uint32_t prof_calls[PROF_COUNT];

const char *const prof_names[PROF_COUNT] = {
    "refresh_area",
    "fill_area",
    "set_region",
    "send_pixels",
    "get_areas",
    "area_setup",
    "chunk_bus",
    "gc_collect",
};

// Reading the cycle counter is a single instruction, so a probe pair costs about
// as much as the two additions that follow it.
uint32_t prof_now(void) {
    return esp_cpu_get_cycle_count();
}

void prof_reset(void) {
    for (size_t i = 0; i < PROF_COUNT; i++) {
        prof_cycles[i] = 0;
        prof_calls[i] = 0;
    }
}

// --- statistical sampling profiler ------------------------------------------
//
// A sampler task sits on the same core as CircuitPython at a higher priority. A
// timer wakes it, which preempts CircuitPython, so the interpreter's context is
// already saved on its own stack and the program counter can be read out of it.
// Both saved frame layouts put exit at offset 0 and pc at offset 4, and exit is
// zero only when the task gave the core up itself, which separates "was running"
// from "was waiting" without any extra bookkeeping.
//
// The addresses mean nothing on their own; symbolise them on the host against
// firmware.elf with xtensa-esp32s3-elf-addr2line.

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

// Pairs of program counter and return address.
static uint32_t prof_samples[PROF_MAX_SAMPLES * 2];
static volatile size_t prof_sample_count;
static volatile size_t prof_waiting_count;
static volatile size_t prof_lost_count;
static volatile bool prof_sampling;

static TaskHandle_t prof_target;
static TaskHandle_t prof_sampler;
static esp_timer_handle_t prof_timer;

static void prof_sampler_task(void *arg) {
    (void)arg;
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!prof_sampling || prof_target == NULL) {
            continue;
        }
        // Running means it is on the other core right now, where the saved
        // context is stale. Anything else has a valid frame to read.
        if (eTaskGetState(prof_target) == eRunning) {
            prof_lost_count++;
            continue;
        }
        // pxTopOfStack is the first member of the task control block; the port's
        // context switch assembly depends on that, so it is safe to rely on.
        uint32_t *frame = *(uint32_t **)prof_target;
        if (frame == NULL) {
            prof_lost_count++;
            continue;
        }
        if (frame[0] == 0) {
            prof_waiting_count++;
        } else if (prof_sample_count < PROF_MAX_SAMPLES) {
            // Interrupt frame: pc at word 1, and a0 at word 3 still holds the
            // return address of whatever was running.
            prof_samples[prof_sample_count * 2] = frame[1];
            prof_samples[prof_sample_count * 2 + 1] = frame[3];
            prof_sample_count++;
        } else {
            prof_lost_count++;
        }
    }
}

static void prof_tick(void *arg) {
    (void)arg;
    if (prof_sampler != NULL) {
        xTaskNotifyGive(prof_sampler);
    }
}

bool prof_sampler_start(uint32_t hz) {
    if (prof_sampling) {
        return true;
    }
    if (hz < 10 || hz > 20000) {
        return false;
    }
    prof_sample_count = 0;
    prof_waiting_count = 0;
    prof_lost_count = 0;
    // Started from the interpreter, so this is the task and the core to watch.
    prof_target = xTaskGetCurrentTaskHandle();

    if (prof_sampler == NULL) {
        // One above the interpreter is enough to preempt it, and low enough to
        // stay out of the way of the drivers.
        if (xTaskCreatePinnedToCore(prof_sampler_task, "prof", 2048, NULL,
            uxTaskPriorityGet(prof_target) + 1, &prof_sampler, xPortGetCoreID()) != pdPASS) {
            prof_sampler = NULL;
            return false;
        }
    }
    if (prof_timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = prof_tick,
            .name = "prof",
        };
        if (esp_timer_create(&args, &prof_timer) != ESP_OK) {
            return false;
        }
    }
    prof_sampling = true;
    // Period deliberately not a whole millisecond: the FreeRTOS tick runs at
    // 1 kHz and workloads tend to line up with it, which would alias.
    if (esp_timer_start_periodic(prof_timer, 1000000 / hz + 1) != ESP_OK) {
        prof_sampling = false;
        return false;
    }
    return true;
}

void prof_sampler_stop(void) {
    if (!prof_sampling) {
        return;
    }
    prof_sampling = false;
    esp_timer_stop(prof_timer);
}

const uint32_t *prof_sampler_data(size_t *count, size_t *waiting, size_t *lost) {
    *count = prof_sample_count;
    *waiting = prof_waiting_count;
    *lost = prof_lost_count;
    return prof_samples;
}

#endif
