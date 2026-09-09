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
    // Interpreter paths. These run once per bytecode or more, so the probe pair
    // is a noticeable share of what it measures; the numbers say where the time
    // goes, not what it would cost without the probes.
    PROF_GC_ALLOC,          // one heap allocation
    PROF_CLASS_LOOKUP,      // mp_obj_class_lookup: a name searched in a class and its bases
    PROF_MAP_LOOKUP,        // mp_map_lookup, every dict and members lookup that reaches it
    PROF_LOAD_METHOD,       // mp_load_method, the general path of LOAD_METHOD
    PROF_LOAD_ATTR,         // mp_load_attr, the general path of LOAD_ATTR
    PROF_STORE_ATTR,        // mp_store_attr, the general path of STORE_ATTR
    PROF_BINARY_OP,         // mp_binary_op, the general path of operators
    PROF_SETUP_CODE_STATE,  // mp_setup_code_state, the general call path
    PROF_CALL_N_KW,         // mp_call_function_n_kw, the general call path (nested)
    PROF_INSTANCE_NEW,      // mp_obj_instance_make_new, creating an instance (nested)
    PROF_INT_BINARY_OP,     // mp_obj_int_binary_op, big int arithmetic
    PROF_FLOAT_BINARY_OP,   // mp_obj_float_binary_op
    PROF_STR_FORMAT,        // str.format and f-strings
    PROF_EXC_TRACEBACK,     // mp_obj_exception_add_traceback, once per raise caught by the VM
    PROF_QSTR_FIND,         // qstr_find_strn, interning
    PROF_GETITER,           // mp_getiter
    PROF_ITERNEXT,          // mp_iternext, the general path of FOR_ITER
    // Phases of one collection, as gc_collect() in main.c runs them.
    PROF_GC_ROOTS,          // gc_collect_start: mp_state roots and the pystack
    PROF_GC_CSTACK,         // gc_helper_collect_regs_and_stack
    PROF_GC_PORT,           // port_gc_collect and the module hooks
    PROF_GC_SWEEP,          // gc_collect_end: overflow rescans, finalisers, sweep
    PROF_GC_RESCAN,         // one round of gc_deal_with_stack_overflow
    PROF_CLASS_CACHE_HIT,   // mp_obj_class_lookup answered from its cache (calls only)
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

// Per-opcode accounting in the interpreter loop: at every dispatch the cycles
// since the previous dispatch are charged to the opcode that just ran. Calls
// into another bytecode function keep charging naturally, because the callee's
// dispatches stamp the same state; the caller's CALL gets the frame setup and
// the callee's RETURN gets the unwinding.
extern uint32_t prof_op_count[256];
extern uint64_t prof_op_cycles[256];
extern uint32_t prof_op_last;
extern uint8_t prof_op_cur;

#define PROF_OP_STAMP(next) do { \
        uint32_t _now = prof_now(); \
        prof_op_cycles[prof_op_cur] += (uint32_t)(_now - prof_op_last); \
        prof_op_count[prof_op_cur]++; \
        prof_op_cur = (next); \
        prof_op_last = _now; \
} while (0)

#else

#define PROF_BEGIN(id) ((void)0)
#define PROF_END(id) ((void)0)
#define PROF_OP_STAMP(next) ((void)0)

#endif
