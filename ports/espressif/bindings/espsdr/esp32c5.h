// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Petr Vavrin
//
// SPDX-License-Identifier: MIT

// The ESP32-C5 side of espsdr, included by Receiver.c only.
//
// The register addresses and the filter measurements are ESP-SDR's
// (https://github.com/ESPARGOS/esp-sdr, docs/how-it-works.md and its C5
// target); the capture sequence follows what librftest's adctrig() writes, as
// disassembled from the archive the IDF ships. adctrig() itself cannot be
// used: it gives the engine the upper half of SRAM bank 1 (0x40830000), and on
// the C5 CircuitPython's .data and .bss reach well into that bank.
//
// The engine owns a whole 128 KiB bank while it runs, not just the half it
// writes: the CPU reads garbage anywhere in the bank (measured on a XIAO
// ESP32-C5), and so, presumably, would any DMA. No bank can be reserved at boot
// as on the S3 -- once Wi-Fi is up the IDF heap has well under 128 KiB left in
// all -- and the heap's banks cannot be borrowed safely either: Wi-Fi's DMA
// writes into heap buffers at any time.
//
// Bank 0 can. It holds the start of IRAM, code that no DMA ever writes, and at
// most a little .data at its end. A capture copies the lower half to a scratch
// buffer, lets the engine write the samples over the code, and swaps samples
// and code back, all with every interrupt off and running from flash, so that
// nothing executes from IRAM in between.

#pragma once

#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "riscv/csr.h"
#include "soc/soc.h"

// The code is overwritten from the CPU too, which a PMP split of IRAM forbids.
#if defined(CONFIG_ESP_SYSTEM_PMP_IDRAM_SPLIT)
#error "espsdr on the ESP32-C5 writes to IRAM and needs CONFIG_ESP_SYSTEM_PMP_IDRAM_SPLIT off"
#endif

// bit 31 enable, 21-20 and 17 cleared (17 makes the engine wrap around
// instead of stopping), 19 trigger (pulse), 18 done, 0-16 sample count.
#define DUMP_CTRL_REG (0x600a9004)
#define DUMP_CTRL_ENABLE (1u << 31)
#define DUMP_CTRL_TRIGGER (1u << 19)
#define DUMP_CTRL_DONE (1u << 18)
#define DUMP_CTRL_CLEAR (0x803fffffu)
// Bits 17-20 select the source, 15 being the receiver's baseband; 21-23 the
// sample clock divider.
#define DUMP_MODE_REG (0x600a9008)
#define DUMP_MODE_MASK (0x00fe0000u)
// Four 6-bit fields, each choosing an internal signal (24-27 are I and Q on
// this chip), and bit 24 on.
#define DUMP_LANES_REG (0x600a9018)
#define DUMP_LANES_MASK (0x01ffffffu)
#define DUMP_LANES_IQ (24u | (25u << 6) | (26u << 12) | (27u << 18) | (1u << 24))
// Clock enables adctrig() sets before a capture.
#define DUMP_CLOCK_REG (0x600a9c04)
#define MODEM_CLOCK_REG (0x600a0800)
#define MODEM_CLOCK_DUMP (1u << 2)
// Bit 0 gates the selected source off; adctrig() clears it.
#define DUMP_GATE_REG (0x600a20b4)
// Bits 8-11, one per 128 KiB SRAM bank from 0x40800000, give that bank to the
// engine; bit 16 makes it write the upper 64 KiB half instead of the lower.
#define SRAM_OWNER_REG (0x60095004)
#define SRAM_OWNER_MASK (0x00010f00u)
// The AGC: bits 8-14 the highest gain index the PHY calibrated for the
// current channel, bit 23 forces the index in bits 24-31 instead of the AGC's.
#define GAIN_LIMIT_REG (0x600a702c)
#define GAIN_FORCE (1u << 23)

// Never more samples than half a bank holds, so the guard words fit too.
#define GUARD_WORDS (4)
#define GUARD_WORD (0x5a5aa5a5u)

// Bank 0's lower half, code in any build with more than 64 KiB of IRAM. The
// upper half may end in .data (IRAM is a little under 128 KiB with some
// switches); the engine leaves it alone, and with interrupts off the CPU does
// not reach for it either while it is out of reach.
#define DUMP_START (0x40800000u)
#define DUMP_OWNER (1u << 8)
#define DUMP_HALF (0x10000u)

extern int _iram_start;
extern int _iram_end;

static portMUX_TYPE espsdr_lock = portMUX_INITIALIZER_UNLOCKED;

extern void phy_stop_tx_tone(unsigned);
extern void phy_pbus_workmode(void);
// The PBUS in debug mode takes forced values for its fields; block 2 and 3,
// bank 2 are the I and Q DC DACs (0-511).
extern void phy_pbus_debugmode(void);
extern void phy_pbus_force_test(unsigned block, unsigned bank, unsigned value);
extern unsigned phy_pbus_rd(unsigned block, unsigned bank);
extern void phy_pbus_xpd_rx_on(unsigned);
extern void phy_pbus_xpd_tx_off(void);
extern void phy_set_rxclk_en(unsigned);
// MHz and the analog filter path: 0 the narrow one Wi-Fi uses, 1 a wide one.
extern void phy_set_chanfreq(unsigned, unsigned);
extern unsigned phy_chip_i2c_readReg(unsigned block, unsigned host, unsigned reg);
extern void phy_i2c_writeReg(unsigned block, unsigned host, unsigned reg, unsigned data);

#define GAIN_MAX (89)

// The capacitor pair of the analog filter, as on the S3 but at registers 6
// and 7 of host 1. Widths above 23 MHz need the PHY's wide path, which only a
// full channel setup switches (tune_bandwidth()); the capacitors narrow
// either path.
#define RX_FILTER_BLOCK (0x67)
#define RX_FILTER_HOST (1)
#define RX_FILTER_REG (6)
#define BANDWIDTH_MIN (11)
#define BANDWIDTH_MAX (48)
#define BANDWIDTH_NARROW_MAX (23)

// Code against approximate full width in MHz, from ESP-SDR's noise spectra at
// 2300 and 5500 MHz, 80 MS/s: the narrow path, then the wide one.
static const uint8_t bandwidth_curve_narrow[][2] = {
    {0, 23}, {4, 22}, {8, 21}, {12, 20}, {16, 18}, {24, 16}, {32, 15}, {40, 13}, {48, 12}, {60, 11},
};
static const uint8_t bandwidth_curve_wide[][2] = {
    {0, 48}, {4, 45}, {8, 42}, {12, 40}, {16, 37}, {24, 34}, {32, 30}, {40, 27}, {48, 25}, {56, 23}, {60, 22},
};

static bool wide_path(mp_int_t mhz) {
    return mhz == 0 || mhz > BANDWIDTH_NARROW_MAX;
}

static uint8_t filter_code_for(mp_int_t mhz) {
    if (wide_path(mhz)) {
        return filter_code_on(bandwidth_curve_wide, MP_ARRAY_SIZE(bandwidth_curve_wide), mhz);
    }
    return filter_code_on(bandwidth_curve_narrow, MP_ARRAY_SIZE(bandwidth_curve_narrow), mhz);
}

static unsigned filter_read(unsigned j) {
    return phy_chip_i2c_readReg(RX_FILTER_BLOCK, RX_FILTER_HOST, RX_FILTER_REG + j);
}

static void filter_write(unsigned j, unsigned value) {
    phy_i2c_writeReg(RX_FILTER_BLOCK, RX_FILTER_HOST, RX_FILTER_REG + j, value);
}

// Where the LO follows the frequency asked for, as ESPsoup measured it with a
// signal generator: below about 2.1 GHz it stays near 2.2 GHz instead.
static void check_frequency(int32_t khz) {
    if ((khz >= 2130000 && khz <= 2730000) || (khz >= 4790000 && khz <= 5990000)) {
        return;
    }
    mp_raise_ValueError_varg(MP_ERROR_TEXT("%q must be 2130-2730 or 4790-5990 MHz"), MP_QSTR_frequency);
}

// Two ways to tune, after ESPsoup's ESP-SDR fork (tuning.h, TUNEM 0 and 2):
//
// - the PHY on a real channel of the band for its calibration, then the PLL
//   moved to the exact MHz and a kHz offset; exact from 2130 to 2730 and 4790
//   to about 5745 MHz, some 20 dB deafer above that;
// - above 3 GHz, the Wi-Fi driver on a 5 GHz channel, then libphy's own
//   phy_change_channel() to the MHz; exact up to 5990 MHz, but whole MHz only
//   and on the narrow filter path.
//
// The second is taken above 3 GHz for whole MHz, unless the bandwidth asks for
// the wide path below 5745 MHz.
#define DRIVER_PATH_ONLY_ABOVE (5745)

extern unsigned char phy_param[];
// The crystal selector phy_chip_set_chan() passes, the PLL MHz, an LO offset in kHz.
extern void phy_set_rf_freq_offset(unsigned crystal, unsigned mhz, int offset_khz);
extern void phy_change_channel(int mhz, int a, int b, int c);
#define PHY_PARAM_CRYSTAL (49)

static const uint8_t channels_5g[] = {
    140, 36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136,
    144, 149, 153, 157, 161, 165, 169, 173, 177,
};

static esp_err_t tune(int32_t khz, int8_t bandwidth) {
    int32_t mhz = (khz + 500) / 1000;
    int32_t offset = khz - mhz * 1000;
    unsigned path = bandwidth >= 0 && wide_path(bandwidth) ? 1 : 0;
    if (mhz > 3000 && offset == 0 && (path == 0 || mhz > DRIVER_PATH_ONLY_ABOVE)) {
        // Channel 140 as ESPsoup uses, else any the country setting allows:
        // the channel only switches the driver to the 5 GHz band.
        esp_err_t err = ESP_FAIL;
        for (size_t i = 0; i < MP_ARRAY_SIZE(channels_5g) && err != ESP_OK; i++) {
            err = esp_wifi_set_channel(channels_5g[i], WIFI_SECOND_CHAN_NONE);
        }
        if (err != ESP_OK) {
            return err;
        }
        phy_change_channel(mhz, 1, 0, 0);
        return ESP_OK;
    }
    phy_set_chanfreq(mhz, path);
    if (offset != 0) {
        phy_set_rf_freq_offset(phy_param[PHY_PARAM_CRYSTAL], mhz, offset);
    }
    return ESP_OK;
}

// The engine's rates, fastest first: 80 MS/s divided by 1, 2, 4, 8, 10 or 20,
// the index being the divider field. The lower ones drop samples rather than
// filter.
static const uint32_t hw_rates[] = { 80000000, 40000000, 20000000, 10000000, 8000000, 4000000 };

static uint32_t hw_rate_bits(size_t index) {
    return index;
}

// Widest the analog filter gets, in MHz.
#define BANDWIDTH_WIDEST (52)

static mp_int_t max_gain(void) {
    mp_int_t limit = (REG_READ(GAIN_LIMIT_REG) >> 8) & 127;
    return limit <= GAIN_MAX ? limit : 0;
}

// What librftest's force_rx_gain() does for the Wi-Fi receiver here. Linking
// librftest instead would bring its mac_init(), which collides with Zigbee's.
static void force_gain(bool manual, unsigned index) {
    uint32_t agc = REG_READ(GAIN_LIMIT_REG) & 0x00ffffffu;
    REG_WRITE(GAIN_LIMIT_REG, agc | (index << 24));
    REG_WRITE(GAIN_LIMIT_REG, (agc & ~GAIN_FORCE) | (index << 24) | (manual ? GAIN_FORCE : 0));
}

static void receive_only(void) {
    phy_stop_tx_tone(1);
    phy_pbus_workmode();
    phy_pbus_xpd_tx_off();
    phy_pbus_xpd_rx_on(1);
    phy_set_rxclk_en(1);
}

// The scratch buffer the code is kept in and the samples end up in, PSRAM if
// there is any. Internal RAM would make the copies, done with interrupts off,
// several times faster, but there is seldom 64 KiB of it to spare.
static volatile uint32_t *capture_buffer(size_t n) {
    if ((uint32_t)&_iram_start > DUMP_START || (uint32_t)&_iram_end < DUMP_START + DUMP_HALF) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("IRAM does not fill half of SRAM bank 0 in this build"));
    }
    size_t bytes = (n + GUARD_WORDS) * sizeof(uint32_t);
    uint32_t *scratch = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (scratch == NULL) {
        scratch = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    }
    if (scratch == NULL) {
        m_malloc_fail(bytes);
    }
    return scratch;
}

static void capture_buffer_free(volatile uint32_t *words) {
    heap_caps_free((void *)words);
}

static bool capture_into(volatile uint32_t *words, size_t n, uint32_t rate) {
    volatile uint32_t *bank = (volatile uint32_t *)DUMP_START;
    const size_t total = n + GUARD_WORDS;
    const uint32_t timeout = esp_rom_get_cpu_ticks_per_us() * CAPTURE_TIMEOUT_US;

    // The critical section leaves interrupts above level 3 enabled; none may
    // run while the code they would run is overwritten.
    taskENTER_CRITICAL(&espsdr_lock);
    const unsigned mstatus = RV_CLEAR_CSR(mstatus, MSTATUS_MIE);
    const uint32_t ctrl_saved = REG_READ(DUMP_CTRL_REG);
    const uint32_t mode_saved = REG_READ(DUMP_MODE_REG);
    const uint32_t lanes_saved = REG_READ(DUMP_LANES_REG);
    const uint32_t clock_saved = REG_READ(DUMP_CLOCK_REG);
    const uint32_t modem_clock_saved = REG_READ(MODEM_CLOCK_REG);
    const uint32_t gate_saved = REG_READ(DUMP_GATE_REG);
    const uint32_t owner_saved = REG_READ(SRAM_OWNER_REG);

    for (size_t i = 0; i < total; i++) {
        words[i] = bank[i];
    }
    for (size_t i = 0; i < n; i++) {
        bank[i] = UNWRITTEN;
    }
    for (size_t i = 0; i < GUARD_WORDS; i++) {
        bank[n + i] = GUARD_WORD ^ i;
    }

    const uint32_t ctrl = (ctrl_saved & ~DUMP_CTRL_CLEAR) | n;
    REG_WRITE(DUMP_CTRL_REG, ctrl);
    REG_WRITE(DUMP_MODE_REG, (mode_saved & ~DUMP_MODE_MASK) | (15u << 17) | (rate << 21));
    REG_WRITE(DUMP_CLOCK_REG, 0xffffffffu);
    REG_WRITE(MODEM_CLOCK_REG, modem_clock_saved | MODEM_CLOCK_DUMP);
    REG_WRITE(DUMP_LANES_REG, (lanes_saved & ~DUMP_LANES_MASK) | DUMP_LANES_IQ);
    REG_WRITE(DUMP_GATE_REG, gate_saved & ~1u);

    REG_WRITE(SRAM_OWNER_REG, (owner_saved & ~SRAM_OWNER_MASK) | DUMP_OWNER);
    __asm__ volatile ("fence rw, rw" ::: "memory");
    (void)REG_READ(SRAM_OWNER_REG);
    REG_WRITE(DUMP_CTRL_REG, ctrl | DUMP_CTRL_ENABLE);
    REG_WRITE(DUMP_CTRL_REG, ctrl | DUMP_CTRL_ENABLE | DUMP_CTRL_TRIGGER);
    REG_WRITE(DUMP_CTRL_REG, ctrl | DUMP_CTRL_ENABLE);
    const uint32_t start = esp_cpu_get_cycle_count();
    while (!(REG_READ(DUMP_CTRL_REG) & DUMP_CTRL_DONE) &&
           esp_cpu_get_cycle_count() - start < timeout) {
    }
    const bool done = (REG_READ(DUMP_CTRL_REG) & DUMP_CTRL_DONE) != 0;
    REG_WRITE(DUMP_CTRL_REG, ctrl);
    REG_WRITE(SRAM_OWNER_REG, owner_saved);
    __asm__ volatile ("fence rw, rw" ::: "memory");
    (void)REG_READ(SRAM_OWNER_REG);

    REG_WRITE(DUMP_GATE_REG, gate_saved);
    REG_WRITE(DUMP_LANES_REG, lanes_saved);
    REG_WRITE(MODEM_CLOCK_REG, modem_clock_saved);
    REG_WRITE(DUMP_CLOCK_REG, clock_saved);
    REG_WRITE(DUMP_MODE_REG, mode_saved);
    REG_WRITE(DUMP_CTRL_REG, ctrl_saved);

    for (size_t i = 0; i < total; i++) {
        uint32_t sample = bank[i];
        bank[i] = words[i];
        words[i] = sample;
    }
    __asm__ volatile ("fence.i" ::: "memory");
    RV_SET_CSR(mstatus, mstatus & MSTATUS_MIE);
    taskEXIT_CRITICAL(&espsdr_lock);

    for (size_t i = 0; i < GUARD_WORDS; i++) {
        if (words[n + i] != (GUARD_WORD ^ i)) {
            return false;
        }
    }
    return done && words[0] != UNWRITTEN && words[n - 1] != UNWRITTEN;
}
