// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Petr Vavrin
//
// SPDX-License-Identifier: MIT

// espidf.TDM; see extended.h.

#include "py/mphal.h"
#include "py/obj.h"
#include "py/objproperty.h"
#include "py/runtime.h"

#include "bindings/espidf/__init__.h"
#include "bindings/espidf/extended.h"
#include "common-hal/microcontroller/Pin.h"
#include "shared-bindings/microcontroller/Pin.h"
#include "shared-bindings/util.h"
#include "shared/runtime/context_manager_helpers.h"
#include "supervisor/shared/tick.h"

// The headers below exist only where the switch brings them in.
#if CIRCUITPY_ESPIDF_I2S_TDM

#include "driver/i2s_tdm.h"

// I2S in TDM mode: up to 16 slots per frame on one data line, for microphone
// arrays and multichannel codecs, where audiobusio stops at two. Raw frames
// in and out of buffers, interleaved slot by slot, as the codec defines them.

typedef struct {
    mp_obj_base_t base;
    i2s_chan_handle_t tx;
    i2s_chan_handle_t rx;
    uint32_t sample_rate;
    uint8_t channels;
    uint8_t bits;
    uint8_t pins[5];
    uint8_t npins;
} espidf_tdm_obj_t;

static espidf_tdm_obj_t *_open[2];

static void tdm_close(espidf_tdm_obj_t *self) {
    if (self->tx == NULL && self->rx == NULL) {
        return;
    }
    if (self->tx != NULL) {
        i2s_channel_disable(self->tx);
        i2s_del_channel(self->tx);
        self->tx = NULL;
    }
    if (self->rx != NULL) {
        i2s_channel_disable(self->rx);
        i2s_del_channel(self->rx);
        self->rx = NULL;
    }
    for (int i = 0; i < self->npins; i++) {
        reset_pin_number(self->pins[i]);
    }
    self->npins = 0;
    for (size_t i = 0; i < MP_ARRAY_SIZE(_open); i++) {
        if (_open[i] == self) {
            _open[i] = NULL;
        }
    }
}

void espidf_i2s_tdm_reset(void) {
    for (size_t i = 0; i < MP_ARRAY_SIZE(_open); i++) {
        if (_open[i] != NULL) {
            tdm_close(_open[i]);
        }
    }
}

static espidf_tdm_obj_t *get_open(mp_obj_t self_in) {
    espidf_tdm_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->tx == NULL && self->rx == NULL) {
        raise_deinited_error();
    }
    return self;
}

// The same pin may be data out and data in: the driver then loops it back.
static int take_pin(espidf_tdm_obj_t *self, mp_obj_t obj, qstr arg) {
    if (obj == mp_const_none) {
        return I2S_GPIO_UNUSED;
    }
    const mcu_pin_obj_t *pin = validate_obj_is_pin(obj, arg);
    for (int i = 0; i < self->npins; i++) {
        if (self->pins[i] == pin->number) {
            return pin->number;
        }
    }
    if (!common_hal_mcu_pin_is_free(pin)) {
        raise_ValueError_invalid_pin_name(arg);
    }
    claim_pin(pin);
    self->pins[self->npins++] = pin->number;
    return pin->number;
}

//| class TDM:
//|     """I2S in TDM mode, as the controller: up to 16 slots per frame on one
//|     data line, for microphone arrays and multichannel codecs. Frames are
//|     raw and interleaved, slot 0 first. Out, in, or both at once on one
//|     controller (one on the C5, two on the S3) -- with the same pin for both
//|     the driver loops the data back, which tests it with nothing wired."""
//|
//|     def __init__(self, bit_clock: microcontroller.Pin, word_select: microcontroller.Pin, *, data_out: Optional[microcontroller.Pin] = None, data_in: Optional[microcontroller.Pin] = None, main_clock: Optional[microcontroller.Pin] = None, channels: int = 4, sample_rate: int = 16000, bits: int = 16, format: str = "philips") -> None:
//|         """
//|         :param int channels: active slots, 1 to 16.
//|         :param int bits: per sample, 8, 16, 24 or 32. 24-bit samples go
//|           out in 32-bit slots but take 3 bytes in a buffer, packed.
//|         :param str format: ``"philips"``, ``"msb"``, ``"pcm_short"`` or
//|           ``"pcm_long"``, as the codec wants its word select."""
//|         ...
//|
static mp_obj_t espidf_tdm_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_bit_clock, ARG_word_select, ARG_data_out, ARG_data_in, ARG_main_clock, ARG_channels, ARG_sample_rate, ARG_bits, ARG_format };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_bit_clock, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_word_select, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_data_out, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = mp_const_none } },
        { MP_QSTR_data_in, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = mp_const_none } },
        { MP_QSTR_main_clock, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = mp_const_none } },
        { MP_QSTR_channels, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 4 } },
        { MP_QSTR_sample_rate, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 16000 } },
        { MP_QSTR_bits, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 16 } },
        { MP_QSTR_format, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = MP_OBJ_NEW_QSTR(MP_QSTR_philips) } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    mp_int_t channels = mp_arg_validate_int_range(args[ARG_channels].u_int, 1, 16, MP_QSTR_channels);
    mp_int_t sample_rate = mp_arg_validate_int_range(args[ARG_sample_rate].u_int, 1000, 192000, MP_QSTR_sample_rate);
    mp_int_t bits = args[ARG_bits].u_int;
    if (bits != 8 && bits != 16 && bits != 24 && bits != 32) {
        mp_arg_error_invalid(MP_QSTR_bits);
    }
    qstr format = mp_obj_str_get_qstr(args[ARG_format].u_obj);
    if (format != MP_QSTR_philips && format != MP_QSTR_msb && format != MP_QSTR_pcm_short && format != MP_QSTR_pcm_long) {
        mp_arg_error_invalid(MP_QSTR_format);
    }
    if (args[ARG_data_out].u_obj == mp_const_none && args[ARG_data_in].u_obj == mp_const_none) {
        mp_raise_ValueError(MP_ERROR_TEXT("Need data_out, data_in or both"));
    }
    size_t slot = 0;
    while (slot < MP_ARRAY_SIZE(_open) && _open[slot] != NULL) {
        slot++;
    }
    if (slot == MP_ARRAY_SIZE(_open)) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("All I2S controllers in use"));
    }

    espidf_tdm_obj_t *self = mp_obj_malloc_with_finaliser(espidf_tdm_obj_t, &espidf_tdm_type);
    self->tx = self->rx = NULL;
    self->npins = 0;
    self->channels = channels;
    self->bits = bits;
    self->sample_rate = sample_rate;

    i2s_tdm_gpio_config_t gpio = {
        .bclk = take_pin(self, args[ARG_bit_clock].u_obj, MP_QSTR_bit_clock),
        .ws = take_pin(self, args[ARG_word_select].u_obj, MP_QSTR_word_select),
        .dout = take_pin(self, args[ARG_data_out].u_obj, MP_QSTR_data_out),
        .din = take_pin(self, args[ARG_data_in].u_obj, MP_QSTR_data_in),
        .mclk = take_pin(self, args[ARG_main_clock].u_obj, MP_QSTR_main_clock),
    };
    bool out = gpio.dout != I2S_GPIO_UNUSED;
    bool in = gpio.din != I2S_GPIO_UNUSED;

    i2s_chan_config_t chan_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    // Zeros, not the last buffer again, when the program does not keep up.
    chan_config.auto_clear = true;
    esp_err_t err = i2s_new_channel(&chan_config, out ? &self->tx : NULL, in ? &self->rx : NULL);
    if (err != ESP_OK) {
        for (int i = 0; i < self->npins; i++) {
            reset_pin_number(self->pins[i]);
        }
        self->npins = 0;
        if (err == ESP_ERR_NOT_FOUND) {
            mp_raise_RuntimeError(MP_ERROR_TEXT("All I2S controllers in use"));
        }
        CHECK_ESP_RESULT(err);
    }
    _open[slot] = self;

    i2s_tdm_slot_mask_t mask = (i2s_tdm_slot_mask_t)((1u << channels) - 1);
    i2s_data_bit_width_t width = (i2s_data_bit_width_t)bits;
    i2s_tdm_config_t config = {
        .clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(sample_rate),
        .gpio_cfg = gpio,
    };
    if (format == MP_QSTR_philips) {
        config.slot_cfg = (i2s_tdm_slot_config_t)I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(width, I2S_SLOT_MODE_STEREO, mask);
    } else if (format == MP_QSTR_msb) {
        config.slot_cfg = (i2s_tdm_slot_config_t)I2S_TDM_MSB_SLOT_DEFAULT_CONFIG(width, I2S_SLOT_MODE_STEREO, mask);
    } else if (format == MP_QSTR_pcm_short) {
        config.slot_cfg = (i2s_tdm_slot_config_t)I2S_TDM_PCM_SHORT_SLOT_DEFAULT_CONFIG(width, I2S_SLOT_MODE_STEREO, mask);
    } else {
        config.slot_cfg = (i2s_tdm_slot_config_t)I2S_TDM_PCM_LONG_SLOT_DEFAULT_CONFIG(width, I2S_SLOT_MODE_STEREO, mask);
    }
    if (bits == 24) {
        // 24-bit samples in 32-bit slots, so that the frame clock divides.
        config.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
        config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_384;
    }
    // The default 256 x the sample rate is too slow a main clock for many
    // slots: the bit clock has to fit channels x slot bits per frame.
    if (channels * (bits == 24 ? 32 : bits) > 256) {
        config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_512;
    }
    if (self->tx != NULL) {
        err = i2s_channel_init_tdm_mode(self->tx, &config);
    }
    if (err == ESP_OK && self->rx != NULL) {
        err = i2s_channel_init_tdm_mode(self->rx, &config);
    }
    if (err == ESP_OK && self->rx != NULL) {
        err = i2s_channel_enable(self->rx);
    }
    if (err == ESP_OK && self->tx != NULL) {
        err = i2s_channel_enable(self->tx);
    }
    if (err != ESP_OK) {
        tdm_close(self);
        CHECK_ESP_RESULT(err);
    }
    return MP_OBJ_FROM_PTR(self);
}

// Moves the buffer in chunks the driver takes within a short wait, running
// background tasks between them.
static size_t transfer(espidf_tdm_obj_t *self, bool out, mp_obj_t buf_in, mp_float_t timeout) {
    mp_buffer_info_t buf;
    mp_get_buffer_raise(buf_in, &buf, out ? MP_BUFFER_READ : MP_BUFFER_WRITE);
    i2s_chan_handle_t chan = out ? self->tx : self->rx;
    if (chan == NULL) {
        mp_raise_RuntimeError(out ? MP_ERROR_TEXT("No data_out") : MP_ERROR_TEXT("No data_in"));
    }
    size_t done = 0;
    uint64_t end = supervisor_ticks_ms64() + (uint64_t)(timeout * 1000);
    while (done < buf.len) {
        size_t n = 0;
        esp_err_t err = out ?
            i2s_channel_write(chan, (uint8_t *)buf.buf + done, buf.len - done, &n, 10) :
            i2s_channel_read(chan, (uint8_t *)buf.buf + done, buf.len - done, &n, 10);
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            CHECK_ESP_RESULT(err);
        }
        done += n;
        RUN_BACKGROUND_TASKS;
        if (mp_hal_is_interrupted() || supervisor_ticks_ms64() > end) {
            break;
        }
    }
    return done;
}

//|     def write(self, buffer: ReadableBuffer, *, timeout: float = 1.0) -> int:
//|         """Queue frames to send; returns the bytes taken, fewer if
//|         ``timeout`` passed first. When nothing is queued zeros go out, and
//|         queued frames follow the DMA buffers already filled: up to 6 x 240
//|         frames later (fewer with large frames, 4092 bytes per buffer)."""
//|         ...
//|
static mp_obj_t espidf_tdm_write(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_self, ARG_buffer, ARG_timeout };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_self, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_buffer, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_timeout, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = MP_OBJ_NULL } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    mp_float_t timeout = args[ARG_timeout].u_obj == MP_OBJ_NULL ? (mp_float_t)1.0 : mp_obj_get_float(args[ARG_timeout].u_obj);
    return mp_obj_new_int_from_uint(transfer(get_open(args[ARG_self].u_obj), true, args[ARG_buffer].u_obj, timeout));
}
static MP_DEFINE_CONST_FUN_OBJ_KW(espidf_tdm_write_obj, 2, espidf_tdm_write);

//|     def readinto(self, buffer: WriteableBuffer, *, timeout: float = 1.0) -> int:
//|         """Fill ``buffer`` with received frames, oldest first; returns the
//|         bytes read, fewer if ``timeout`` passed first. Receiving runs from
//|         the constructor on: the DMA buffers hold the first 6 x 240 frames
//|         that nothing read, and frames after that are dropped."""
//|         ...
//|
static mp_obj_t espidf_tdm_readinto(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_self, ARG_buffer, ARG_timeout };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_self, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_buffer, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_timeout, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = MP_OBJ_NULL } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    mp_float_t timeout = args[ARG_timeout].u_obj == MP_OBJ_NULL ? (mp_float_t)1.0 : mp_obj_get_float(args[ARG_timeout].u_obj);
    return mp_obj_new_int_from_uint(transfer(get_open(args[ARG_self].u_obj), false, args[ARG_buffer].u_obj, timeout));
}
static MP_DEFINE_CONST_FUN_OBJ_KW(espidf_tdm_readinto_obj, 2, espidf_tdm_readinto);

//|     frame_bytes: int
//|     """Bytes per frame in a buffer: channels x bytes per sample."""
//|
//|     sample_rate: int
//|     """Frames per second."""
//|
static mp_obj_t espidf_tdm_get_frame_bytes(mp_obj_t self_in) {
    espidf_tdm_obj_t *self = get_open(self_in);
    return MP_OBJ_NEW_SMALL_INT(self->channels * self->bits / 8);
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_tdm_get_frame_bytes_obj, espidf_tdm_get_frame_bytes);
MP_PROPERTY_GETTER(espidf_tdm_frame_bytes_obj, (mp_obj_t)&espidf_tdm_get_frame_bytes_obj);

static mp_obj_t espidf_tdm_get_sample_rate(mp_obj_t self_in) {
    return mp_obj_new_int_from_uint(get_open(self_in)->sample_rate);
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_tdm_get_sample_rate_obj, espidf_tdm_get_sample_rate);
MP_PROPERTY_GETTER(espidf_tdm_sample_rate_obj, (mp_obj_t)&espidf_tdm_get_sample_rate_obj);

//|     def deinit(self) -> None:
//|         """Stop and release the controller and the pins."""
//|         ...
//|
static mp_obj_t espidf_tdm_deinit(mp_obj_t self_in) {
    tdm_close(MP_OBJ_TO_PTR(self_in));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_tdm_deinit_obj, espidf_tdm_deinit);

static mp_obj_t espidf_tdm___exit__(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    return espidf_tdm_deinit(args[0]);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(espidf_tdm___exit___obj, 4, 4, espidf_tdm___exit__);

static const mp_rom_map_elem_t espidf_tdm_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&espidf_tdm_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_readinto), MP_ROM_PTR(&espidf_tdm_readinto_obj) },
    { MP_ROM_QSTR(MP_QSTR_frame_bytes), MP_ROM_PTR(&espidf_tdm_frame_bytes_obj) },
    { MP_ROM_QSTR(MP_QSTR_sample_rate), MP_ROM_PTR(&espidf_tdm_sample_rate_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&espidf_tdm_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&espidf_tdm_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&espidf_tdm___exit___obj) },
};
static MP_DEFINE_CONST_DICT(espidf_tdm_locals_dict, espidf_tdm_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    espidf_tdm_type,
    MP_QSTR_TDM,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, espidf_tdm_make_new,
    locals_dict, &espidf_tdm_locals_dict
    );

#endif
