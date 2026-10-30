// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Petr Vavrin
//
// SPDX-License-Identifier: MIT

#include <string.h>

#include "py/binary.h"
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
    uint16_t frequency;
    // -1 for the hardware AGC.
    int8_t gain;
    // -1 leaves the PHY's own filter; else the MHz asked for, 0 for widest.
    int8_t bandwidth;
    bool deinited;
    bool promiscuous_was_on;
} espsdr_receiver_obj_t;

// One radio, one receiver.
static espsdr_receiver_obj_t *_receiver = NULL;

static void check_for_deinit(espsdr_receiver_obj_t *self) {
    if (self->deinited) {
        raise_deinited_error();
    }
}

// Tune and switch the front end to receive only. Done again after every change
// of frequency, gain or bandwidth, because esp_wifi_set_channel() rebuilds the
// PHY state.
static esp_err_t configure(espsdr_receiver_obj_t *self) {
    esp_err_t err = esp_wifi_set_channel(channel_for(self->frequency), WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) {
        return err;
    }
    tune_bandwidth(self->frequency, self->bandwidth);
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

//| class Receiver:
//|     """Snapshot receiver on a Wi-Fi channel."""
//|
//|     def __init__(
//|         self,
//|         *,
//|         frequency: int = 2412,
//|         sample_rate: int = 80_000_000,
//|         gain: Optional[int] = None,
//|         bandwidth: Optional[int] = None,
//|     ) -> None:
//|         """Take over the radio. Wi-Fi must be enabled and idle.
//|
//|         :param int frequency: centre frequency in MHz. For now only Wi-Fi
//|           channel frequencies: 2412 to 2472 in steps of 5, and 2484; on the
//|           ESP32-C5 also the 5 GHz channels, 5180 to 5885 in steps of 20 as
//|           far as the country setting allows.
//|         :param int sample_rate: samples per second. ESP32-S3: 80_000_000,
//|           40_000_000 or 16_000_000. ESP32-C5: 80_000_000, 40_000_000,
//|           20_000_000, 10_000_000, 8_000_000 or 4_000_000. The lower rates
//|           appear to drop samples rather than filter, so out-of-band signals
//|           alias.
//|         :param int gain: a PHY gain index from 0 to `max_gain`, or None for
//|           the hardware AGC. Not dB.
//|         :param int bandwidth: analog filter width in MHz, or 0 for the
//|           widest: 13 to 69 on the ESP32-S3, 11 to 48 on the ESP32-C5. None
//|           keeps the PHY's own (about 20 MHz)."""
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
    enum { ARG_frequency, ARG_sample_rate, ARG_gain, ARG_bandwidth };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_frequency, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 2412} },
        { MP_QSTR_sample_rate, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 80000000} },
        { MP_QSTR_gain, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = mp_const_none} },
        { MP_QSTR_bandwidth, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = mp_const_none} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_int_t frequency = args[ARG_frequency].u_int;
    channel_for(frequency);
    uint32_t sample_rate = args[ARG_sample_rate].u_int;
    rate_bits(sample_rate);
    mp_int_t gain = -1;
    if (args[ARG_gain].u_obj != mp_const_none) {
        gain = mp_arg_validate_int_range(mp_obj_get_int(args[ARG_gain].u_obj), 0, GAIN_MAX, MP_QSTR_gain);
    }
    int8_t bandwidth = validate_bandwidth(args[ARG_bandwidth].u_obj);

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
//|         `espsdr.MAX_SAMPLES`:
//|
//|         * ``bytearray`` or ``array('b')``: I, Q interleaved, the top 8 of
//|           the 10 bits, signed. Two bytes per sample.
//|         * ``array('h')``: I, Q interleaved, all 10 bits, signed.
//|         * ``array('L')`` or ``array('I')``: the raw words as the hardware
//|           writes them: Q in bits 0-9 and I in bits 10-19 (two's complement),
//|           gain index in bits 20-27. Positive frequencies are above the
//|           centre with that assignment; the other way round mirrors them.
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
    } else if (typecode == 'L' || typecode == 'I') {
        n = bufinfo.len / 4;
    } else {
        mp_raise_ValueError_varg(MP_ERROR_TEXT("%q must be a bytearray or array of type 'b', 'h', 'I' or 'L'"), MP_QSTR_buffer);
    }
    n = mp_arg_validate_int_range(n, 1, ESPSDR_MAX_SAMPLES, MP_QSTR_buffer);

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

    bool complete = capture_into(words, n, rate_bits(self->sample_rate));

    if (self->bandwidth >= 0) {
        for (unsigned j = 0; j < 2; j++) {
            filter_write(j, filter_saved[j]);
        }
    }

    if (!complete) {
        capture_buffer_free(words);
        mp_raise_OSError(MP_ETIMEDOUT);
    }

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

//|     frequency: int
//|     """Centre frequency in MHz; a Wi-Fi channel frequency."""
//|
static mp_obj_t espsdr_receiver_get_frequency(mp_obj_t self_in) {
    espsdr_receiver_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    return MP_OBJ_NEW_SMALL_INT(self->frequency);
}
MP_DEFINE_CONST_FUN_OBJ_1(espsdr_receiver_get_frequency_obj, espsdr_receiver_get_frequency);

static mp_obj_t espsdr_receiver_set_frequency(mp_obj_t self_in, mp_obj_t value) {
    espsdr_receiver_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    mp_int_t mhz = mp_obj_get_int(value);
    channel_for(mhz);
    self->frequency = mhz;
    configure_or_raise(self);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(espsdr_receiver_set_frequency_obj, espsdr_receiver_set_frequency);

MP_PROPERTY_GETSET(espsdr_receiver_frequency_obj,
    (mp_obj_t)&espsdr_receiver_get_frequency_obj,
    (mp_obj_t)&espsdr_receiver_set_frequency_obj);

//|     sample_rate: int
//|     """Samples per second, one of the rates the constructor takes."""
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
    rate_bits(rate);
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
    self->bandwidth = validate_bandwidth(value);
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
//|
static mp_obj_t espsdr_receiver_get_max_gain(mp_obj_t self_in) {
    espsdr_receiver_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    return MP_OBJ_NEW_SMALL_INT(max_gain());
}
MP_DEFINE_CONST_FUN_OBJ_1(espsdr_receiver_get_max_gain_obj, espsdr_receiver_get_max_gain);

MP_PROPERTY_GETTER(espsdr_receiver_max_gain_obj,
    (mp_obj_t)&espsdr_receiver_get_max_gain_obj);

static const mp_rom_map_elem_t espsdr_receiver_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&espsdr_receiver_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&espsdr_receiver_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&espsdr_receiver___exit___obj) },
    { MP_ROM_QSTR(MP_QSTR_capture), MP_ROM_PTR(&espsdr_receiver_capture_obj) },
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
