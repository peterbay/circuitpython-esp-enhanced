// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2020 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// These macros are used to place code and data into different linking sections.

#pragma once

// CIRCUITPY-CHANGE: the espressif branch below is chosen by a port config
// macro. Pull the config in here rather than rely on every includer having
// done so first: py/bc.c had this header before py/bc.h, and its tagged
// function silently stayed in flash.
#include "py/mpconfig.h"

#if !defined(__ZEPHYR__) && (defined(IMXRT1XXX) || defined(FOMU) || defined(RASPBERRYPI))
#define PLACE_IN_DTCM_DATA(name) name __attribute__((section(".dtcm_data." #name)))
#define PLACE_IN_DTCM_BSS(name) name __attribute__((section(".dtcm_bss." #name)))
// Don't inline ITCM functions because that may pull them out of ITCM into other sections.
#define PLACE_IN_ITCM(name) __attribute__((section(".itcm." #name), noinline, aligned(4))) name
#elif !defined(__ZEPHYR__) && defined(STM32H7)
#define PLACE_IN_DTCM_DATA(name) name __attribute__((section(".dtcm_data." #name)))
#define PLACE_IN_DTCM_BSS(name) name __attribute__((section(".dtcm_bss." #name)))
// using ITCM on the H7 generates hard fault exception
#define PLACE_IN_ITCM(name) name
#elif !defined(__ZEPHYR__) && defined(ESP_PLATFORM) && defined(CIRCUITPY_HOT_CODE_IN_IRAM) && CIRCUITPY_HOT_CODE_IN_IRAM
// CIRCUITPY-CHANGE: on the ESP32 family code runs from flash through a cache
// it shares with PSRAM, and what a hot function costs depends on which other
// hot lines its address happens to alias with: the same struct.unpack measured
// 1450 cycles in one build and 2750 in the next with its instructions
// unchanged, and whole workloads moved by a quarter. IRAM is uncached SRAM.
// The interpreter loop and the functions it calls on every opcode go there,
// about 19 KB, so that they neither suffer from the aliasing nor cause it.
// CIRCUITPY_HOT_CODE_IN_IRAM is 1 for the interpreter core (the first two
// tiers) and 2 to add the runtime the libraries lean on (the third).
// The same cache serves the constant data in flash, and the tables the
// interpreter reads on every step -- the opcode dispatch table, the type
// structs, the qstr hashes and lengths -- alias just like code does, and
// a text pad does not move them (the rodata segment starts on a 64 KB
// boundary). PLACE_IN_DTCM_DATA puts such a table in internal RAM, where
// .dram1 is copied at boot; .bss is in internal RAM already.
#define PLACE_IN_DTCM_DATA(name) name __attribute__((section(".dram1." #name)))
#define PLACE_IN_DTCM_BSS(name) name
#define PLACE_IN_ITCM(name) __attribute__((section(".iram1." #name), noinline)) name
#define PLACE_IN_HOT_CODE(name) PLACE_IN_ITCM(name)
#if CIRCUITPY_HOT_CODE_IN_IRAM >= 2
#define PLACE_IN_WARM_CODE(name) PLACE_IN_ITCM(name)
#endif
#else
#define PLACE_IN_DTCM_DATA(name) name
#define PLACE_IN_DTCM_BSS(name) name
#define PLACE_IN_ITCM(name) name
#endif

// CIRCUITPY-CHANGE: the second tier of the interpreter's hot code -- what the
// opcodes call for attributes, methods, classes, allocation, calls, iteration
// and operators. Ports with a small tightly coupled memory keep PLACE_IN_ITCM
// for the first tier only; this one is placed where a port asks for it.
#ifndef PLACE_IN_HOT_CODE
#define PLACE_IN_HOT_CODE(name) name
#endif

// CIRCUITPY-CHANGE: the third tier -- what library code reaches through the
// second: strings and their formatting, number parsing and printing, big
// ints, struct, lists, dicts, sets, ranges, bytearrays, allocation, the
// collector's sweep, argument parsing, generators and the exception path.
// Sampling seven workloads built from Adafruit bundle libraries found these
// taking most of what still ran from flash after the first two tiers.
#ifndef PLACE_IN_WARM_CODE
#define PLACE_IN_WARM_CODE(name) name
#endif
