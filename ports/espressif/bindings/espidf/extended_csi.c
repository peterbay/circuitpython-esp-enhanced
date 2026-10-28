// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Petr Vavrin
//
// SPDX-License-Identifier: MIT

// espidf.CSI; see extended.h.

#include <math.h>
#include <string.h>

#include "py/mpstate.h"
#include "py/obj.h"
#include "py/runtime.h"

#include "bindings/espidf/__init__.h"
#include "bindings/espidf/extended.h"

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_wifi.h"

// channel state information from the WiFi radio.
//
// For every frame the radio receives, the hardware reports how the channel
// distorted it. The pattern changes when something in the room moves, which is
// what makes it usable for presence and motion detection.
//
// Frames only arrive when the radio has a reason to receive them, so this needs
// either a connection or wifi.Monitor running alongside.
//
// The ESP-IDF callback runs in the WiFi task and its buffer is only valid for the
// duration of the call, so the record is copied into a ring buffer that Python
// drains. Dropping is preferred to blocking the radio.

#if CIRCUITPY_ESPIDF_CSI

// How much of each record is kept. 128 covers HT20 and HT40 on the pre-Wi-Fi-6
// parts, measured on an ESP32-S3. An HE20 record carries 242 tones rather than
// 64, so Wi-Fi 6 parts need room for roughly four times as much; that figure is
// sized from the tone count. defined() rather than a bare test: the build runs
// with -Wundef -Werror, and the macro is absent on parts without Wi-Fi 6.
#ifndef ESPIDF_CSI_MAX_BYTES
#if defined(CONFIG_SOC_WIFI_HE_SUPPORT)
#define ESPIDF_CSI_MAX_BYTES (512)
#else
#define ESPIDF_CSI_MAX_BYTES (128)
#endif
#endif

// The 64-bit field goes first so the rest packs without padding holes.
typedef struct {
    int64_t timestamp;
    uint16_t length;
    uint16_t rx_seq;
    uint8_t mac[6];
    int8_t rssi;
    int8_t noise_floor;
    uint8_t channel;
    uint8_t sig_mode;
    uint8_t rx_state;
    uint8_t cwb;
    uint8_t first_word_invalid;
    int8_t data[ESPIDF_CSI_MAX_BYTES];
} espidf_csi_record_t;

// Metadata slots filled by readinto(). int32 throughout so an array('i') reads
// them without unpacking, and the layout stays fixed as fields are added.
#define ESPIDF_CSI_META_INTS (16)

typedef struct {
    mp_obj_base_t base;
    espidf_csi_record_t *records;
    size_t size;
    volatile size_t head;
    volatile size_t tail;
    volatile size_t lost;
    uint8_t source[6];
    bool filtered;
    bool running;
} espidf_csi_obj_t;

// The callback needs to find the object without going through the heap. A
// static pointer is not something the collector looks at, though, so the same
// object is also held in a root pointer for as long as it runs: otherwise a
// program that dropped it without deinit() would have it collected while the
// Wi-Fi task kept writing into it.
static espidf_csi_obj_t *espidf_csi_singleton;
MP_REGISTER_ROOT_POINTER(mp_obj_t espidf_csi_active);

static void espidf_csi_cb(void *ctx, wifi_csi_info_t *info) {
    espidf_csi_obj_t *self = espidf_csi_singleton;
    if (self == NULL || !self->running || info == NULL || info->buf == NULL) {
        return;
    }
    // Channel state from two transmitters describes two different paths and
    // averaging them is meaningless, so an unwanted source is dropped before it
    // costs a queue slot.
    if (self->filtered && memcmp(info->mac, self->source, 6) != 0) {
        return;
    }
    size_t next = (self->head + 1) % self->size;
    if (next == self->tail) {
        self->lost++;
        return;
    }
    espidf_csi_record_t *rec = &self->records[self->head];
    rec->timestamp = esp_timer_get_time();
    rec->rssi = info->rx_ctrl.rssi;
    rec->noise_floor = info->rx_ctrl.noise_floor;
    rec->channel = info->rx_ctrl.channel;
    rec->rx_state = info->rx_ctrl.rx_state;
    // The receive metadata differs between generations as much as the
    // acquisition config does. Wi-Fi 6 parts name the format field
    // cur_bb_format and number it differently, and carry no bandwidth flag at
    // all -- a nonzero secondary channel is what says the frame was 40 MHz.
    #if defined(CONFIG_SOC_WIFI_HE_SUPPORT)
    rec->sig_mode = info->rx_ctrl.cur_bb_format;
    rec->cwb = info->rx_ctrl.second != 0;
    #else
    rec->sig_mode = info->rx_ctrl.sig_mode;
    rec->cwb = info->rx_ctrl.cwb;
    #endif
    rec->first_word_invalid = info->first_word_invalid ? 1 : 0;
    rec->rx_seq = info->rx_seq;
    memcpy(rec->mac, info->mac, 6);
    size_t len = info->len;
    if (len > ESPIDF_CSI_MAX_BYTES) {
        len = ESPIDF_CSI_MAX_BYTES;
    }
    rec->length = len;
    memcpy(rec->data, info->buf, len);
    self->head = next;
}

// Shared by readinto() and readinto_amplitude(): both hand back the same slots.
static void espidf_csi_fill_meta(espidf_csi_record_t *rec, int32_t *meta) {
    meta[0] = rec->rssi;
    meta[1] = rec->noise_floor;
    meta[2] = rec->channel;
    meta[3] = rec->sig_mode;
    meta[4] = rec->rx_state;
    meta[5] = rec->rx_seq;
    meta[6] = rec->cwb;
    meta[7] = rec->first_word_invalid;
    meta[8] = (int32_t)(uint32_t)(rec->timestamp & 0xFFFFFFFF);
    meta[9] = (int32_t)(rec->timestamp >> 32);
    for (size_t i = 0; i < 6; i++) {
        meta[10 + i] = rec->mac[i];
    }
}

// Checked before the queue is looked at, so that a buffer of the wrong size
// fails the same way whether or not a frame happens to be waiting. Validating
// after the empty-queue check would leave the mistake latent until the timing
// changed.
static bool espidf_csi_check_meta(mp_obj_t meta_in, mp_buffer_info_t *meta) {
    if (meta_in == mp_const_none) {
        return false;
    }
    mp_get_buffer_raise(meta_in, meta, MP_BUFFER_WRITE);
    if (meta->len < ESPIDF_CSI_META_INTS * sizeof(int32_t)) {
        mp_raise_ValueError(MP_ERROR_TEXT("meta must hold 16 32-bit ints"));
    }
    return true;
}

// The caller may pass any writable buffer, including a memoryview slice that
// starts off a word boundary, so the slots are built aligned and copied.
static void espidf_csi_store_meta(mp_buffer_info_t *meta, espidf_csi_record_t *rec) {
    int32_t slots[ESPIDF_CSI_META_INTS];
    espidf_csi_fill_meta(rec, slots);
    memcpy(meta->buf, slots, sizeof(slots));
}

//| class CSI:
//|     """Channel state information for received WiFi frames.
//|
//|     Where RSSI is one number per frame, this is the radio's estimate of the
//|     channel on each OFDM subcarrier. Being frequency resolved, it registers a
//|     change in the multipath around the antenna even when the received power
//|     does not move, which is what makes presence and motion detection possible.
//|
//|     Only frames the station itself receives are reported, and only while
//|     something is arriving, so this needs a connection and traffic on it.
//|     Pinging the gateway in a loop is the usual way to get a steady rate.
//|     A running `wifi.Monitor` suppresses CSI entirely rather than feeding it:
//|     measured on ESP-IDF v6.0.1, promiscuous mode yielded no records at all,
//|     on the connected channel or any other. Only one instance at a time.
//|
//|     Records accumulate in a queue outside the heap and are dropped rather than
//|     stalling the radio, so `lost()` is worth watching. `packet()` is the
//|     convenient reader; `readinto()` and `readinto_amplitude()` allocate nothing
//|     and are the ones that keep up at a useful frame rate."""
//|
//|     def __init__(
//|         self,
//|         queue: int = 16,
//|         *,
//|         source: ReadableBuffer | None = None,
//|         lltf: bool = True,
//|         htltf: bool = True,
//|         stbc_htltf2: bool = True,
//|         ltf_merge: bool = True,
//|         channel_filter: bool = True,
//|         shift: int | None = None,
//|         dump_ack: bool = False,
//|         he_su: bool = True,
//|         he_mu: bool = True,
//|         he_dcm: bool = True,
//|         he_beamformed: bool = True,
//|     ) -> None:
//|         """Start collecting. ``queue`` is how many records are buffered before
//|         the oldest ones start being dropped.
//|
//|         ``source`` is a six byte MAC address. Frames from anything else are
//|         discarded in the callback without taking a queue slot. Channel state
//|         from two transmitters describes two different paths, so anything that
//|         compares records over time wants this set.
//|
//|         ``lltf``, ``htltf`` and ``stbc_htltf2`` select which training fields
//|         are reported. Each adds to the record, and only the first
//|         ``ESPIDF_CSI_MAX_BYTES`` are kept, so leaving all three on truncates a
//|         HT frame. ``lltf`` alone gives the same 128 bytes for HT and non-HT
//|         frames, which is what makes records comparable.
//|
//|         ``shift`` is the scaling: `None` lets the radio scale automatically,
//|         a number fixes it instead. The range depends on the part -- 0 to 15
//|         where CSI is described the pre-Wi-Fi-6 way, 0 to 8 on Wi-Fi 6 parts
//|         with MAC version 3 (C5, C61) and 0 to 3 on MAC version 2 (C6).
//|
//|         Wi-Fi 6 parts describe acquisition differently and not every argument
//|         reaches the hardware on every chip:
//|
//|         ===================  ==========================  =================
//|         argument             pre-Wi-Fi-6 (ESP32, S2,     Wi-Fi 6 (C5, C6,
//|                              S3, C3, C2)                 C61)
//|         ===================  ==========================  =================
//|         ``lltf``             L-LTF                       L-LTF
//|         ``htltf``            HT-LTF                      HT-LTF, both 20 and
//|                                                          40 MHz
//|         ``stbc_htltf2``      STBC HT-LTF2                ignored
//|         ``ltf_merge``        averages L-LTF and HT-LTF   ignored
//|         ``channel_filter``   smooths adjacent carriers   ignored
//|         ``dump_ack``         ACK frames                  ACK frames
//|         ``he_su``            ignored                     HE-LTF, single user
//|         ``he_mu``            ignored                     HE-LTF, multi user
//|         ``he_dcm``           ignored                     HE-LTF, DCM
//|         ``he_beamformed``    ignored                     HE-LTF, beamformed
//|         ===================  ==========================  =================
//|
//|         An HE record holds far more than an HT one -- 242 tones against 64 --
//|         so ``ESPIDF_CSI_MAX_BYTES`` defaults higher on those parts."""
//|         ...
static mp_obj_t espidf_csi_make_new(const mp_obj_type_t *type, size_t n_args,
    size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_queue, ARG_source, ARG_lltf, ARG_htltf, ARG_stbc_htltf2,
           ARG_ltf_merge, ARG_channel_filter, ARG_shift, ARG_dump_ack,
           ARG_he_su, ARG_he_mu, ARG_he_dcm, ARG_he_beamformed };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_queue, MP_ARG_INT, { .u_int = 16 } },
        { MP_QSTR_source, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = mp_const_none } },
        { MP_QSTR_lltf, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = true } },
        { MP_QSTR_htltf, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = true } },
        { MP_QSTR_stbc_htltf2, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = true } },
        { MP_QSTR_ltf_merge, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = true } },
        { MP_QSTR_channel_filter, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = true } },
        { MP_QSTR_shift, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = mp_const_none } },
        { MP_QSTR_dump_ack, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = false } },
        { MP_QSTR_he_su, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = true } },
        { MP_QSTR_he_mu, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = true } },
        { MP_QSTR_he_dcm, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = true } },
        { MP_QSTR_he_beamformed, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = true } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    if (espidf_csi_singleton != NULL) {
        mp_raise_ValueError(MP_ERROR_TEXT("CSI is already running"));
    }
    mp_int_t queue = args[ARG_queue].u_int;
    if (queue < 2 || queue > 64) {
        mp_raise_ValueError(MP_ERROR_TEXT("queue must be 2 to 64"));
    }
    #if defined(CONFIG_SOC_WIFI_HE_SUPPORT)
    bool any_ltf = args[ARG_lltf].u_bool || args[ARG_htltf].u_bool ||
        args[ARG_he_su].u_bool || args[ARG_he_mu].u_bool ||
        args[ARG_he_dcm].u_bool || args[ARG_he_beamformed].u_bool;
    #else
    bool any_ltf = args[ARG_lltf].u_bool || args[ARG_htltf].u_bool ||
        args[ARG_stbc_htltf2].u_bool;
    #endif
    if (!any_ltf) {
        mp_raise_ValueError(MP_ERROR_TEXT("at least one training field is needed"));
    }

    uint8_t source[6];
    bool filtered = args[ARG_source].u_obj != mp_const_none;
    if (filtered) {
        mp_buffer_info_t source_buf;
        mp_get_buffer_raise(args[ARG_source].u_obj, &source_buf, MP_BUFFER_READ);
        if (source_buf.len != 6) {
            mp_raise_ValueError(MP_ERROR_TEXT("source must be 6 bytes"));
        }
        memcpy(source, source_buf.buf, 6);
    }

    // Scaling is spelled differently on Wi-Fi 6 parts: a manual-scale flag plus
    // a 0-15 shift before, one field afterwards, four bits wide on MAC version 3
    // and two before it. Validated here, ahead of the allocation below.
    #if defined(CONFIG_SOC_WIFI_HE_SUPPORT)
    #if defined(CONFIG_SOC_WIFI_MAC_VERSION_NUM) && CONFIG_SOC_WIFI_MAC_VERSION_NUM >= 3
    #define ESPIDF_CSI_SHIFT_MAX (8)
    #else
    #define ESPIDF_CSI_SHIFT_MAX (3)
    #endif
    #else
    #define ESPIDF_CSI_SHIFT_MAX (15)
    #endif
    bool manu_scale = args[ARG_shift].u_obj != mp_const_none;
    mp_int_t shift = 0;
    if (manu_scale) {
        shift = mp_arg_validate_int_range(mp_obj_get_int(args[ARG_shift].u_obj), 0,
            ESPIDF_CSI_SHIFT_MAX, MP_QSTR_shift);
    }

    espidf_csi_obj_t *self = mp_obj_malloc(espidf_csi_obj_t, &espidf_csi_type);
    // Outside the heap: the callback runs in the WiFi task and must not race a
    // collection, and the buffer is too big to want moving around anyway.
    self->records = heap_caps_calloc(queue, sizeof(espidf_csi_record_t), MALLOC_CAP_8BIT);
    if (self->records == NULL) {
        mp_raise_espidf_MemoryError();
    }
    self->size = queue;
    self->head = 0;
    self->tail = 0;
    self->lost = 0;
    self->filtered = filtered;
    if (filtered) {
        memcpy(self->source, source, 6);
    }
    self->running = true;
    espidf_csi_singleton = self;
    MP_STATE_VM(espidf_csi_active) = MP_OBJ_FROM_PTR(self);

    // Wi-Fi 6 parts describe acquisition with an entirely different structure:
    // wifi_csi_config_t is wifi_csi_acquire_config_t there, and of the fields
    // below only dump_ack_en exists in both. The Python arguments stay the same
    // so a script ports unchanged; the docstring says which of them the hardware
    // can act on.
    #if defined(CONFIG_SOC_WIFI_HE_SUPPORT)
    // The STBC selector is left at zero, which means the same thing on both MAC
    // versions -- take the complete first HE-LTF. Only its name differs
    // (acquire_csi_he_stbc against acquire_csi_he_stbc_mode), so not naming it
    // keeps one branch instead of two.
    wifi_csi_config_t csi_config = {
        .enable = 1,
        .acquire_csi_legacy = args[ARG_lltf].u_bool,
        .acquire_csi_ht20 = args[ARG_htltf].u_bool,
        .acquire_csi_ht40 = args[ARG_htltf].u_bool,
        .acquire_csi_su = args[ARG_he_su].u_bool,
        .acquire_csi_mu = args[ARG_he_mu].u_bool,
        .acquire_csi_dcm = args[ARG_he_dcm].u_bool,
        .acquire_csi_beamformed = args[ARG_he_beamformed].u_bool,
        .val_scale_cfg = (uint32_t)shift,
        .dump_ack_en = args[ARG_dump_ack].u_bool,
    };
    #else
    wifi_csi_config_t csi_config = {
        .lltf_en = args[ARG_lltf].u_bool,
        .htltf_en = args[ARG_htltf].u_bool,
        .stbc_htltf2_en = args[ARG_stbc_htltf2].u_bool,
        .ltf_merge_en = args[ARG_ltf_merge].u_bool,
        .channel_filter_en = args[ARG_channel_filter].u_bool,
        .manu_scale = manu_scale,
        .shift = (uint8_t)shift,
        .dump_ack_en = args[ARG_dump_ack].u_bool,
    };
    #endif
    esp_err_t err = esp_wifi_set_csi_config(&csi_config);
    if (err == ESP_OK) {
        err = esp_wifi_set_csi_rx_cb(espidf_csi_cb, NULL);
    }
    if (err == ESP_OK) {
        err = esp_wifi_set_csi(true);
    }
    if (err != ESP_OK) {
        espidf_csi_singleton = NULL;
        MP_STATE_VM(espidf_csi_active) = MP_OBJ_NULL;
        self->running = false;
        free(self->records);
        self->records = NULL;
        // The usual cause, and otherwise a bare "WiFi error 0x3001".
        if (err == ESP_ERR_WIFI_NOT_INIT || err == ESP_ERR_WIFI_NOT_STARTED) {
            mp_raise_RuntimeError(MP_ERROR_TEXT("CSI needs Wi-Fi running: set wifi.radio.enabled = True first"));
        }
        CHECK_ESP_RESULT(err);
    }
    return MP_OBJ_FROM_PTR(self);
}

//|     def queued(self) -> int:
//|         """How many records are waiting."""
//|         ...
static mp_obj_t espidf_csi_queued(mp_obj_t self_in) {
    espidf_csi_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->records == NULL) {
        return MP_OBJ_NEW_SMALL_INT(0);
    }
    size_t head = self->head, tail = self->tail;
    return MP_OBJ_NEW_SMALL_INT((head + self->size - tail) % self->size);
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_csi_queued_obj, espidf_csi_queued);

//|     def lost(self) -> int:
//|         """How many records were dropped because the queue was full."""
//|         ...
static mp_obj_t espidf_csi_lost(mp_obj_t self_in) {
    espidf_csi_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_int_from_uint(self->lost);
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_csi_lost_obj, espidf_csi_lost);

//|     def packet(self) -> dict | None:
//|         """Take the oldest record, or None when there is nothing.
//|
//|         The keys are ``rssi``, ``noise_floor``, ``channel``, ``sig_mode``,
//|         ``rx_state``, ``rx_seq``, ``cwb``, ``first_word_invalid``, ``mac``,
//|         ``timestamp`` and ``data``, the last one being the signed
//|         per-subcarrier values as ``[imaginary, real]`` pairs.
//|
//|         ``sig_mode`` says what the frame was and so how ``data`` is laid out,
//|         but the radio reports it on two different scales and they do not
//|         agree. On parts without Wi-Fi 6 it is the ``sig_mode`` field: 0 for
//|         non-HT, 1 for HT, 3 for VHT. On Wi-Fi 6 parts it is instead
//|         ``cur_bb_format``: 0 for 11b, 1 for 11g or 11a, 2 for HT, 3 for VHT,
//|         4 for HE SU, 5 for HE MU, 6 for HE ER SU, 7 for HE TB, 11 for VHT MU.
//|         Note that 3 means VHT on both but 1 does not mean the same thing, so
//|         code that has to run on either has to know which part it is on.
//|
//|         ``rx_state`` is nonzero for a frame the radio received with errors,
//|         which is worth discarding. ``first_word_invalid`` means the leading
//|         four bytes of ``data`` are not real measurements. ``cwb`` is 0 for a
//|         20 MHz frame and 1 for 40 MHz; on Wi-Fi 6 parts, which carry no such
//|         flag, it is derived from the secondary channel being set.
//|
//|         This allocates a dict and a bytes on every call. `readinto()` is the
//|         one to use when frames are arriving quickly."""
//|         ...
static mp_obj_t espidf_csi_packet(mp_obj_t self_in) {
    espidf_csi_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->records == NULL || self->head == self->tail) {
        return mp_const_none;
    }
    espidf_csi_record_t *rec = &self->records[self->tail];
    mp_obj_t result = mp_obj_new_dict(11);
    mp_obj_dict_store(result, MP_ROM_QSTR(MP_QSTR_rssi), MP_OBJ_NEW_SMALL_INT(rec->rssi));
    mp_obj_dict_store(result, MP_ROM_QSTR(MP_QSTR_noise_floor), MP_OBJ_NEW_SMALL_INT(rec->noise_floor));
    mp_obj_dict_store(result, MP_ROM_QSTR(MP_QSTR_channel), MP_OBJ_NEW_SMALL_INT(rec->channel));
    mp_obj_dict_store(result, MP_ROM_QSTR(MP_QSTR_sig_mode), MP_OBJ_NEW_SMALL_INT(rec->sig_mode));
    mp_obj_dict_store(result, MP_ROM_QSTR(MP_QSTR_rx_state), MP_OBJ_NEW_SMALL_INT(rec->rx_state));
    mp_obj_dict_store(result, MP_ROM_QSTR(MP_QSTR_rx_seq), MP_OBJ_NEW_SMALL_INT(rec->rx_seq));
    mp_obj_dict_store(result, MP_ROM_QSTR(MP_QSTR_cwb), MP_OBJ_NEW_SMALL_INT(rec->cwb));
    mp_obj_dict_store(result, MP_ROM_QSTR(MP_QSTR_first_word_invalid),
        mp_obj_new_bool(rec->first_word_invalid));
    mp_obj_dict_store(result, MP_ROM_QSTR(MP_QSTR_mac), mp_obj_new_bytes(rec->mac, 6));
    mp_obj_dict_store(result, MP_ROM_QSTR(MP_QSTR_timestamp), mp_obj_new_int_from_ll(rec->timestamp));
    mp_obj_dict_store(result, MP_ROM_QSTR(MP_QSTR_data),
        mp_obj_new_bytes((const byte *)rec->data, rec->length));
    self->tail = (self->tail + 1) % self->size;
    return result;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_csi_packet_obj, espidf_csi_packet);

//|     def readinto(
//|         self, data: WriteableBuffer, meta: WriteableBuffer | None = None
//|     ) -> int | None:
//|         """Copy the oldest record into ``data`` and return how many bytes were
//|         written, or None when the queue is empty. Nothing is allocated, so this
//|         keeps up where `packet()` starts feeding the collector.
//|
//|         Only as much as fits is copied. A return value equal to ``len(data)``
//|         means the record may have been longer.
//|
//|         ``meta``, if given, must hold at least sixteen 32-bit ints, and an
//|         ``array('i')`` reads back directly:
//|
//|         ==========  ====================================================
//|         index       value
//|         ==========  ====================================================
//|         0           rssi
//|         1           noise_floor
//|         2           channel
//|         3           sig_mode
//|         4           rx_state
//|         5           rx_seq
//|         6           cwb, 0 for 20 MHz and 1 for 40 MHz
//|         7           first_word_invalid
//|         8           timestamp, low 32 bits
//|         9           timestamp, high 32 bits
//|         10 to 15    the six bytes of the source MAC
//|         ==========  ====================================================
//|
//|         The timestamp reassembles as ``(meta[9] << 32) | (meta[8] &
//|         0xFFFFFFFF)`` in microseconds."""
//|         ...
static mp_obj_t espidf_csi_readinto(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_data, ARG_meta };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_data, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_meta, MP_ARG_OBJ, { .u_obj = mp_const_none } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_buffer_info_t data;
    mp_get_buffer_raise(args[ARG_data].u_obj, &data, MP_BUFFER_WRITE);
    mp_buffer_info_t meta;
    bool has_meta = espidf_csi_check_meta(args[ARG_meta].u_obj, &meta);

    espidf_csi_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    if (self->records == NULL || self->head == self->tail) {
        return mp_const_none;
    }
    espidf_csi_record_t *rec = &self->records[self->tail];
    size_t len = rec->length;
    if (len > data.len) {
        len = data.len;
    }
    memcpy(data.buf, rec->data, len);
    if (has_meta) {
        espidf_csi_store_meta(&meta, rec);
    }
    self->tail = (self->tail + 1) % self->size;
    return MP_OBJ_NEW_SMALL_INT(len);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(espidf_csi_readinto_obj, 2, espidf_csi_readinto);

//|     def readinto_amplitude(
//|         self,
//|         out: WriteableBuffer,
//|         meta: WriteableBuffer | None = None,
//|         skip: int = 0,
//|     ) -> int | None:
//|         """Take the oldest record, turn each ``[imaginary, real]`` pair into a
//|         magnitude, and write those into ``out`` as native floats. Returns how
//|         many were written, or None when the queue is empty.
//|
//|         ``out`` is meant to be a ``ulab`` array of the default float type, whose
//|         buffer this fills in place, leaving it ready for ``numpy`` without a
//|         conversion step. The alternative is sixty-odd Python level float
//|         operations per frame, which is what actually costs the time.
//|
//|         ``skip`` drops that many leading bytes of the record. Pass 4 when
//|         ``first_word_invalid`` is set, since that covers two pairs.
//|
//|         ``meta`` is filled exactly as in `readinto()`."""
//|         ...
static mp_obj_t espidf_csi_readinto_amplitude(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_out, ARG_meta, ARG_skip };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_out, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_meta, MP_ARG_OBJ, { .u_obj = mp_const_none } },
        { MP_QSTR_skip, MP_ARG_INT, { .u_int = 0 } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_int_t skip = mp_arg_validate_int_range(args[ARG_skip].u_int, 0, ESPIDF_CSI_MAX_BYTES,
        MP_QSTR_skip);

    mp_buffer_info_t out;
    mp_get_buffer_raise(args[ARG_out].u_obj, &out, MP_BUFFER_WRITE);
    // A memoryview can start part way into its base, and Xtensa wants floats
    // aligned, so refuse rather than fault.
    if (((uintptr_t)out.buf % sizeof(mp_float_t)) != 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("out must be word aligned"));
    }
    mp_buffer_info_t meta;
    bool has_meta = espidf_csi_check_meta(args[ARG_meta].u_obj, &meta);

    espidf_csi_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    if (self->records == NULL || self->head == self->tail) {
        return mp_const_none;
    }
    espidf_csi_record_t *rec = &self->records[self->tail];
    size_t pairs = rec->length > (size_t)skip ? (rec->length - skip) / 2 : 0;
    if (pairs > out.len / sizeof(mp_float_t)) {
        pairs = out.len / sizeof(mp_float_t);
    }
    const int8_t *iq = rec->data + skip;
    mp_float_t *dest = (mp_float_t *)out.buf;
    for (size_t i = 0; i < pairs; i++) {
        mp_float_t im = iq[2 * i];
        mp_float_t re = iq[2 * i + 1];
        dest[i] = MICROPY_FLOAT_C_FUN(sqrt)(im * im + re * re);
    }
    if (has_meta) {
        espidf_csi_store_meta(&meta, rec);
    }
    self->tail = (self->tail + 1) % self->size;
    return MP_OBJ_NEW_SMALL_INT(pairs);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(espidf_csi_readinto_amplitude_obj, 2, espidf_csi_readinto_amplitude);

//|     def deinit(self) -> None:
//|         """Stop collecting and release the queue."""
//|         ...
static mp_obj_t espidf_csi_deinit(mp_obj_t self_in) {
    espidf_csi_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->running) {
        self->running = false;
        esp_wifi_set_csi(false);
        esp_wifi_set_csi_rx_cb(NULL, NULL);
    }
    espidf_csi_singleton = NULL;
    MP_STATE_VM(espidf_csi_active) = MP_OBJ_NULL;
    if (self->records != NULL) {
        free(self->records);
        self->records = NULL;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_csi_deinit_obj, espidf_csi_deinit);

// Called on soft reset: the object is about to go away, but the radio would keep
// calling into it.
void espidf_csi_reset(void) {
    if (espidf_csi_singleton != NULL) {
        espidf_csi_singleton->running = false;
        esp_wifi_set_csi(false);
        esp_wifi_set_csi_rx_cb(NULL, NULL);
        free(espidf_csi_singleton->records);
        espidf_csi_singleton->records = NULL;
        espidf_csi_singleton = NULL;
    }
    MP_STATE_VM(espidf_csi_active) = MP_OBJ_NULL;
}

static const mp_rom_map_elem_t espidf_csi_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_queued), MP_ROM_PTR(&espidf_csi_queued_obj) },
    { MP_ROM_QSTR(MP_QSTR_lost), MP_ROM_PTR(&espidf_csi_lost_obj) },
    { MP_ROM_QSTR(MP_QSTR_packet), MP_ROM_PTR(&espidf_csi_packet_obj) },
    { MP_ROM_QSTR(MP_QSTR_readinto), MP_ROM_PTR(&espidf_csi_readinto_obj) },
    { MP_ROM_QSTR(MP_QSTR_readinto_amplitude), MP_ROM_PTR(&espidf_csi_readinto_amplitude_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&espidf_csi_deinit_obj) },
};
static MP_DEFINE_CONST_DICT(espidf_csi_locals_dict, espidf_csi_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    espidf_csi_type,
    MP_QSTR_CSI,
    MP_TYPE_FLAG_NONE,
    make_new, espidf_csi_make_new,
    locals_dict, &espidf_csi_locals_dict
    );

#endif // CIRCUITPY_ESPIDF_CSI

