// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Petr Vavrin
//
// SPDX-License-Identifier: MIT

// espidf.RMTOut and espidf.RMTIn; see extended.h.

#include <string.h>

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

#include "driver/rmt_rx.h"
#include "driver/rmt_tx.h"
#include "esp_heap_caps.h"
#include "esp_private/rmt.h"
#include "hal/rmt_ll.h"

// The RMT peripheral as such: arbitrary pulse trains out and in, timed by the
// channel clock. pulseio uses it at a fixed 1 us; this exposes the resolution
// (up to 80 MHz), the hardware's own repeat of a sequence, and the receiver's
// glitch filter and end-of-frame threshold.

#if CIRCUITPY_ESPIDF_RMT

#define RMT_MAX_TICKS (32767)
// More than either chip has channels of one direction.
#define RMT_OPEN_MAX (8)

static void *_open_out[RMT_OPEN_MAX];
static void *_open_in[RMT_OPEN_MAX];

static void track(void **list, void *obj, void *old) {
    for (size_t i = 0; i < RMT_OPEN_MAX; i++) {
        if (list[i] == old) {
            list[i] = obj;
            return;
        }
    }
}

static void check_channel(esp_err_t err) {
    if (err == ESP_ERR_NOT_FOUND) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("All RMT channels in use"));
    }
    CHECK_ESP_RESULT(err);
}

static void validate_resolution(mp_int_t hz) {
    mp_arg_validate_int_range(hz, 1000, 80000000, MP_QSTR_resolution);
}

// --- RMTOut ------------------------------------------------------------------

typedef struct {
    mp_obj_base_t base;
    rmt_channel_handle_t chan;
    rmt_encoder_handle_t encoder;
    // The symbols of the transmission in progress, which the driver reads
    // from while it runs; outside the Python heap.
    rmt_symbol_word_t *symbols;
    uint32_t resolution;
    bool idle;
    bool forever;
    // The last transmission looped in hardware.
    bool looped;
    uint8_t pin;
} espidf_rmt_out_obj_t;

static void out_close(espidf_rmt_out_obj_t *self) {
    if (self->chan == NULL) {
        return;
    }
    rmt_disable(self->chan);
    rmt_del_encoder(self->encoder);
    rmt_del_channel(self->chan);
    self->chan = NULL;
    heap_caps_free(self->symbols);
    self->symbols = NULL;
    reset_pin_number(self->pin);
    track(_open_out, NULL, self);
}

static espidf_rmt_out_obj_t *get_out(mp_obj_t self_in) {
    espidf_rmt_out_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->chan == NULL) {
        raise_deinited_error();
    }
    return self;
}

// Aborts whatever is being sent, a repeat-forever included.
static void out_stop(espidf_rmt_out_obj_t *self) {
    rmt_disable(self->chan);
    CHECK_ESP_RESULT(rmt_enable(self->chan));
    self->forever = false;
    self->looped = false;
}

static bool out_wait(espidf_rmt_out_obj_t *self, mp_float_t timeout) {
    uint64_t end = supervisor_ticks_ms64() + (uint64_t)(timeout * 1000);
    esp_err_t err;
    while ((err = rmt_tx_wait_all_done(self->chan, 0)) == ESP_ERR_TIMEOUT) {
        RUN_BACKGROUND_TASKS;
        if (mp_hal_is_interrupted() || (timeout >= 0 && supervisor_ticks_ms64() > end)) {
            return false;
        }
    }
    CHECK_ESP_RESULT(err);
    return true;
}

//| class RMTOut:
//|     """A pulse train out of the RMT peripheral: durations in ticks of the
//|     channel clock, levels alternating. Unlike `pulseio.PulseOut` the clock
//|     is set (to 12.5 ns), and the hardware can repeat a short sequence by
//|     itself, a set number of times or until `stop`."""
//|
//|     def __init__(self, pin: microcontroller.Pin, *, resolution: int = 10_000_000, carrier_frequency: int = 0, carrier_duty: float = 0.5, idle: bool = False) -> None:
//|         """
//|         :param int resolution: ticks per second, 1 kHz to 80 MHz; a tick
//|           count of 1 to 32767 per duration.
//|         :param int carrier_frequency: if not 0, high levels are sent as a
//|           carrier of this frequency (an IR remote's 38 kHz).
//|         :param float carrier_duty: of the carrier, 0.0 to 1.0.
//|         :param bool idle: the level between transmissions."""
//|         ...
//|
static mp_obj_t espidf_rmt_out_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_pin, ARG_resolution, ARG_carrier_frequency, ARG_carrier_duty, ARG_idle };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_pin, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_resolution, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 10000000 } },
        { MP_QSTR_carrier_frequency, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_carrier_duty, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = MP_OBJ_NULL } },
        { MP_QSTR_idle, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = false } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    const mcu_pin_obj_t *pin = validate_obj_is_free_pin(args[ARG_pin].u_obj, MP_QSTR_pin);
    mp_int_t resolution = args[ARG_resolution].u_int;
    validate_resolution(resolution);
    mp_int_t carrier = mp_arg_validate_int_range(args[ARG_carrier_frequency].u_int, 0, 40000000, MP_QSTR_carrier_frequency);
    mp_float_t carrier_duty = args[ARG_carrier_duty].u_obj == MP_OBJ_NULL ? (mp_float_t)0.5 :
        mp_arg_validate_obj_float_range(args[ARG_carrier_duty].u_obj, 0, 1, MP_QSTR_carrier_duty);
    bool idle = args[ARG_idle].u_bool;

    espidf_rmt_out_obj_t *self = mp_obj_malloc_with_finaliser(espidf_rmt_out_obj_t, &espidf_rmt_out_type);
    self->chan = NULL;
    self->encoder = NULL;
    self->symbols = NULL;
    self->idle = idle;
    self->forever = false;
    self->looped = false;
    self->pin = pin->number;
    rmt_tx_channel_config_t config = {
        .gpio_num = pin->number,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = resolution,
        .mem_block_symbols = SOC_RMT_MEM_WORDS_PER_CHANNEL,
        .trans_queue_depth = 1,
        .flags.init_level = idle,
    };
    rmt_channel_handle_t chan;
    check_channel(rmt_new_tx_channel(&config, &chan));
    rmt_copy_encoder_config_t encoder_config = {};
    esp_err_t err = rmt_new_copy_encoder(&encoder_config, &self->encoder);
    if (err == ESP_OK && carrier != 0) {
        rmt_carrier_config_t carrier_config = {
            .frequency_hz = carrier,
            .duty_cycle = carrier_duty,
        };
        err = rmt_apply_carrier(chan, &carrier_config);
    }
    if (err == ESP_OK) {
        err = rmt_enable(chan);
    }
    if (err != ESP_OK) {
        if (self->encoder != NULL) {
            rmt_del_encoder(self->encoder);
        }
        rmt_del_channel(chan);
        CHECK_ESP_RESULT(err);
    }
    rmt_get_channel_resolution(chan, &self->resolution);
    self->chan = chan;
    claim_pin(pin);
    track(_open_out, self, NULL);
    return MP_OBJ_FROM_PTR(self);
}

//|     def send(self, durations: Sequence[int], *, level: bool = True, repeat: int = 1, wait: bool = True) -> None:
//|         """Send ``durations`` (ticks, 1 to 32767 each), the first at
//|         ``level`` and then alternating. Waits for a transmission still
//|         running, or stops one repeating forever.
//|
//|         :param int repeat: 1 sends once; more repeats the sequence in
//|           hardware that many times, 0 until `stop`. Repeating needs the
//|           sequence to fit in the channel's memory: 94 durations.
//|         :param bool wait: return when done (not with ``repeat=0``)."""
//|         ...
//|
static mp_obj_t espidf_rmt_out_send(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_self, ARG_durations, ARG_level, ARG_repeat, ARG_wait };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_self, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_durations, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_level, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = true } },
        { MP_QSTR_repeat, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 1 } },
        { MP_QSTR_wait, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = true } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    espidf_rmt_out_obj_t *self = get_out(args[ARG_self].u_obj);
    mp_int_t repeat = mp_arg_validate_int_min(args[ARG_repeat].u_int, 0, MP_QSTR_repeat);

    size_t n;
    mp_obj_t *items;
    mp_obj_get_array(args[ARG_durations].u_obj, &n, &items);
    mp_arg_validate_length_min(n, 1, MP_QSTR_durations);
    size_t symbols = (n + 1) / 2;
    if (repeat != 1) {
        // One symbol is left for the end marker the hardware loops on.
        mp_arg_validate_length_max(n, (SOC_RMT_MEM_WORDS_PER_CHANNEL - 1) * 2, MP_QSTR_durations);
    }

    if (self->forever) {
        out_stop(self);
    } else {
        out_wait(self, -1);
    }
    // A counted loop leaves its channel's TX-done status raised, which the
    // driver clears only when disabling a running channel. The next
    // transmission enables that interrupt, the stale event ends it before
    // it is set up, and the driver dereferenced a NULL transaction: a hard
    // fault. The channel is idle here, so clear the status first.
    if (self->looped) {
        int id;
        CHECK_ESP_RESULT(rmt_get_channel_id(self->chan, &id));
        rmt_ll_clear_interrupt_status(&RMT, RMT_LL_EVENT_TX_MASK(id));
        self->looped = false;
    }
    heap_caps_free(self->symbols);
    self->symbols = heap_caps_malloc(symbols * sizeof(rmt_symbol_word_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (self->symbols == NULL) {
        mp_raise_msg(&mp_type_MemoryError, NULL);
    }
    bool level = args[ARG_level].u_bool;
    for (size_t i = 0; i < symbols; i++) {
        uint32_t d0 = mp_arg_validate_int_range(mp_obj_get_int(items[2 * i]), 1, RMT_MAX_TICKS, MP_QSTR_durations);
        // An odd count ends on a zero duration, which the hardware takes as the end.
        uint32_t d1 = 2 * i + 1 < n ?
            mp_arg_validate_int_range(mp_obj_get_int(items[2 * i + 1]), 1, RMT_MAX_TICKS, MP_QSTR_durations) : 0;
        self->symbols[i] = (rmt_symbol_word_t) {
            .duration0 = d0, .level0 = level, .duration1 = d1, .level1 = !level,
        };
    }
    rmt_transmit_config_t config = {
        .loop_count = repeat == 1 ? 0 : (repeat == 0 ? -1 : repeat),
        .flags.eot_level = self->idle,
    };
    CHECK_ESP_RESULT(rmt_transmit(self->chan, self->encoder, self->symbols, symbols * sizeof(rmt_symbol_word_t), &config));
    self->forever = repeat == 0;
    self->looped = repeat != 1;
    if (args[ARG_wait].u_bool && !self->forever) {
        out_wait(self, -1);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(espidf_rmt_out_send_obj, 2, espidf_rmt_out_send);

//|     def wait(self, timeout: float = -1) -> bool:
//|         """Wait for the transmission to end; False if ``timeout`` seconds
//|         passed first (negative: no limit)."""
//|         ...
//|
static mp_obj_t espidf_rmt_out_wait(size_t n_args, const mp_obj_t *args) {
    espidf_rmt_out_obj_t *self = get_out(args[0]);
    mp_float_t timeout = n_args > 1 ? mp_obj_get_float(args[1]) : -1;
    return mp_obj_new_bool(out_wait(self, timeout));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(espidf_rmt_out_wait_obj, 1, 2, espidf_rmt_out_wait);

//|     def stop(self) -> None:
//|         """Abort the transmission; the pin goes to ``idle``."""
//|         ...
//|
static mp_obj_t espidf_rmt_out_stop(mp_obj_t self_in) {
    out_stop(get_out(self_in));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_rmt_out_stop_obj, espidf_rmt_out_stop);

//|     busy: bool
//|     """True while a transmission runs."""
//|
//|     resolution: int
//|     """Ticks per second as set: the clock divided by a whole number."""
//|
static mp_obj_t espidf_rmt_out_get_busy(mp_obj_t self_in) {
    espidf_rmt_out_obj_t *self = get_out(self_in);
    return mp_obj_new_bool(self->forever || rmt_tx_wait_all_done(self->chan, 0) == ESP_ERR_TIMEOUT);
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_rmt_out_get_busy_obj, espidf_rmt_out_get_busy);
MP_PROPERTY_GETTER(espidf_rmt_out_busy_obj, (mp_obj_t)&espidf_rmt_out_get_busy_obj);

static mp_obj_t espidf_rmt_out_get_resolution(mp_obj_t self_in) {
    return mp_obj_new_int_from_uint(get_out(self_in)->resolution);
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_rmt_out_get_resolution_obj, espidf_rmt_out_get_resolution);
MP_PROPERTY_GETTER(espidf_rmt_out_resolution_obj, (mp_obj_t)&espidf_rmt_out_get_resolution_obj);

//|     def deinit(self) -> None:
//|         """Stop and release the channel and the pin."""
//|         ...
//|
static mp_obj_t espidf_rmt_out_deinit(mp_obj_t self_in) {
    out_close(MP_OBJ_TO_PTR(self_in));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_rmt_out_deinit_obj, espidf_rmt_out_deinit);

static mp_obj_t espidf_rmt_out___exit__(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    return espidf_rmt_out_deinit(args[0]);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(espidf_rmt_out___exit___obj, 4, 4, espidf_rmt_out___exit__);

static const mp_rom_map_elem_t espidf_rmt_out_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_send), MP_ROM_PTR(&espidf_rmt_out_send_obj) },
    { MP_ROM_QSTR(MP_QSTR_wait), MP_ROM_PTR(&espidf_rmt_out_wait_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&espidf_rmt_out_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_busy), MP_ROM_PTR(&espidf_rmt_out_busy_obj) },
    { MP_ROM_QSTR(MP_QSTR_resolution), MP_ROM_PTR(&espidf_rmt_out_resolution_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&espidf_rmt_out_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&espidf_rmt_out_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&espidf_rmt_out___exit___obj) },
};
static MP_DEFINE_CONST_DICT(espidf_rmt_out_locals_dict, espidf_rmt_out_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    espidf_rmt_out_type,
    MP_QSTR_RMTOut,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, espidf_rmt_out_make_new,
    locals_dict, &espidf_rmt_out_locals_dict
    );

// --- RMTIn -------------------------------------------------------------------

typedef struct {
    mp_obj_base_t base;
    rmt_channel_handle_t chan;
    rmt_symbol_word_t *buf;
    size_t maxlen;
    rmt_receive_config_t config;
    uint32_t resolution;
    volatile size_t received;
    volatile bool armed;
    volatile bool done;
    uint8_t pin;
    bool owns_pin;
} espidf_rmt_in_obj_t;

static bool on_recv_done(rmt_channel_handle_t chan, const rmt_rx_done_event_data_t *edata, void *user_ctx) {
    espidf_rmt_in_obj_t *self = user_ctx;
    self->received = edata->num_symbols;
    self->done = true;
    return false;
}

static void in_close(espidf_rmt_in_obj_t *self) {
    if (self->chan == NULL) {
        return;
    }
    rmt_disable(self->chan);
    rmt_del_channel(self->chan);
    self->chan = NULL;
    heap_caps_free(self->buf);
    self->buf = NULL;
    if (self->owns_pin) {
        reset_pin_number(self->pin);
    }
    track(_open_in, NULL, self);
}

static espidf_rmt_in_obj_t *get_in(mp_obj_t self_in) {
    espidf_rmt_in_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->chan == NULL) {
        raise_deinited_error();
    }
    return self;
}

//| class RMTIn:
//|     """A pulse train into the RMT peripheral: durations of each level in
//|     ticks of the channel clock, from the first edge until the line has
//|     kept one level for ``idle_us``. Pulses shorter than ``filter_ns`` are
//|     ignored as glitches.
//|
//|     The pin may be one another object drives -- an `RMTOut` -- and is then
//|     only listened to, which receives one's own transmission with nothing
//|     wired."""
//|
//|     def __init__(self, pin: microcontroller.Pin, *, resolution: int = 1_000_000, idle_us: int = 10_000, filter_ns: int = 0, maxlen: int = 128) -> None:
//|         """
//|         :param int resolution: ticks per second, 1 kHz to 80 MHz.
//|         :param int idle_us: one level held this long ends a frame; at most
//|           32767 ticks.
//|         :param int filter_ns: 0 to about 3000 ns.
//|         :param int maxlen: durations kept, rounded up to an even count."""
//|         ...
//|
static mp_obj_t espidf_rmt_in_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_pin, ARG_resolution, ARG_idle_us, ARG_filter_ns, ARG_maxlen };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_pin, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_resolution, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 1000000 } },
        { MP_QSTR_idle_us, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 10000 } },
        { MP_QSTR_filter_ns, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_maxlen, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 128 } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    const mcu_pin_obj_t *pin = validate_obj_is_pin(args[ARG_pin].u_obj, MP_QSTR_pin);
    mp_int_t resolution = args[ARG_resolution].u_int;
    validate_resolution(resolution);
    mp_int_t idle_us = mp_arg_validate_int_min(args[ARG_idle_us].u_int, 1, MP_QSTR_idle_us);
    mp_int_t filter_ns = mp_arg_validate_int_range(args[ARG_filter_ns].u_int, 0, 3187, MP_QSTR_filter_ns);
    mp_int_t maxlen = mp_arg_validate_int_range(args[ARG_maxlen].u_int, 2, 32768, MP_QSTR_maxlen);

    espidf_rmt_in_obj_t *self = mp_obj_malloc_with_finaliser(espidf_rmt_in_obj_t, &espidf_rmt_in_type);
    self->chan = NULL;
    self->maxlen = (maxlen + 1) / 2;
    self->buf = heap_caps_malloc(self->maxlen * sizeof(rmt_symbol_word_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (self->buf == NULL) {
        mp_raise_msg(&mp_type_MemoryError, NULL);
    }
    self->armed = self->done = false;
    self->pin = pin->number;
    self->owns_pin = common_hal_mcu_pin_is_free(pin);
    self->config = (rmt_receive_config_t) {
        .signal_range_min_ns = filter_ns,
        .signal_range_max_ns = idle_us * 1000,
    };
    rmt_rx_channel_config_t config = {
        .gpio_num = pin->number,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = resolution,
        .mem_block_symbols = SOC_RMT_MEM_WORDS_PER_CHANNEL,
    };
    rmt_channel_handle_t chan;
    esp_err_t err = rmt_new_rx_channel(&config, &chan);
    if (err != ESP_OK) {
        heap_caps_free(self->buf);
        self->buf = NULL;
        check_channel(err);
    }
    rmt_rx_event_callbacks_t cbs = { .on_recv_done = on_recv_done };
    err = rmt_rx_register_event_callbacks(chan, &cbs, self);
    if (err == ESP_OK) {
        err = rmt_enable(chan);
    }
    if (err != ESP_OK) {
        rmt_del_channel(chan);
        heap_caps_free(self->buf);
        self->buf = NULL;
        CHECK_ESP_RESULT(err);
    }
    rmt_get_channel_resolution(chan, &self->resolution);
    self->chan = chan;
    if (self->owns_pin) {
        claim_pin(pin);
    }
    track(_open_in, self, NULL);
    return MP_OBJ_FROM_PTR(self);
}

static void in_arm(espidf_rmt_in_obj_t *self) {
    if (self->armed) {
        return;
    }
    self->done = false;
    self->received = 0;
    CHECK_ESP_RESULT(rmt_receive(self->chan, self->buf, self->maxlen * sizeof(rmt_symbol_word_t), &self->config));
    self->armed = true;
}

//|     def start(self) -> None:
//|         """Listen for the next frame, so that `read` gets one that begins
//|         before it is called -- such as this program's own `RMTOut`."""
//|         ...
//|
static mp_obj_t espidf_rmt_in_start(mp_obj_t self_in) {
    in_arm(get_in(self_in));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_rmt_in_start_obj, espidf_rmt_in_start);

//|     def read(self, timeout: float = 1.0) -> Optional[List[Tuple[bool, int]]]:
//|         """The next frame as ``(level, ticks)`` pairs, starting to listen
//|         if `start` has not; None if none ended within ``timeout`` seconds,
//|         and then listening stops."""
//|         ...
//|
static mp_obj_t espidf_rmt_in_read(size_t n_args, const mp_obj_t *args) {
    espidf_rmt_in_obj_t *self = get_in(args[0]);
    mp_float_t timeout = n_args > 1 ? mp_obj_get_float(args[1]) : (mp_float_t)1.0;
    in_arm(self);
    uint64_t end = supervisor_ticks_ms64() + (uint64_t)(timeout * 1000);
    while (!self->done) {
        RUN_BACKGROUND_TASKS;
        if (mp_hal_is_interrupted() || supervisor_ticks_ms64() > end) {
            // Disabling the channel ends the pending receive.
            rmt_disable(self->chan);
            CHECK_ESP_RESULT(rmt_enable(self->chan));
            self->armed = false;
            return mp_const_none;
        }
    }
    self->armed = false;
    mp_obj_t list = mp_obj_new_list(0, NULL);
    for (size_t i = 0; i < self->received; i++) {
        rmt_symbol_word_t s = self->buf[i];
        mp_obj_t pair[2] = { mp_obj_new_bool(s.level0), MP_OBJ_NEW_SMALL_INT(s.duration0) };
        mp_obj_list_append(list, mp_obj_new_tuple(2, pair));
        // A zero duration marks where the idle threshold ended the frame.
        if (s.duration1 == 0) {
            break;
        }
        pair[0] = mp_obj_new_bool(s.level1);
        pair[1] = MP_OBJ_NEW_SMALL_INT(s.duration1);
        mp_obj_list_append(list, mp_obj_new_tuple(2, pair));
    }
    return list;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(espidf_rmt_in_read_obj, 1, 2, espidf_rmt_in_read);

//|     resolution: int
//|     """Ticks per second as set: the clock divided by a whole number."""
//|
static mp_obj_t espidf_rmt_in_get_resolution(mp_obj_t self_in) {
    return mp_obj_new_int_from_uint(get_in(self_in)->resolution);
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_rmt_in_get_resolution_obj, espidf_rmt_in_get_resolution);
MP_PROPERTY_GETTER(espidf_rmt_in_resolution_obj, (mp_obj_t)&espidf_rmt_in_get_resolution_obj);

//|     def deinit(self) -> None:
//|         """Release the channel, and the pin if this object took it."""
//|         ...
//|
static mp_obj_t espidf_rmt_in_deinit(mp_obj_t self_in) {
    in_close(MP_OBJ_TO_PTR(self_in));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_rmt_in_deinit_obj, espidf_rmt_in_deinit);

static mp_obj_t espidf_rmt_in___exit__(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    return espidf_rmt_in_deinit(args[0]);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(espidf_rmt_in___exit___obj, 4, 4, espidf_rmt_in___exit__);

static const mp_rom_map_elem_t espidf_rmt_in_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_start), MP_ROM_PTR(&espidf_rmt_in_start_obj) },
    { MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&espidf_rmt_in_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_resolution), MP_ROM_PTR(&espidf_rmt_in_resolution_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&espidf_rmt_in_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&espidf_rmt_in_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&espidf_rmt_in___exit___obj) },
};
static MP_DEFINE_CONST_DICT(espidf_rmt_in_locals_dict, espidf_rmt_in_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    espidf_rmt_in_type,
    MP_QSTR_RMTIn,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, espidf_rmt_in_make_new,
    locals_dict, &espidf_rmt_in_locals_dict
    );

void espidf_rmt_reset(void) {
    for (size_t i = 0; i < RMT_OPEN_MAX; i++) {
        if (_open_in[i] != NULL) {
            in_close(_open_in[i]);
        }
        if (_open_out[i] != NULL) {
            out_close(_open_out[i]);
        }
    }
}

#endif
