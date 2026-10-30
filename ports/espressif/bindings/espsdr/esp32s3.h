// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Petr Vavrin
//
// SPDX-License-Identifier: MIT

// The ESP32-S3 side of espsdr, included by Receiver.c only. The addresses and
// bit fields are the ones ESP-SDR worked out from Espressif's RF test library
// and describes in its docs/how-it-works.md; the functions are private ones in
// the ROM, libphy and librftest that the factory test code calls.

#pragma once

#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "heap_memory_layout.h"
#include "soc/soc.h"

// The dump engine writes into a fixed 64 KiB bank. Value 4 in the low nibble
// of the owner register gives it the bank; the CPU must not touch the bank
// until it is handed back.
#define DUMP_BANK_START (0x3fcd0000)
#define DUMP_BANK_END (0x3fce0000)
#define DUMP_BUFFER ((volatile uint32_t *)DUMP_BANK_START)
// bit 31 enable, 19 trigger (pulse), 18 done, 16 selects 16 MS/s, 15 halves
// 80 MS/s to 40, 0-14 sample count.
#define DUMP_CTRL_REG (0x60033d5c)
// Four 6-bit fields, each choosing an internal signal; 0-3 are I and Q.
#define DUMP_LANES_REG (0x60033d90)
#define SRAM_OWNER_REG (0x600c101c)
// Bits 8-14: highest gain index the PHY calibrated for the current channel.
#define GAIN_LIMIT_REG (0x6001c02c)

#define DUMP_CTRL_ENABLE (1u << 31)
#define DUMP_CTRL_TRIGGER (1u << 19)
#define DUMP_CTRL_DONE (1u << 18)
#define DUMP_CTRL_16MSPS (1u << 16)
#define DUMP_CTRL_40MSPS (1u << 15)

// The bank must hold neither heap nor stacks: the engine overwrites it, and
// while it owns the bank the CPU reads garbage there. CircuitPython's static
// data ends well below it (checked in the map), so the heap is all there is
// to keep out.
SOC_RESERVE_MEMORY_REGION(DUMP_BANK_START, DUMP_BANK_END, espsdr_dump_bank);

extern void stop_tx_tone(unsigned);
extern void rom_pbus_workmode(void);
extern void rom_pbus_xpd_rx_on(unsigned);
extern void rom_pbus_xpd_tx_off(void);
extern void rom_set_rxclk_en(unsigned);
// manual, gain index, unused. With manual 0 the hardware AGC takes over.
extern void force_rx_gain(unsigned, unsigned, unsigned);
extern unsigned rom_chip_i2c_readReg(unsigned block, unsigned host, unsigned reg);
extern void rom_chip_i2c_writeReg(unsigned block, unsigned host, unsigned reg, unsigned data);

// Anything above this in the gain limit field means the PHY has not filled it in.
#define GAIN_MAX (82)

// The analog low-pass filter ahead of the ADC: a 6-bit capacitor code in the
// low bits of two registers of the baseband block, one per branch. A larger
// code is a narrower filter. The PHY sets its own for Wi-Fi, about 20 MHz.
#define RX_FILTER_BLOCK (0x67)
#define RX_FILTER_HOST (0)
#define RX_FILTER_REG (4)
#define BANDWIDTH_MIN (13)
#define BANDWIDTH_MAX (69)

// Code against approximate full -3 dB width in MHz, as ESP-SDR measured it on
// an S3 from the noise spectrum at 80 MS/s.
static const uint8_t bandwidth_curve[][2] = {
    {0, 69}, {4, 51}, {8, 45}, {16, 33}, {24, 25}, {32, 21}, {48, 16}, {60, 13},
};

static uint8_t filter_code_for(mp_int_t mhz) {
    return filter_code_on(bandwidth_curve, MP_ARRAY_SIZE(bandwidth_curve), mhz);
}

static unsigned filter_read(unsigned j) {
    return rom_chip_i2c_readReg(RX_FILTER_BLOCK, RX_FILTER_HOST, RX_FILTER_REG + j);
}

static void filter_write(unsigned j, unsigned value) {
    rom_chip_i2c_writeReg(RX_FILTER_BLOCK, RX_FILTER_HOST, RX_FILTER_REG + j, value);
}

// Wi-Fi channels of the 2.4 GHz band only.
static mp_int_t channel_for(mp_int_t mhz) {
    if (mhz == 2484) {
        return 14;
    }
    if (mhz >= 2412 && mhz <= 2472 && (mhz - 2412) % 5 == 0) {
        return (mhz - 2412) / 5 + 1;
    }
    mp_raise_ValueError_varg(MP_ERROR_TEXT("%q must be a Wi-Fi channel frequency (2412-2472 in steps of 5, or 2484)"), MP_QSTR_frequency);
}

static uint32_t rate_bits(uint32_t sample_rate) {
    switch (sample_rate) {
        case 80000000:
            return 0;
        case 40000000:
            return DUMP_CTRL_40MSPS;
        case 16000000:
            return DUMP_CTRL_16MSPS;
    }
    mp_raise_ValueError_varg(MP_ERROR_TEXT("%q must be 80000000, 40000000 or 16000000"), MP_QSTR_sample_rate);
}

static mp_int_t max_gain(void) {
    mp_int_t limit = (REG_READ(GAIN_LIMIT_REG) >> 8) & 127;
    return limit <= GAIN_MAX ? limit : 0;
}

// The analog filter mode is part of the channel setup on some chips; not here.
static void tune_bandwidth(mp_int_t frequency, int8_t bandwidth) {
    (void)frequency;
    (void)bandwidth;
}

static void force_gain(bool manual, unsigned index) {
    force_rx_gain(manual, index, 0);
}

static void receive_only(void) {
    stop_tx_tone(1);
    rom_pbus_workmode();
    rom_pbus_xpd_tx_off();
    rom_pbus_xpd_rx_on(1);
    rom_set_rxclk_en(1);
}

// The samples end up in the reserved bank itself.
static volatile uint32_t *capture_buffer(size_t n) {
    (void)n;
    return DUMP_BUFFER;
}

static void capture_buffer_free(volatile uint32_t *words) {
    (void)words;
}

static bool capture_into(volatile uint32_t *words, size_t n, uint32_t rate) {
    for (size_t i = 0; i < n; i++) {
        words[i] = UNWRITTEN;
    }
    uint32_t ctrl = DUMP_CTRL_ENABLE | rate | n;
    uint32_t owner = REG_READ(SRAM_OWNER_REG);
    REG_WRITE(DUMP_CTRL_REG, 0);
    REG_WRITE(DUMP_LANES_REG, (0 << 0) | (1 << 6) | (2 << 12) | (3 << 18));
    REG_WRITE(SRAM_OWNER_REG, (owner & ~15u) | 4u);
    int64_t start = esp_timer_get_time();
    REG_WRITE(DUMP_CTRL_REG, ctrl);
    REG_WRITE(DUMP_CTRL_REG, ctrl | DUMP_CTRL_TRIGGER);
    REG_WRITE(DUMP_CTRL_REG, ctrl);
    while (!(REG_READ(DUMP_CTRL_REG) & DUMP_CTRL_DONE) &&
           esp_timer_get_time() - start < CAPTURE_TIMEOUT_US) {
    }
    bool done = (REG_READ(DUMP_CTRL_REG) & DUMP_CTRL_DONE) != 0;
    REG_WRITE(DUMP_CTRL_REG, 0);
    REG_WRITE(SRAM_OWNER_REG, owner);
    return done && words[0] != UNWRITTEN && words[n - 1] != UNWRITTEN;
}
