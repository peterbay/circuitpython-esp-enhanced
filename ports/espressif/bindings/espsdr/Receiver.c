// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Petr Vavrin
//
// SPDX-License-Identifier: MIT

#include <string.h>

#include "py/binary.h"
#include "py/mphal.h"
#include "py/mperrno.h"
#include "py/objproperty.h"
#include "py/runtime.h"

#include "shared-bindings/util.h"
#include "shared-bindings/wifi/__init__.h"
#include "shared-bindings/wifi/Radio.h"
#include "shared/runtime/context_manager_helpers.h"

#include "bindings/espsdr/Receiver.h"

#include "esp_wifi.h"
#include "sdkconfig.h"

// Everything here is undocumented. The dump engine writes one 32-bit word per
// sample into on-chip SRAM: two 10-bit two's complement fields in bits 0-9 and
// 10-19, and in bits 20-27 the receiver gain index in force for that sample.
// Which field is I: see capture(). Where it writes, how it is started and
// which private PHY functions tune the radio differs per chip; the chip
// headers below hold that.

// Written over the capture memory first: a word still holding it was never
// written by the engine, so a short capture is caught instead of returning
// the previous one.
#define UNWRITTEN (0xa5a0055au)

// A full capture at the slowest rate (16380 samples at 4 MS/s) takes about 4 ms.
#define CAPTURE_TIMEOUT_US (20000)

// The analog low-pass filter ahead of the ADC is a 6-bit capacitor code in
// the low bits of two registers of the baseband block, one per branch. A larger
// code is a narrower filter.
#define RX_FILTER_MASK (63u)

// Linear between measured {code, MHz} points, widest first. 0 MHz asks for the
// widest setting.
static uint8_t filter_code_on(const uint8_t curve[][2], size_t count, mp_int_t mhz) {
    if (mhz == 0 || mhz >= curve[0][1]) {
        return curve[0][0];
    }
    for (size_t i = 1; i < count; i++) {
        if (mhz >= curve[i][1]) {
            mp_int_t wide = curve[i - 1][1], narrow = curve[i][1];
            mp_int_t c0 = curve[i - 1][0], c1 = curve[i][0];
            return c0 + ((c1 - c0) * (wide - mhz) + (wide - narrow) / 2) / (wide - narrow);
        }
    }
    return curve[count - 1][0];
}

#include "bindings/espsdr/dsp.h"

#if defined(CONFIG_IDF_TARGET_ESP32S3)
#include "bindings/espsdr/esp32s3.h"
#elif defined(CONFIG_IDF_TARGET_ESP32C5)
#include "bindings/espsdr/esp32c5.h"
#else
#error "espsdr has no support for this chip"
#endif

typedef struct {
    mp_obj_base_t base;
    uint32_t sample_rate;
    // kHz.
    int32_t frequency;
    // -1 for the hardware AGC.
    int8_t gain;
    // -1 leaves the PHY's own filter; else the MHz asked for, 0 for widest.
    int8_t bandwidth;
    // sample_rate is hw_rates[source] / decimation.
    uint8_t source;
    uint8_t decimation;
    bool filtered;
    bool deinited;
    bool promiscuous_was_on;
} espsdr_receiver_obj_t;

// Full width in MHz of what reaches the ADC: about 23 MHz with the PHY's own
// filter, and up to 15 % over the setting as measured.
static uint32_t analog_width_mhz(int8_t bandwidth) {
    if (bandwidth < 0) {
        return 24;
    }
    if (bandwidth == 0) {
        return BANDWIDTH_WIDEST;
    }
    return bandwidth + (bandwidth * 15 + 99) / 100;
}

// How a rate is had: straight from the engine, or decimated on the chip from
// the slowest engine rate that still spans what the analog filter lets
// through. The engine's lower rates drop samples without filtering, so
// anything wider than them folds back in. A rate too low to reach from such a
// source in 128 steps comes from the fastest one that does reach it; a
// narrower bandwidth keeps that clean.
static void plan_rate(espsdr_receiver_obj_t *self, uint32_t rate, int8_t bandwidth, bool filtered) {
    for (size_t i = 0; i < MP_ARRAY_SIZE(hw_rates); i++) {
        if (hw_rates[i] == rate && (!filtered || i == 0)) {
            self->source = i;
            self->decimation = 1;
            return;
        }
    }
    uint32_t width = analog_width_mhz(bandwidth) * 1000000;
    int fallback = -1;
    for (size_t i = MP_ARRAY_SIZE(hw_rates); i-- > 0;) {
        uint32_t src = hw_rates[i];
        if (rate == 0 || src % rate != 0 || src / rate < 2 || src / rate > DECIMATION_MAX) {
            continue;
        }
        if (src >= width || i == 0) {
            fallback = i;
            break;
        }
        fallback = i;
    }
    if (fallback >= 0) {
        self->source = fallback;
        self->decimation = hw_rates[fallback] / rate;
        return;
    }
    mp_raise_ValueError_varg(MP_ERROR_TEXT("%q must be one of SAMPLE_RATES, or one of them divided by 2 to %d"),
        MP_QSTR_sample_rate, DECIMATION_MAX);
}

static size_t max_samples(espsdr_receiver_obj_t *self) {
    if (self->decimation == 1) {
        return ESPSDR_MAX_SAMPLES;
    }
    return (ESPSDR_MAX_SAMPLES - fir_taps_for(self->decimation)) / self->decimation;
}

// One radio, one receiver.
static espsdr_receiver_obj_t *_receiver = NULL;

static void check_for_deinit(espsdr_receiver_obj_t *self) {
    if (self->deinited) {
        raise_deinited_error();
    }
}

// MHz, an int or a float, to kHz, checked against what the chip can tune.
static int32_t frequency_khz(mp_obj_t value) {
    int32_t khz;
    if (mp_obj_is_int(value)) {
        khz = mp_arg_validate_int_range(mp_obj_get_int(value), 0, 10000, MP_QSTR_frequency) * 1000;
    } else {
        mp_float_t mhz = mp_arg_validate_obj_float_range(value, 0, 10000, MP_QSTR_frequency);
        khz = (int32_t)(mhz * 1000 + MICROPY_FLOAT_CONST(0.5));
    }
    check_frequency(khz);
    return khz;
}

// Tune and switch the front end to receive only. Done again after every change
// of frequency, gain or bandwidth, because tuning rebuilds the PHY state.
static esp_err_t configure(espsdr_receiver_obj_t *self) {
    esp_err_t err = tune(self->frequency, self->bandwidth);
    if (err != ESP_OK) {
        return err;
    }
    receive_only();
    if (self->gain >= 0) {
        mp_int_t limit = max_gain();
        if (self->gain > limit) {
            self->gain = limit;
        }
        force_gain(true, self->gain);
    } else {
        force_gain(false, 0);
    }
    // ESP-SDR waits this long for the receiver to settle after the switch.
    esp_rom_delay_us(3000);
    return ESP_OK;
}

static void configure_or_raise(espsdr_receiver_obj_t *self) {
    esp_err_t err = configure(self);
    if (err != ESP_OK) {
        mp_raise_RuntimeError_varg(MP_ERROR_TEXT("Could not tune: %d"), (int)err);
    }
}

static void release(espsdr_receiver_obj_t *self) {
    force_gain(false, 0);
    if (!self->promiscuous_was_on) {
        esp_wifi_set_promiscuous(false);
    }
    // The pbus calls leave the transmitter off and the PHY in a test mode. A
    // stop and start of the driver puts it back the way Wi-Fi expects.
    wifi_radio_obj_t *radio = &common_hal_wifi_radio_obj;
    if (common_hal_wifi_radio_get_enabled(radio)) {
        common_hal_wifi_radio_set_enabled(radio, false);
        common_hal_wifi_radio_set_enabled(radio, true);
    }
    self->deinited = true;
}

void espsdr_reset(void) {
    if (_receiver != NULL) {
        release(_receiver);
        _receiver = NULL;
    }
}

// One snapshot of n samples at a rate code, as raw words; the caller hands
// them back with capture_buffer_free().
static volatile uint32_t *acquire(espsdr_receiver_obj_t *self, size_t n, uint32_t rate) {
    volatile uint32_t *words = capture_buffer(n);

    // Only for the capture, and put back after it: the PHY rewrites these when
    // it retunes, and Wi-Fi expects its own filter whenever it gets the radio.
    unsigned filter_saved[2];
    if (self->bandwidth >= 0) {
        unsigned code = filter_code_for(self->bandwidth);
        for (unsigned j = 0; j < 2; j++) {
            filter_saved[j] = filter_read(j);
            filter_write(j, (filter_saved[j] & ~RX_FILTER_MASK) | code);
        }
    }

    bool complete = capture_into(words, n, rate);

    if (self->bandwidth >= 0) {
        for (unsigned j = 0; j < 2; j++) {
            filter_write(j, filter_saved[j]);
        }
    }

    if (!complete) {
        capture_buffer_free(words);
        mp_raise_OSError(MP_ETIMEDOUT);
    }
    return words;
}

#if defined(CONFIG_IDF_TARGET_ESP32C5)
// Hardware DC correction through the baseband's DC DACs, after ESPsoup's
// dc_cal.h (GPL-3.0-or-later), itself a port of ESP-SDR's S31 dc.c: measure
// the DC, the 2x2 response of the two DACs to +-16 codes, then Newton steps
// with step halving, and keep the best codes measured. The PBUS has to be in
// debug mode for that, so the correction lasts until the next retune or gain
// change puts it back in work mode, and it needs a manual gain: in debug mode
// the PBUS no longer follows the AGC.

// Mean I and Q of a snapshot at 20 MS/s, in 8-bit counts (10-bit / 4) as
// ESP-SDR's thresholds are.
static void dc_measure(espsdr_receiver_obj_t *self, float m[2]) {
    const size_t n = 4096;
    volatile uint32_t *words = acquire(self, n, hw_rate_bits(2));
    int64_t si = 0, sq = 0;
    for (size_t j = 0; j < n; j++) {
        si += word_i(words[j]);
        sq += word_q(words[j]);
    }
    capture_buffer_free(words);
    m[0] = si / (n * 4.0f);
    m[1] = sq / (n * 4.0f);
}

static void dc_apply(int i, int q) {
    phy_pbus_force_test(2, 2, i);
    phy_pbus_force_test(3, 2, q);
    mp_hal_delay_ms(10);
}

static float dc_cost(const float m[2]) {
    return m[0] * m[0] + m[1] * m[1];
}

static int dc_clamp(int v, int base) {
    v = v < base - 96 ? base - 96 : v > base + 96 ? base + 96 : v;
    return v < 0 ? 0 : v > 511 ? 511 : v;
}

// Returns the best codes found; before and after get the DC measured.
static void dc_calibrate(espsdr_receiver_obj_t *self, float before[2], float after[2], const int base[2], int best[2]) {
    best[0] = base[0];
    best[1] = base[1];
    float best_cost = dc_cost(before);
    after[0] = before[0];
    after[1] = before[1];
    int di = base[0] > 495 ? -16 : 16, dq = base[1] > 495 ? -16 : 16;
    float mi[2], mq[2], obs[2];
    dc_apply(base[0] + di, base[1]);
    dc_measure(self, mi);
    dc_apply(base[0], base[1] + dq);
    dc_measure(self, mq);
    float a = (mi[0] - before[0]) / di, b = (mq[0] - before[0]) / dq;
    float c = (mi[1] - before[1]) / di, d = (mq[1] - before[1]) / dq;
    float det = a * d - b * c;
    if (fabsf(det) < .002f || fabsf(a) > 4 || fabsf(b) > 4 || fabsf(c) > 4 || fabsf(d) > 4) {
        return;
    }
    for (unsigned step = 0; step < 5 && best_cost > .25f; step++) {
        float delta[2] = { (-d * after[0] + b * after[1]) / det, (c * after[0] - a * after[1]) / det };
        bool improved = false;
        for (unsigned trial = 0; trial < 3; trial++) {
            int next[2];
            for (unsigned axis = 0; axis < 2; axis++) {
                float change = fmaxf(-32.f, fminf(32.f, delta[axis])) / (float)(1u << trial);
                next[axis] = dc_clamp(best[axis] + (int)roundf(change), base[axis]);
            }
            if (next[0] == best[0] && next[1] == best[1]) {
                break;
            }
            dc_apply(next[0], next[1]);
            dc_measure(self, obs);
            if (dc_cost(obs) < best_cost) {
                best_cost = dc_cost(obs);
                best[0] = next[0];
                best[1] = next[1];
                after[0] = obs[0];
                after[1] = obs[1];
                improved = true;
                break;
            }
        }
        if (!improved) {
            break;
        }
    }
}
#endif

//| class Receiver:
//|     """Snapshot receiver in the 2.4 GHz band, and on the ESP32-C5 the 5 GHz band."""
//|
//|     def __init__(
//|         self,
//|         *,
//|         frequency: float = 2412,
//|         sample_rate: int = 80_000_000,
//|         gain: Optional[int] = None,
//|         bandwidth: Optional[int] = None,
//|         filtered: bool = False,
//|     ) -> None:
//|         """Take over the radio. Wi-Fi must be enabled and idle.
//|
//|         :param float frequency: centre frequency in MHz. ESP32-S3: a Wi-Fi
//|           channel frequency, 2412 to 2472 in steps of 5, or 2484. ESP32-C5:
//|           any frequency from 2130 to 2730 or 4790 to 5990 MHz, in steps of
//|           1 kHz (a float for fractions of a MHz).
//|         :param int sample_rate: samples per second: one of `espsdr.SAMPLE_RATES`,
//|           which come straight from the hardware, or one of those divided by
//|           2 to 128, which is decimated on the chip through a low-pass filter.
//|           The hardware's own lower rates drop samples rather than filter,
//|           so signals wider than the rate fold back in; decimated rates do
//|           not (see `filtered`).
//|         :param int gain: a PHY gain index from 0 to `max_gain`, or None for
//|           the hardware AGC. Not dB.
//|         :param int bandwidth: analog filter width in MHz, or 0 for the
//|           widest: 13 to 69 on the ESP32-S3, 11 to 48 on the ESP32-C5. None
//|           keeps the PHY's own (about 20 MHz).
//|         :param bool filtered: decimate on the chip for every rate below the
//|           fastest, the hardware's own lower rates included."""
//|         ...
//|
static int8_t validate_bandwidth(mp_obj_t value) {
    if (value == mp_const_none) {
        return -1;
    }
    mp_int_t mhz = mp_obj_get_int(value);
    if (mhz != 0) {
        mp_arg_validate_int_range(mhz, BANDWIDTH_MIN, BANDWIDTH_MAX, MP_QSTR_bandwidth);
    }
    return mhz;
}

static mp_obj_t espsdr_receiver_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_frequency, ARG_sample_rate, ARG_gain, ARG_bandwidth, ARG_filtered };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_frequency, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = MP_OBJ_NEW_SMALL_INT(2412)} },
        { MP_QSTR_sample_rate, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 80000000} },
        { MP_QSTR_gain, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = mp_const_none} },
        { MP_QSTR_bandwidth, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = mp_const_none} },
        { MP_QSTR_filtered, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    int32_t frequency = frequency_khz(args[ARG_frequency].u_obj);
    uint32_t sample_rate = args[ARG_sample_rate].u_int;
    mp_int_t gain = -1;
    if (args[ARG_gain].u_obj != mp_const_none) {
        gain = mp_arg_validate_int_range(mp_obj_get_int(args[ARG_gain].u_obj), 0, GAIN_MAX, MP_QSTR_gain);
    }
    int8_t bandwidth = validate_bandwidth(args[ARG_bandwidth].u_obj);
    bool filtered = args[ARG_filtered].u_bool;
    espsdr_receiver_obj_t plan;
    plan_rate(&plan, sample_rate, bandwidth, filtered);

    if (_receiver != NULL) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("Receiver in use"));
    }
    // What "import wifi" does: start the driver if nothing has yet. A radio the
    // script switched off stays off.
    common_hal_wifi_init(true);
    wifi_radio_obj_t *radio = &common_hal_wifi_radio_obj;
    if (!common_hal_wifi_radio_get_enabled(radio)) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("Wi-Fi is not enabled"));
    }
    if (common_hal_wifi_radio_get_connected(radio) || common_hal_wifi_radio_get_ap_active(radio)) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("Wi-Fi in use"));
    }

    espsdr_receiver_obj_t *self = mp_obj_malloc_with_finaliser(espsdr_receiver_obj_t, &espsdr_receiver_type);
    self->frequency = frequency;
    self->sample_rate = sample_rate;
    self->source = plan.source;
    self->decimation = plan.decimation;
    self->filtered = filtered;
    self->gain = gain;
    self->bandwidth = bandwidth;
    self->deinited = false;

    // Promiscuous mode keeps the receiver running with no association.
    bool promiscuous = false;
    esp_wifi_get_promiscuous(&promiscuous);
    self->promiscuous_was_on = promiscuous;
    if (!promiscuous) {
        esp_wifi_set_promiscuous(true);
    }
    esp_err_t err = configure(self);
    if (err != ESP_OK) {
        release(self);
        mp_raise_RuntimeError_varg(MP_ERROR_TEXT("Could not tune: %d"), (int)err);
    }
    _receiver = self;
    return MP_OBJ_FROM_PTR(self);
}

//|     def deinit(self) -> None:
//|         """Give the radio back to Wi-Fi."""
//|         ...
//|
static mp_obj_t espsdr_receiver_deinit(mp_obj_t self_in) {
    espsdr_receiver_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->deinited && _receiver == self) {
        espsdr_reset();
    }
    self->deinited = true;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espsdr_receiver_deinit_obj, espsdr_receiver_deinit);

//|     def __enter__(self) -> Receiver:
//|         """No-op used by Context Managers."""
//|         ...
//|
//  Provided by context manager helper.

//|     def __exit__(self) -> None:
//|         """Automatically deinitializes when exiting a context."""
//|         ...
//|
static mp_obj_t espsdr_receiver_obj___exit__(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    return espsdr_receiver_deinit(args[0]);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(espsdr_receiver___exit___obj, 4, 4, espsdr_receiver_obj___exit__);

//|     def capture(self, buffer: WriteableBuffer) -> int:
//|         """Take one snapshot and fill ``buffer`` with it. The buffer's type
//|         sets the format and its length the number of samples, at most
//|         `max_samples`:
//|
//|         * ``bytearray`` or ``array('b')``: I, Q interleaved, the top 8 of
//|           the 10 bits, signed. Two bytes per sample.
//|         * ``array('h')``: I, Q interleaved, all 10 bits, signed.
//|         * ``array('f')``: I, Q interleaved, in the same units as ``'h'``;
//|           at a decimated rate with the fractions the filter adds.
//|         * ``array('L')`` or ``array('I')``: the raw words as the hardware
//|           writes them: Q in bits 0-9 and I in bits 10-19 (two's complement),
//|           gain index in bits 20-27. Positive frequencies are above the
//|           centre with that assignment; the other way round mirrors them.
//|           Not at a decimated rate.
//|
//|         At a decimated rate the mean of the capture is taken off.
//|
//|         The capture takes 205 us at 80 MS/s for the full length. On the
//|         ESP32-C5 interrupts are off for that and for copying the SRAM the
//|         engine borrows to and from a scratch buffer in PSRAM: 32 us for
//|         256 samples, about 1 ms for 4096, 9.6 ms for the full length
//|         (13.5 ms at 4 MS/s).
//|
//|         :return: the gain index the receiver used, to scale AGC snapshots.
//|         """
//|         ...
//|
static mp_obj_t espsdr_receiver_capture(mp_obj_t self_in, mp_obj_t buffer_in) {
    espsdr_receiver_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);

    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(buffer_in, &bufinfo, MP_BUFFER_WRITE);
    char typecode = bufinfo.typecode;
    size_t n;
    if (typecode == BYTEARRAY_TYPECODE || typecode == 'b' || typecode == 'B') {
        n = bufinfo.len / 2;
    } else if (typecode == 'h') {
        n = bufinfo.len / 4;
    } else if (typecode == 'f') {
        n = bufinfo.len / 8;
    } else if ((typecode == 'L' || typecode == 'I') && self->decimation == 1) {
        n = bufinfo.len / 4;
    } else {
        mp_raise_ValueError_varg(MP_ERROR_TEXT("%q must be a bytearray or array of type 'b', 'h', 'f', or at the hardware's rates 'I' or 'L'"), MP_QSTR_buffer);
    }
    n = mp_arg_validate_int_range(n, 1, max_samples(self), MP_QSTR_buffer);

    if (self->decimation > 1) {
        unsigned r = self->decimation;
        size_t need = n * r + fir_taps_for(r);
        volatile uint32_t *words = acquire(self, need, hw_rate_bits(self->source));
        mp_int_t gain = (words[need - 1] >> 20) & 0xff;
        // In units of 1/64 of the 10-bit LSB.
        const int16_t *iq = decimate(words, n, r);
        for (size_t i = 0; i < 2 * n; i++) {
            if (typecode == 'f') {
                ((float *)bufinfo.buf)[i] = iq[i] / 64.0f;
            } else if (typecode == 'h') {
                ((int16_t *)bufinfo.buf)[i] = (iq[i] + 32) >> 6;
            } else {
                ((int8_t *)bufinfo.buf)[i] = iq[i] >> 8;
            }
        }
        capture_buffer_free(words);
        return MP_OBJ_NEW_SMALL_INT(gain);
    }

    volatile uint32_t *words = acquire(self, n, hw_rate_bits(self->source));

    // The low field goes out as Q and the high one as I. Taken the other way
    // round, as ESP-SDR names them, the spectrum comes out mirrored: an
    // 802.15.4 transmitter at 2435 MHz showed up at +2 MHz with the receiver
    // at 2437 and at -3 MHz with it at 2432.
    //
    // Shifting a 10-bit field to the top of a 32-bit word and back sign-extends it.
    if (typecode == 'h') {
        int16_t *out = bufinfo.buf;
        for (size_t i = 0; i < n; i++) {
            uint32_t w = words[i];
            out[2 * i] = (int16_t)((int32_t)(w << 12) >> 22);
            out[2 * i + 1] = (int16_t)((int32_t)(w << 22) >> 22);
        }
    } else if (typecode == 'f') {
        float *out = bufinfo.buf;
        for (size_t i = 0; i < n; i++) {
            out[2 * i] = word_i(words[i]);
            out[2 * i + 1] = word_q(words[i]);
        }
    } else if (typecode == 'L' || typecode == 'I') {
        uint32_t *out = bufinfo.buf;
        for (size_t i = 0; i < n; i++) {
            out[i] = words[i];
        }
    } else {
        uint8_t *out = bufinfo.buf;
        for (size_t i = 0; i < n; i++) {
            uint32_t w = words[i];
            out[2 * i] = (w >> 12) & 0xff;
            out[2 * i + 1] = (w >> 2) & 0xff;
        }
    }
    mp_int_t gain = (words[n - 1] >> 20) & 0xff;
    capture_buffer_free(words);
    return MP_OBJ_NEW_SMALL_INT(gain);
}
static MP_DEFINE_CONST_FUN_OBJ_2(espsdr_receiver_capture_obj, espsdr_receiver_capture);

#if defined(CONFIG_IDF_TARGET_ESP32C5)
//|     def calibrate_dc(self) -> Tuple[float, float, float, float]:
//|         """ESP32-C5 only. Trim the receiver's own DC offset in hardware,
//|         through its DC DACs, so the spike at the centre of a spectrum shrinks.
//|
//|         Needs a manual `gain`: the correction freezes the front end's analog
//|         settings, which the AGC could no longer change. It lasts until the
//|         next change of `frequency`, `gain` or `bandwidth`; calibrate again
//|         after one. Takes some 20 snapshots and 50 to 150 ms.
//|
//|         :return: the mean I and Q before and after, in units of the 10-bit
//|           sample (as ``capture()`` into ``array('h')``).
//|         """
//|         ...
//|
static mp_obj_t espsdr_receiver_calibrate_dc(mp_obj_t self_in) {
    espsdr_receiver_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    if (self->gain < 0) {
        mp_raise_ValueError_varg(MP_ERROR_TEXT("%q must be set, not None"), MP_QSTR_gain);
    }
    // From the table values in work mode, not an earlier correction.
    configure_or_raise(self);
    float before[2], after[2];
    dc_measure(self, before);
    after[0] = before[0];
    after[1] = before[1];
    if (dc_cost(before) >= .25f) {
        unsigned live[4][2];
        for (unsigned block = 0; block < 4; block++) {
            for (unsigned bank = 1; bank <= 2; bank++) {
                live[block][bank - 1] = phy_pbus_rd(block, bank);
            }
        }
        int base[2] = { (int)live[2][1], (int)live[3][1] };
        int best[2];
        // Debug mode, with the live state forced so that nothing else moves.
        phy_pbus_debugmode();
        for (unsigned block = 0; block < 4; block++) {
            for (unsigned bank = 1; bank <= 2; bank++) {
                phy_pbus_force_test(block, bank, live[block][bank - 1]);
            }
        }
        nlr_buf_t nlr;
        if (nlr_push(&nlr) == 0) {
            dc_calibrate(self, before, after, base, best);
            nlr_pop();
        } else {
            dc_apply(base[0], base[1]);
            nlr_jump(nlr.ret_val);
        }
        // The last trial may have been worse: the best codes measured.
        dc_apply(best[0], best[1]);
    }
    mp_obj_t result[4] = {
        mp_obj_new_float(before[0] * 4), mp_obj_new_float(before[1] * 4),
        mp_obj_new_float(after[0] * 4), mp_obj_new_float(after[1] * 4),
    };
    return mp_obj_new_tuple(4, result);
}
static MP_DEFINE_CONST_FUN_OBJ_1(espsdr_receiver_calibrate_dc_obj, espsdr_receiver_calibrate_dc);
#endif

//|     def spectrum(self, out: WriteableBuffer, *, average: int = 1) -> int:
//|         """Take one snapshot and fill ``out`` with its averaged power
//|         spectrum, computed on the chip: far faster than an FFT in Python,
//|         most of all on the ESP32-C5.
//|
//|         ``len(out)`` is the number of bins, a power of two from 64 to 1024;
//|         ``average`` consecutive blocks of that many samples are averaged, at
//|         most `espsdr.MAX_SAMPLES` samples in all. Bin ``k`` is at
//|         ``(k - len(out) // 2) * sample_rate / len(out)`` Hz from the centre,
//|         so the spectrum runs from -sample_rate/2 up. The mean of the capture
//|         is taken off first and a Hann window applied.
//|
//|         * ``array('h')``: the level of each bin in 0.01 dB.
//|         * ``array('f')``: in dB.
//|
//|         The levels are relative to a full-scale tone in one bin (dBFS); the
//|         noise floor depends on the gain, which is an index, not dB.
//|
//|         :return: the gain index the receiver used.
//|         """
//|         ...
//|
static mp_obj_t espsdr_receiver_spectrum(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_self, ARG_out, ARG_average };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_self, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_out, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_average, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 1} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    espsdr_receiver_obj_t *self = MP_OBJ_TO_PTR(args[ARG_self].u_obj);
    check_for_deinit(self);

    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(args[ARG_out].u_obj, &bufinfo, MP_BUFFER_WRITE);
    char typecode = bufinfo.typecode;
    size_t nfft;
    if (typecode == 'h') {
        nfft = bufinfo.len / 2;
    } else if (typecode == 'f') {
        nfft = bufinfo.len / 4;
    } else {
        mp_raise_ValueError_varg(MP_ERROR_TEXT("%q must be an array of type 'h' or 'f'"), MP_QSTR_out);
    }
    if (nfft < 64 || nfft > FFT_MAX || (nfft & (nfft - 1)) != 0) {
        mp_raise_ValueError_varg(MP_ERROR_TEXT("%q length must be a power of two from 64 to %d"), MP_QSTR_out, FFT_MAX);
    }
    mp_int_t average = mp_arg_validate_int_range(args[ARG_average].u_int, 1, ESPSDR_MAX_SAMPLES / nfft, MP_QSTR_average);

    dsp_init_tables();
    int32_t *x = m_new(int32_t, 2 * nfft);
    uint64_t *acc = m_new(uint64_t, nfft);
    if (self->decimation > 1) {
        mp_raise_ValueError_varg(MP_ERROR_TEXT("%q must be one of SAMPLE_RATES"), MP_QSTR_sample_rate);
    }
    volatile uint32_t *words = acquire(self, nfft * average, hw_rate_bits(self->source));
    power_spectrum(words, nfft, average, x, acc);
    mp_int_t gain = (words[nfft * average - 1] >> 20) & 0xff;
    capture_buffer_free(words);

    // Negative frequencies first.
    int32_t reference = full_scale_db100(nfft);
    for (size_t h = 0; h < nfft; h++) {
        int32_t db100 = db100_u64(acc[(h + nfft / 2) % nfft] / average) - reference;
        if (typecode == 'h') {
            ((int16_t *)bufinfo.buf)[h] = db100;
        } else {
            ((float *)bufinfo.buf)[h] = db100 / 100.0f;
        }
    }
    m_del(int32_t, x, 2 * nfft);
    m_del(uint64_t, acc, nfft);
    return MP_OBJ_NEW_SMALL_INT(gain);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(espsdr_receiver_spectrum_obj, 2, espsdr_receiver_spectrum);

//|     frequency: float
//|     """Centre frequency in MHz, as the constructor takes it: an int when it is
//|     a whole number of MHz, else a float."""
//|
static mp_obj_t espsdr_receiver_get_frequency(mp_obj_t self_in) {
    espsdr_receiver_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    if (self->frequency % 1000 == 0) {
        return MP_OBJ_NEW_SMALL_INT(self->frequency / 1000);
    }
    return mp_obj_new_float(self->frequency / MICROPY_FLOAT_CONST(1000.0));
}
MP_DEFINE_CONST_FUN_OBJ_1(espsdr_receiver_get_frequency_obj, espsdr_receiver_get_frequency);

static mp_obj_t espsdr_receiver_set_frequency(mp_obj_t self_in, mp_obj_t value) {
    espsdr_receiver_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    self->frequency = frequency_khz(value);
    configure_or_raise(self);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(espsdr_receiver_set_frequency_obj, espsdr_receiver_set_frequency);

MP_PROPERTY_GETSET(espsdr_receiver_frequency_obj,
    (mp_obj_t)&espsdr_receiver_get_frequency_obj,
    (mp_obj_t)&espsdr_receiver_set_frequency_obj);

//|     sample_rate: int
//|     """Samples per second, one of the rates the constructor takes. Setting
//|     it takes effect with the next capture."""
//|
static mp_obj_t espsdr_receiver_get_sample_rate(mp_obj_t self_in) {
    espsdr_receiver_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    return mp_obj_new_int_from_uint(self->sample_rate);
}
MP_DEFINE_CONST_FUN_OBJ_1(espsdr_receiver_get_sample_rate_obj, espsdr_receiver_get_sample_rate);

static mp_obj_t espsdr_receiver_set_sample_rate(mp_obj_t self_in, mp_obj_t value) {
    espsdr_receiver_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    uint32_t rate = mp_obj_get_int(value);
    plan_rate(self, rate, self->bandwidth, self->filtered);
    self->sample_rate = rate;
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(espsdr_receiver_set_sample_rate_obj, espsdr_receiver_set_sample_rate);

MP_PROPERTY_GETSET(espsdr_receiver_sample_rate_obj,
    (mp_obj_t)&espsdr_receiver_get_sample_rate_obj,
    (mp_obj_t)&espsdr_receiver_set_sample_rate_obj);

//|     gain: Optional[int]
//|     """PHY gain index (0 to `max_gain`), or None for the hardware AGC. A
//|     value above `max_gain` is lowered to it."""
//|
static mp_obj_t espsdr_receiver_get_gain(mp_obj_t self_in) {
    espsdr_receiver_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    return self->gain < 0 ? mp_const_none : MP_OBJ_NEW_SMALL_INT(self->gain);
}
MP_DEFINE_CONST_FUN_OBJ_1(espsdr_receiver_get_gain_obj, espsdr_receiver_get_gain);

static mp_obj_t espsdr_receiver_set_gain(mp_obj_t self_in, mp_obj_t value) {
    espsdr_receiver_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    self->gain = value == mp_const_none ? -1 :
        mp_arg_validate_int_range(mp_obj_get_int(value), 0, GAIN_MAX, MP_QSTR_gain);
    configure_or_raise(self);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(espsdr_receiver_set_gain_obj, espsdr_receiver_set_gain);

MP_PROPERTY_GETSET(espsdr_receiver_gain_obj,
    (mp_obj_t)&espsdr_receiver_get_gain_obj,
    (mp_obj_t)&espsdr_receiver_set_gain_obj);

//|     bandwidth: Optional[int]
//|     """Analog filter width in MHz (13 to 69 on the ESP32-S3, 11 to 48 on the
//|     ESP32-C5, 0 for the widest), applied for the length of each capture
//|     only; None keeps the PHY's own, about 20 MHz. Approximate: the curve was
//|     measured on one board. On the ESP32-C5 setting it retunes the radio."""
//|
static mp_obj_t espsdr_receiver_get_bandwidth(mp_obj_t self_in) {
    espsdr_receiver_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    return self->bandwidth < 0 ? mp_const_none : MP_OBJ_NEW_SMALL_INT(self->bandwidth);
}
MP_DEFINE_CONST_FUN_OBJ_1(espsdr_receiver_get_bandwidth_obj, espsdr_receiver_get_bandwidth);

static mp_obj_t espsdr_receiver_set_bandwidth(mp_obj_t self_in, mp_obj_t value) {
    espsdr_receiver_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    int8_t bandwidth = validate_bandwidth(value);
    // A wider filter may need a faster source for a decimated rate.
    plan_rate(self, self->sample_rate, bandwidth, self->filtered);
    self->bandwidth = bandwidth;
    configure_or_raise(self);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(espsdr_receiver_set_bandwidth_obj, espsdr_receiver_set_bandwidth);

MP_PROPERTY_GETSET(espsdr_receiver_bandwidth_obj,
    (mp_obj_t)&espsdr_receiver_get_bandwidth_obj,
    (mp_obj_t)&espsdr_receiver_set_bandwidth_obj);

//|     max_gain: int
//|     """Highest gain index the PHY calibrated for the current channel."""
//|
static mp_obj_t espsdr_receiver_get_max_gain(mp_obj_t self_in) {
    espsdr_receiver_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    return MP_OBJ_NEW_SMALL_INT(max_gain());
}
MP_DEFINE_CONST_FUN_OBJ_1(espsdr_receiver_get_max_gain_obj, espsdr_receiver_get_max_gain);

MP_PROPERTY_GETTER(espsdr_receiver_max_gain_obj,
    (mp_obj_t)&espsdr_receiver_get_max_gain_obj);

//|     filtered: bool
//|     """Whether every rate below the fastest is decimated on the chip, the
//|     hardware's own lower rates included. Decimated captures are free of
//|     aliases but shorter (see `max_samples`)."""
//|
static mp_obj_t espsdr_receiver_get_filtered(mp_obj_t self_in) {
    espsdr_receiver_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    return mp_obj_new_bool(self->filtered);
}
MP_DEFINE_CONST_FUN_OBJ_1(espsdr_receiver_get_filtered_obj, espsdr_receiver_get_filtered);

static mp_obj_t espsdr_receiver_set_filtered(mp_obj_t self_in, mp_obj_t value) {
    espsdr_receiver_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    bool filtered = mp_obj_is_true(value);
    plan_rate(self, self->sample_rate, self->bandwidth, filtered);
    self->filtered = filtered;
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(espsdr_receiver_set_filtered_obj, espsdr_receiver_set_filtered);

MP_PROPERTY_GETSET(espsdr_receiver_filtered_obj,
    (mp_obj_t)&espsdr_receiver_get_filtered_obj,
    (mp_obj_t)&espsdr_receiver_set_filtered_obj);

//|     max_samples: int
//|     """The most samples one capture can hold at the current rate:
//|     `espsdr.MAX_SAMPLES` at the hardware's rates; fewer when decimating,
//|     each output sample taking ``hardware rate / sample_rate`` input ones."""
//|
//|
static mp_obj_t espsdr_receiver_get_max_samples(mp_obj_t self_in) {
    espsdr_receiver_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    return MP_OBJ_NEW_SMALL_INT(max_samples(self));
}
MP_DEFINE_CONST_FUN_OBJ_1(espsdr_receiver_get_max_samples_obj, espsdr_receiver_get_max_samples);

MP_PROPERTY_GETTER(espsdr_receiver_max_samples_obj,
    (mp_obj_t)&espsdr_receiver_get_max_samples_obj);

static const mp_rom_map_elem_t espsdr_receiver_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&espsdr_receiver_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&espsdr_receiver_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&espsdr_receiver___exit___obj) },
    { MP_ROM_QSTR(MP_QSTR_capture), MP_ROM_PTR(&espsdr_receiver_capture_obj) },
    { MP_ROM_QSTR(MP_QSTR_spectrum), MP_ROM_PTR(&espsdr_receiver_spectrum_obj) },
    { MP_ROM_QSTR(MP_QSTR_filtered), MP_ROM_PTR(&espsdr_receiver_filtered_obj) },
    #if defined(CONFIG_IDF_TARGET_ESP32C5)
    { MP_ROM_QSTR(MP_QSTR_calibrate_dc), MP_ROM_PTR(&espsdr_receiver_calibrate_dc_obj) },
    #endif
    { MP_ROM_QSTR(MP_QSTR_max_samples), MP_ROM_PTR(&espsdr_receiver_max_samples_obj) },
    { MP_ROM_QSTR(MP_QSTR_frequency), MP_ROM_PTR(&espsdr_receiver_frequency_obj) },
    { MP_ROM_QSTR(MP_QSTR_sample_rate), MP_ROM_PTR(&espsdr_receiver_sample_rate_obj) },
    { MP_ROM_QSTR(MP_QSTR_gain), MP_ROM_PTR(&espsdr_receiver_gain_obj) },
    { MP_ROM_QSTR(MP_QSTR_bandwidth), MP_ROM_PTR(&espsdr_receiver_bandwidth_obj) },
    { MP_ROM_QSTR(MP_QSTR_max_gain), MP_ROM_PTR(&espsdr_receiver_max_gain_obj) },
};
static MP_DEFINE_CONST_DICT(espsdr_receiver_locals_dict, espsdr_receiver_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    espsdr_receiver_type,
    MP_QSTR_Receiver,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, espsdr_receiver_make_new,
    locals_dict, &espsdr_receiver_locals_dict
    );
