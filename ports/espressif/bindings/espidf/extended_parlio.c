// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Petr Vavrin
//
// SPDX-License-Identifier: MIT

// espidf.ParallelOut and espidf.ParallelIn; see extended.h.

#include <string.h>

#include "py/mperrno.h"
#include "py/obj.h"
#include "py/objproperty.h"
#include "py/runtime.h"

#include "bindings/espidf/__init__.h"
#include "bindings/espidf/extended.h"
#include "common-hal/microcontroller/Pin.h"
#include "shared-bindings/microcontroller/Pin.h"
#include "shared-bindings/util.h"
#include "shared/runtime/context_manager_helpers.h"

// The headers below exist only where the switch brings them in.
#if CIRCUITPY_ESPIDF_PARLIO

#include "driver/parlio_rx.h"
#include "driver/parlio_tx.h"
#include "esp_heap_caps.h"

// The parallel IO peripheral: 1, 2, 4 or 8 pins clocked out of, or sampled
// into, memory by DMA at a fixed rate. Out, a pattern generator; in, a
// logic analyser. The DMA works from internal RAM, so data is copied between
// it and the Python buffer, which may be in PSRAM.

#define PARLIO_MAX_PINS (8)
// One RX frame is at most 65535 bytes; TX is kept to the same so that one
// internal buffer serves either.
#define PARLIO_MAX_BYTES (65535)

typedef struct {
    mp_obj_base_t base;
    parlio_tx_unit_handle_t unit;
    uint8_t *dma_buf;
    size_t dma_len;
    uint32_t frequency;
    uint8_t pins[PARLIO_MAX_PINS];
    uint8_t width;
    bool looping;
} espidf_parallel_out_obj_t;

typedef struct {
    mp_obj_base_t base;
    parlio_rx_unit_handle_t unit;
    parlio_rx_delimiter_handle_t delimiter;
    uint32_t frequency;
    uint8_t pins[PARLIO_MAX_PINS];
    uint32_t claimed;  // bit i: pins[i] was claimed by this object
    uint8_t width;
} espidf_parallel_in_obj_t;

// One unit each on the C5.
static espidf_parallel_out_obj_t *_out;
static espidf_parallel_in_obj_t *_in;

// Pins as a tuple or list of 1, 2, 4 or 8.
static size_t get_pins(mp_obj_t pins_in, const mcu_pin_obj_t **pins, bool allow_own_output) {
    size_t len;
    mp_obj_t *items;
    mp_obj_get_array(pins_in, &len, &items);
    if (len != 1 && len != 2 && len != 4 && len != 8) {
        mp_raise_ValueError(MP_ERROR_TEXT("pins must be 1, 2, 4 or 8 pins"));
    }
    for (size_t i = 0; i < len; i++) {
        const mcu_pin_obj_t *pin = validate_obj_is_pin(items[i], MP_QSTR_pins);
        bool own = false;
        if (allow_own_output && _out != NULL) {
            for (size_t j = 0; j < _out->width; j++) {
                own |= _out->pins[j] == pin->number;
            }
        }
        if (!own) {
            assert_pin_free(pin);
        }
        for (size_t j = 0; j < i; j++) {
            if (pins[j] == pin) {
                mp_raise_ValueError(MP_ERROR_TEXT("pins must differ"));
            }
        }
        pins[i] = pin;
    }
    return len;
}

// --- ParallelOut -------------------------------------------------------------

static void out_close(espidf_parallel_out_obj_t *self) {
    if (self->unit == NULL) {
        return;
    }
    parlio_tx_unit_disable(self->unit);
    parlio_del_tx_unit(self->unit);
    self->unit = NULL;
    heap_caps_free(self->dma_buf);
    self->dma_buf = NULL;
    for (size_t i = 0; i < self->width; i++) {
        reset_pin_number(self->pins[i]);
    }
    if (_out == self) {
        _out = NULL;
    }
}

static void out_check(espidf_parallel_out_obj_t *self) {
    if (self->unit == NULL) {
        raise_deinited_error();
    }
}

// Stop a looping transmission: only disabling the unit ends one.
static void out_stop(espidf_parallel_out_obj_t *self) {
    if (self->looping) {
        CHECK_ESP_RESULT(parlio_tx_unit_disable(self->unit));
        CHECK_ESP_RESULT(parlio_tx_unit_enable(self->unit));
        self->looping = false;
    }
}

//| class ParallelOut:
//|     """1, 2, 4 or 8 pins clocked out of a buffer by DMA at a fixed rate: a
//|     pattern generator. ESP32-C5 only."""
//|
//|     def __init__(self, pins: Sequence[microcontroller.Pin], *, frequency: int = 1_000_000) -> None:
//|         """
//|         :param pins: 1, 2, 4 or 8 pins; the first is bit 0.
//|         :param int frequency: samples per second, 240 MHz divided by a whole
//|           number."""
//|         ...
//|
static mp_obj_t espidf_parallel_out_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_pins, ARG_frequency };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_pins, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_frequency, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 1000000 } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    if (_out != NULL) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("ParallelOut in use"));
    }
    const mcu_pin_obj_t *pins[PARLIO_MAX_PINS];
    size_t width = get_pins(args[ARG_pins].u_obj, pins, false);
    mp_int_t frequency = mp_arg_validate_int_range(args[ARG_frequency].u_int, 4000, 40000000, MP_QSTR_frequency);

    espidf_parallel_out_obj_t *self = mp_obj_malloc_with_finaliser(espidf_parallel_out_obj_t, &espidf_parallel_out_type);
    self->unit = NULL;
    self->width = width;
    self->frequency = frequency;
    self->looping = false;
    self->dma_len = PARLIO_MAX_BYTES;
    self->dma_buf = heap_caps_malloc(self->dma_len, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (self->dma_buf == NULL) {
        m_malloc_fail(self->dma_len);
    }

    parlio_tx_unit_config_t config = {
        .clk_src = PARLIO_CLK_SRC_DEFAULT,
        .clk_in_gpio_num = -1,
        .output_clk_freq_hz = frequency,
        .data_width = width,
        .clk_out_gpio_num = -1,
        .valid_gpio_num = -1,
        .trans_queue_depth = 2,
        .max_transfer_size = self->dma_len,
        .shift_edge = PARLIO_SHIFT_EDGE_POS,
        .bit_pack_order = PARLIO_BIT_PACK_ORDER_LSB,
    };
    for (size_t i = 0; i < PARLIO_TX_UNIT_MAX_DATA_WIDTH; i++) {
        config.data_gpio_nums[i] = i < width ? pins[i]->number : -1;
    }
    esp_err_t err = parlio_new_tx_unit(&config, &self->unit);
    if (err == ESP_OK) {
        err = parlio_tx_unit_enable(self->unit);
        if (err != ESP_OK) {
            parlio_del_tx_unit(self->unit);
            self->unit = NULL;
        }
    }
    if (err != ESP_OK) {
        heap_caps_free(self->dma_buf);
        self->dma_buf = NULL;
        raise_esp_error(err);
    }
    for (size_t i = 0; i < width; i++) {
        self->pins[i] = pins[i]->number;
        claim_pin(pins[i]);
    }
    _out = self;
    return MP_OBJ_FROM_PTR(self);
}

//|     def write(self, buffer: ReadableBuffer, *, loop: bool = False, timeout: float = 5.0) -> None:
//|         """Send ``buffer``: with 8 pins one byte per sample, with fewer the
//|         samples are packed from the low bits up. Up to 65535 bytes. Waits
//|         until it has gone out, or with ``loop`` repeats it until `stop` and
//|         returns at once."""
//|         ...
//|
static mp_obj_t espidf_parallel_out_write(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_self, ARG_buffer, ARG_loop, ARG_timeout };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_self, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_buffer, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_loop, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = false } },
        { MP_QSTR_timeout, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = MP_OBJ_NULL } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    espidf_parallel_out_obj_t *self = MP_OBJ_TO_PTR(args[ARG_self].u_obj);
    out_check(self);
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(args[ARG_buffer].u_obj, &bufinfo, MP_BUFFER_READ);
    mp_arg_validate_length_range(bufinfo.len, 1, self->dma_len, MP_QSTR_buffer);
    mp_float_t timeout = args[ARG_timeout].u_obj == MP_OBJ_NULL ? 5 : mp_obj_get_float(args[ARG_timeout].u_obj);

    out_stop(self);
    memcpy(self->dma_buf, bufinfo.buf, bufinfo.len);
    parlio_transmit_config_t tx = { .idle_value = 0 };
    tx.flags.loop_transmission = args[ARG_loop].u_bool;
    CHECK_ESP_RESULT(parlio_tx_unit_transmit(self->unit, self->dma_buf, bufinfo.len * 8, &tx));
    if (args[ARG_loop].u_bool) {
        self->looping = true;
        return mp_const_none;
    }
    esp_err_t err = parlio_tx_unit_wait_all_done(self->unit, (int)(timeout * 1000));
    if (err == ESP_ERR_TIMEOUT) {
        mp_raise_OSError(MP_ETIMEDOUT);
    }
    CHECK_ESP_RESULT(err);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(espidf_parallel_out_write_obj, 2, espidf_parallel_out_write);

//|     def stop(self) -> None:
//|         """End a looping `write`; the pins go low."""
//|         ...
//|
static mp_obj_t espidf_parallel_out_stop(mp_obj_t self_in) {
    espidf_parallel_out_obj_t *self = MP_OBJ_TO_PTR(self_in);
    out_check(self);
    out_stop(self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_parallel_out_stop_obj, espidf_parallel_out_stop);

//|     def deinit(self) -> None:
//|         """Stop and release the pins."""
//|         ...
//|
static mp_obj_t espidf_parallel_out_deinit(mp_obj_t self_in) {
    out_close(MP_OBJ_TO_PTR(self_in));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_parallel_out_deinit_obj, espidf_parallel_out_deinit);

//|     def __enter__(self) -> ParallelOut:
//|         """No-op used by Context Managers."""
//|         ...
//|
//|     def __exit__(self) -> None:
//|         """Automatically deinitializes when exiting a context."""
//|         ...
//|
//|
static mp_obj_t espidf_parallel_out___exit__(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    return espidf_parallel_out_deinit(args[0]);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(espidf_parallel_out___exit___obj, 4, 4, espidf_parallel_out___exit__);

static const mp_rom_map_elem_t espidf_parallel_out_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&espidf_parallel_out_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&espidf_parallel_out_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&espidf_parallel_out_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&espidf_parallel_out_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&espidf_parallel_out___exit___obj) },
};
static MP_DEFINE_CONST_DICT(espidf_parallel_out_locals_dict, espidf_parallel_out_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    espidf_parallel_out_type,
    MP_QSTR_ParallelOut,
    MP_TYPE_FLAG_NONE,
    make_new, espidf_parallel_out_make_new,
    locals_dict, &espidf_parallel_out_locals_dict
    );

// --- ParallelIn --------------------------------------------------------------

static void in_close(espidf_parallel_in_obj_t *self) {
    if (self->unit == NULL) {
        return;
    }
    parlio_del_rx_delimiter(self->delimiter);
    parlio_del_rx_unit(self->unit);
    self->unit = NULL;
    for (size_t i = 0; i < self->width; i++) {
        if (self->claimed & (1u << i)) {
            reset_pin_number(self->pins[i]);
        }
    }
    if (_in == self) {
        _in = NULL;
    }
}

//| class ParallelIn:
//|     """1, 2, 4 or 8 pins sampled into a buffer by DMA at a fixed rate: a
//|     logic analyser. ESP32-C5 only.
//|
//|     The pins may include those of this board's own `ParallelOut`, to watch
//|     what it sends; any other pin must be free."""
//|
//|     def __init__(self, pins: Sequence[microcontroller.Pin], *, frequency: int = 1_000_000) -> None:
//|         """
//|         :param pins: 1, 2, 4 or 8 pins; the first is bit 0.
//|         :param int frequency: samples per second, 240 MHz divided by a whole
//|           number."""
//|         ...
//|
static mp_obj_t espidf_parallel_in_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_pins, ARG_frequency };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_pins, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_frequency, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 1000000 } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    if (_in != NULL) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("ParallelIn in use"));
    }
    const mcu_pin_obj_t *pins[PARLIO_MAX_PINS];
    size_t width = get_pins(args[ARG_pins].u_obj, pins, true);
    mp_int_t frequency = mp_arg_validate_int_range(args[ARG_frequency].u_int, 4000, 40000000, MP_QSTR_frequency);

    espidf_parallel_in_obj_t *self = mp_obj_malloc_with_finaliser(espidf_parallel_in_obj_t, &espidf_parallel_in_type);
    self->unit = NULL;
    self->delimiter = NULL;
    self->width = width;
    self->frequency = frequency;
    self->claimed = 0;

    parlio_rx_unit_config_t config = {
        .trans_queue_depth = 2,
        .max_recv_size = PARLIO_MAX_BYTES,
        .data_width = width,
        .clk_src = PARLIO_CLK_SRC_DEFAULT,
        .exp_clk_freq_hz = frequency,
        .clk_in_gpio_num = -1,
        .clk_out_gpio_num = -1,
        .valid_gpio_num = -1,
    };
    for (size_t i = 0; i < PARLIO_RX_UNIT_MAX_DATA_WIDTH; i++) {
        config.data_gpio_nums[i] = i < width ? pins[i]->number : -1;
    }
    CHECK_ESP_RESULT(parlio_new_rx_unit(&config, &self->unit));
    parlio_rx_soft_delimiter_config_t delimiter = {
        .sample_edge = PARLIO_SAMPLE_EDGE_POS,
        .bit_pack_order = PARLIO_BIT_PACK_ORDER_LSB,
        .eof_data_len = PARLIO_MAX_BYTES,
    };
    esp_err_t err = parlio_new_rx_soft_delimiter(&delimiter, &self->delimiter);
    if (err != ESP_OK) {
        parlio_del_rx_unit(self->unit);
        self->unit = NULL;
        raise_esp_error(err);
    }
    for (size_t i = 0; i < width; i++) {
        self->pins[i] = pins[i]->number;
        if (pin_number_is_free(pins[i]->number)) {
            claim_pin(pins[i]);
            self->claimed |= 1u << i;
        }
    }
    _in = self;
    return MP_OBJ_FROM_PTR(self);
}

//|     def read(self, buffer: WriteableBuffer, *, timeout: float = 5.0) -> None:
//|         """Fill ``buffer`` with samples taken from now on: with 8 pins one byte
//|         per sample, with fewer packed from the low bits up. Up to 65535
//|         bytes."""
//|         ...
//|
static mp_obj_t espidf_parallel_in_read(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_self, ARG_buffer, ARG_timeout };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_self, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_buffer, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_timeout, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = MP_OBJ_NULL } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    espidf_parallel_in_obj_t *self = MP_OBJ_TO_PTR(args[ARG_self].u_obj);
    if (self->unit == NULL) {
        raise_deinited_error();
    }
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(args[ARG_buffer].u_obj, &bufinfo, MP_BUFFER_WRITE);
    mp_arg_validate_length_range(bufinfo.len, 1, PARLIO_MAX_BYTES, MP_QSTR_buffer);
    mp_float_t timeout = args[ARG_timeout].u_obj == MP_OBJ_NULL ? 5 : mp_obj_get_float(args[ARG_timeout].u_obj);

    uint8_t *dma_buf = heap_caps_malloc(bufinfo.len, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (dma_buf == NULL) {
        m_malloc_fail(bufinfo.len);
    }
    // The delimiter's frame length is the buffer's, so that the frame ends
    // exactly when it is full.
    parlio_del_rx_delimiter(self->delimiter);
    self->delimiter = NULL;
    parlio_rx_soft_delimiter_config_t delimiter = {
        .sample_edge = PARLIO_SAMPLE_EDGE_POS,
        .bit_pack_order = PARLIO_BIT_PACK_ORDER_LSB,
        .eof_data_len = bufinfo.len,
    };
    esp_err_t err = parlio_new_rx_soft_delimiter(&delimiter, &self->delimiter);
    if (err == ESP_OK) {
        err = parlio_rx_unit_enable(self->unit, true);
    }
    if (err == ESP_OK) {
        parlio_receive_config_t rx = { .delimiter = self->delimiter };
        err = parlio_rx_unit_receive(self->unit, dma_buf, bufinfo.len, &rx);
        if (err == ESP_OK) {
            err = parlio_rx_soft_delimiter_start_stop(self->unit, self->delimiter, true);
        }
        if (err == ESP_OK) {
            err = parlio_rx_unit_wait_all_done(self->unit, (int)(timeout * 1000));
        }
        parlio_rx_soft_delimiter_start_stop(self->unit, self->delimiter, false);
        parlio_rx_unit_disable(self->unit);
    }
    if (err == ESP_OK) {
        memcpy(bufinfo.buf, dma_buf, bufinfo.len);
    }
    heap_caps_free(dma_buf);
    if (err == ESP_ERR_TIMEOUT) {
        mp_raise_OSError(MP_ETIMEDOUT);
    }
    CHECK_ESP_RESULT(err);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(espidf_parallel_in_read_obj, 2, espidf_parallel_in_read);

//|     def deinit(self) -> None:
//|         """Release the pins."""
//|         ...
//|
static mp_obj_t espidf_parallel_in_deinit(mp_obj_t self_in) {
    in_close(MP_OBJ_TO_PTR(self_in));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_parallel_in_deinit_obj, espidf_parallel_in_deinit);

//|     def __enter__(self) -> ParallelIn:
//|         """No-op used by Context Managers."""
//|         ...
//|
//|     def __exit__(self) -> None:
//|         """Automatically deinitializes when exiting a context."""
//|         ...
//|
//|
static mp_obj_t espidf_parallel_in___exit__(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    return espidf_parallel_in_deinit(args[0]);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(espidf_parallel_in___exit___obj, 4, 4, espidf_parallel_in___exit__);

static const mp_rom_map_elem_t espidf_parallel_in_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&espidf_parallel_in_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&espidf_parallel_in_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&espidf_parallel_in_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&espidf_parallel_in___exit___obj) },
};
static MP_DEFINE_CONST_DICT(espidf_parallel_in_locals_dict, espidf_parallel_in_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    espidf_parallel_in_type,
    MP_QSTR_ParallelIn,
    MP_TYPE_FLAG_NONE,
    make_new, espidf_parallel_in_make_new,
    locals_dict, &espidf_parallel_in_locals_dict
    );

void espidf_parlio_reset(void) {
    if (_in != NULL) {
        in_close(_in);
    }
    if (_out != NULL) {
        out_close(_out);
    }
}

#endif
