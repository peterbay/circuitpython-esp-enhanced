// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdint.h>

// Cycle counting probes for finding where a refresh actually spends its time.
// Off by default; a build turns them on with CIRCUITPY_PROF=1 and reads the
// counters back through espidf.prof_stats().
//
// Only put probes on code that runs tens of times per frame. A probe pair costs
// about ten cycles, which is nothing next to a chunk of pixel conversion but
// would swamp something called once per bytecode.
#ifndef CIRCUITPY_PROF
#define CIRCUITPY_PROF (0)
#endif

typedef enum {
    PROF_REFRESH_AREA,  // whole _refresh_area, once per dirty area
    PROF_FILL_AREA,     // pixel conversion of one subrectangle
    PROF_SET_REGION,    // column and page window commands
    PROF_SEND_PIXELS,   // handing one subrectangle to the bus, includes waiting
    PROF_GET_AREAS,     // walking the group for dirty areas, once per refresh
    PROF_AREA_SETUP,    // clipping and chunk arithmetic before the subrectangles
    PROF_CHUNK_BUS,     // per subrectangle: waiting out the previous transfer and
                        // closing and reopening the bus transaction around it
    PROF_GC_COLLECT,    // one garbage collection
    PROF_COUNT,
} prof_id_t;

// Each sample is a program counter and the return address of the function it was
// in, which is what lets a sample in ROM be blamed on its caller. 1024 pairs is
// 8 KB of RAM and, at two kilohertz, about half a second of wall time.
#define PROF_MAX_SAMPLES (1024)

#if CIRCUITPY_PROF

#include <stdbool.h>
#include <stddef.h>

extern uint64_t prof_cycles[PROF_COUNT];
extern uint32_t prof_calls[PROF_COUNT];
extern const char *const prof_names[PROF_COUNT];

uint32_t prof_now(void);
void prof_reset(void);

// Statistical profiler: samples the program counter of the task that starts it.
bool prof_sampler_start(uint32_t hz);
void prof_sampler_stop(void);
const uint32_t *prof_sampler_data(size_t *count, size_t *waiting, size_t *lost);

#define PROF_BEGIN(id) uint32_t _prof_start_##id = prof_now()
#define PROF_END(id) do { \
        prof_cycles[id] += (uint32_t)(prof_now() - _prof_start_##id); \
        prof_calls[id]++; \
} while (0)

#else

#define PROF_BEGIN(id) ((void)0)
#define PROF_END(id) ((void)0)

#endif
