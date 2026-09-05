// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Peter Vavrin
//
// SPDX-License-Identifier: MIT

#include <string.h>

#include "py/mphal.h"
#include "py/objproperty.h"
#include "py/runtime.h"
#include "py/mperrno.h"

#include "shared-bindings/util.h"
#include "shared/runtime/context_manager_helpers.h"
#include "shared/runtime/interrupt_char.h"
#include "supervisor/shared/tick.h"

#include "bindings/ieee802154/Radio.h"

#include "esp_err.h"
#include "esp_ieee802154.h"
#include "esp_mac.h"

// Defined in esp_ieee802154.c and exported, but missing from the component's
// public header, so declare them here rather than reach into private_include.
extern bool esp_ieee802154_get_auto_ack_rx(void);
extern esp_err_t esp_ieee802154_set_auto_ack_rx(bool enable);
extern esp_err_t esp_ieee802154_set_auto_ack_tx(bool enable);

typedef struct {
    uint64_t timestamp;
    uint8_t len;
    int8_t rssi;
    uint8_t lqi;
    uint8_t channel;
    bool pending;
} ieee802154_rx_meta_t;

// Metadata layout for readinto(), which hands back a fixed 16-byte record so a
// collector never has to allocate. Keep in step with the docstring.
#define META_SIZE (16)

enum tx_state {
    TX_IDLE = 0,
    TX_RUNNING,
    TX_DONE,
    TX_FAILED,
};

typedef struct {
    mp_obj_base_t base;
    uint8_t *frames;
    ieee802154_rx_meta_t *meta;
    uint8_t size;
    volatile uint8_t head;
    volatile uint8_t tail;
    volatile uint32_t lost;
    volatile uint8_t tx_state;
    volatile uint8_t tx_error;
    volatile uint8_t ack_len;
    volatile bool ed_done;
    volatile int8_t ed_power;
    uint8_t pending_mode;
    uint8_t ack[IEEE802154_FRAME_SLOT];
} ieee802154_radio_obj_t;

// The radio is a single piece of hardware and the driver's completion callbacks
// are plain global functions with nowhere to hang a context pointer, so the
// object has to be reachable from a static. Cleared on deinit, and every
// callback tolerates it being NULL.
static ieee802154_radio_obj_t *_radio = NULL;

// esp_ieee802154_enable() ends in ieee802154_mac_init(), which allocates the
// interrupt and registers a sleep callback. esp_ieee802154_disable() does not
// undo that cleanly: a second enable returns ESP_FAIL (0xffffffff), so a script
// that creates a Radio, drops it and creates another one -- which is every
// second run under CircuitPython -- would fail. The driver is therefore enabled
// once and left enabled; releasing a Radio only puts the radio to sleep.
static bool _driver_enabled = false;

// --- driver callbacks, called from the driver's ISR context ------------------
//
// Registered through esp_ieee802154_event_callback_list_register() rather than
// by overriding the driver's weak esp_ieee802154_*_done symbols. The Zigbee
// stack defines those same symbols -- strongly, from its prebuilt library -- so
// defining them here means the two modules cannot be in one firmware at all.
// A registered list takes precedence over them, which turns a link-time
// collision into the runtime arbitration this actually wants: whoever holds the
// radio gets the frames, and the Zigbee stack gets them when nobody does.
//
// They must not allocate, must not raise, and must not call into the VM: all
// they do is copy into the ring buffer and hand the driver's buffer straight
// back.

// Counted before anything else, and outside the object, so that it separates
// "the driver never calls us" from "it calls us and the frame is lost in here".
static volatile uint32_t _rx_callbacks;

// How much of a frame the driver hands up is actually the caller's: everything
// except the leading length byte and the trailing two bytes, which held the
// checksum on the air and which the hardware has overwritten with the RSSI and
// LQI it reports through frame_info anyway. Clamped because the length byte
// comes off the air and nothing above this has range-checked it yet.
static size_t strip_phy(const uint8_t *frame) {
    size_t psdu = frame[0];
    if (psdu > IEEE802154_FRAME_SLOT - 1) {
        psdu = IEEE802154_FRAME_SLOT - 1;
    }
    return psdu > 2 ? psdu - 2 : 0;
}

// esp_ieee802154_transmit() does not copy: it keeps the caller's pointer and
// programs it as the radio's DMA source. The frame arrives here as a Python
// buffer, and on a board with PSRAM the Python heap lives there -- out of reach
// of DMA. Transmitting straight from it puts whatever the MAC happens to fetch
// on the air, with a checksum to match, so every receiver drops the frame at
// the CRC check while the sender still sees a successful transmission. Bounce
// through internal RAM instead. Safe as a single static because send() blocks
// until the radio is done with it.
static uint8_t _tx_buf[IEEE802154_FRAME_SLOT] __attribute__((aligned(4)));

// The registered callback list takes precedence over the driver's weak
// symbols, and once registered it stays registered -- the driver refuses to
// change it after its MAC is up. So when no Radio holds the radio, these have
// to hand the frame on to whoever the weak symbols belong to, which is the
// Zigbee stack. Swallowing it instead leaves Zigbee with a radio that receives
// nothing, for the rest of the boot, with no error anywhere.
static void rx_done_cb(uint8_t *frame, esp_ieee802154_frame_info_t *frame_info) {
    _rx_callbacks++;
    ieee802154_radio_obj_t *self = _radio;
    #if CIRCUITPY_ZIGBEE
    if (self == NULL) {
        esp_ieee802154_receive_done(frame, frame_info);
        return;
    }
    #endif
    if (self != NULL) {
        uint8_t next = (self->head + 1) % self->size;
        if (next == self->tail) {
            // Full. Drop the newest rather than the oldest: a collector that has
            // fallen behind is better served by a contiguous older run.
            self->lost++;
        } else {
            size_t len = strip_phy(frame);
            memcpy(self->frames + (size_t)self->head * IEEE802154_FRAME_SLOT, frame + 1, len);
            ieee802154_rx_meta_t *m = &self->meta[self->head];
            m->len = (uint8_t)len;
            m->rssi = frame_info->rssi;
            m->lqi = frame_info->lqi;
            m->channel = frame_info->channel;
            m->pending = frame_info->pending;
            m->timestamp = frame_info->timestamp;
            self->head = next;
        }
    }
    esp_ieee802154_receive_handle_done(frame);
}

static void tx_done_cb(const uint8_t *frame, const uint8_t *ack,
    esp_ieee802154_frame_info_t *ack_frame_info) {
    ieee802154_radio_obj_t *self = _radio;
    #if CIRCUITPY_ZIGBEE
    if (self == NULL) {
        esp_ieee802154_transmit_done(frame, ack, ack_frame_info);
        return;
    }
    #endif
    if (self != NULL) {
        self->ack_len = 0;
    }
    if (ack != NULL) {
        if (self != NULL) {
            size_t len = strip_phy(ack);
            memcpy(self->ack, ack + 1, len);
            self->ack_len = (uint8_t)len;
        }
        esp_ieee802154_receive_handle_done(ack);
    }
    if (self != NULL) {
        self->tx_state = TX_DONE;
    }
}

static void tx_failed_cb(const uint8_t *frame, esp_ieee802154_tx_error_t error) {
    ieee802154_radio_obj_t *self = _radio;
    #if CIRCUITPY_ZIGBEE
    if (self == NULL) {
        esp_ieee802154_transmit_failed(frame, error);
        return;
    }
    #endif
    if (self != NULL) {
        self->tx_error = (uint8_t)error;
        self->tx_state = TX_FAILED;
    }
}

static void ed_done_cb(int8_t power) {
    ieee802154_radio_obj_t *self = _radio;
    #if CIRCUITPY_ZIGBEE
    if (self == NULL) {
        esp_ieee802154_energy_detect_done(power);
        return;
    }
    #endif
    if (self != NULL) {
        self->ed_power = power;
        self->ed_done = true;
    }
}

static const esp_ieee802154_event_cb_list_t _callbacks = {
    .rx_done_cb = rx_done_cb,
    .tx_done_cb = tx_done_cb,
    .tx_failed_cb = tx_failed_cb,
    .ed_done_cb = ed_done_cb,
};

// --- helpers -----------------------------------------------------------------

// Called on soft reset. Without this a script that raises before its deinit()
// leaves the radio claimed, and every later run fails with "already in use"
// until the board is power cycled.
void ieee802154_reset(void) {
    if (_radio != NULL) {
        _radio = NULL;
        esp_ieee802154_sleep();
    }
}

static void check_for_deinit(ieee802154_radio_obj_t *self) {
    if (self->frames == NULL) {
        raise_deinited_error();
    }
}

static void check_esp_err(esp_err_t err) {
    if (err != ESP_OK) {
        // Name the failure: this layer sits on a driver that shares one radio
        // with Wi-Fi and BLE, and "input/output error" hides which of the many
        // ways that can go wrong actually happened.
        // The numeric code too: several of the errors this layer can hit are not
        // in esp_err_to_name's table and come out as "UNKNOWN ERROR", which says
        // nothing about which call failed or why.
        mp_raise_msg_varg(&mp_type_RuntimeError, MP_ERROR_TEXT("%s (0x%x)"),
            esp_err_to_name(err), (unsigned int)err);
    }
}

//| class Radio:
//|     """The raw IEEE 802.15.4 radio.
//|
//|     A frame is the MAC header followed by the MAC payload, and nothing else,
//|     in both directions. The length byte and the checksum belong to the radio:
//|     it writes them on transmit and takes them off on receive, and neither
//|     appears in what Python passes in or gets back. Anything the receiver has
//|     to be told, such as how long the payload is, goes in the payload.
//|
//|     The checksum the hardware verified is not returned, because it is not
//|     there any more -- the hardware overwrites it with the signal strength and
//|     link quality, which `receive` reports as ``rssi`` and ``lqi``.
//|
//|     Building and parsing the MAC header is left to Python. That is deliberate:
//|     it can be changed without reflashing, and anything built on top of
//|     802.15.4 needs full control of the header anyway.
//|
//|     Only one radio can exist at a time."""
//|
//|     def __init__(
//|         self,
//|         *,
//|         channel: int = 15,
//|         pan_id: int = 0x1234,
//|         short_address: int = 0xFFFF,
//|         extended_address: ReadableBuffer | None = None,
//|         queue: int = 8,
//|     ) -> None:
//|         """Bring the radio up and start receiving.
//|
//|         :param int channel: 11 to 26. Both ends must agree.
//|         :param int pan_id: the network both ends belong to.
//|         :param int short_address: this node's 16-bit address, 0xFFFF for none.
//|         :param ReadableBuffer extended_address: this node's 8-byte address.
//|           Defaults to the one in the chip's efuse.
//|         :param int queue: how many received frames to hold. Frames arriving
//|           with the queue full are counted in `lost` and dropped."""
//|         ...
static mp_obj_t ieee802154_radio_make_new(const mp_obj_type_t *type, size_t n_args,
    size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_channel, ARG_pan_id, ARG_short_address, ARG_extended_address, ARG_queue };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_channel, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 15 } },
        { MP_QSTR_pan_id, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 0x1234 } },
        { MP_QSTR_short_address, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 0xFFFF } },
        { MP_QSTR_extended_address, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = mp_const_none } },
        { MP_QSTR_queue, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 8 } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    if (_radio != NULL) {
        mp_raise_ValueError(MP_ERROR_TEXT("Radio is already in use"));
    }

    mp_int_t channel = mp_arg_validate_int_range(args[ARG_channel].u_int, 11, 26, MP_QSTR_channel);
    mp_int_t pan_id = mp_arg_validate_int_range(args[ARG_pan_id].u_int, 0, 0xFFFF, MP_QSTR_pan_id);
    mp_int_t short_address =
        mp_arg_validate_int_range(args[ARG_short_address].u_int, 0, 0xFFFF, MP_QSTR_short_address);
    // Two slots is the smallest ring that can hold anything, since one is always
    // left empty to tell full from empty.
    mp_int_t queue = mp_arg_validate_int_range(args[ARG_queue].u_int, 2, 64, MP_QSTR_queue);

    uint8_t ext_addr[8];
    if (args[ARG_extended_address].u_obj == mp_const_none) {
        check_esp_err(esp_read_mac(ext_addr, ESP_MAC_IEEE802154));
    } else {
        mp_buffer_info_t buf;
        mp_get_buffer_raise(args[ARG_extended_address].u_obj, &buf, MP_BUFFER_READ);
        if (buf.len != sizeof(ext_addr)) {
            mp_raise_ValueError(MP_ERROR_TEXT("extended_address must be 8 bytes"));
        }
        memcpy(ext_addr, buf.buf, sizeof(ext_addr));
    }

    ieee802154_radio_obj_t *self = mp_obj_malloc(ieee802154_radio_obj_t, &ieee802154_radio_type);
    self->size = (uint8_t)queue;
    self->head = 0;
    self->tail = 0;
    self->lost = 0;
    self->tx_state = TX_IDLE;
    self->ack_len = 0;
    self->pending_mode = 0;
    self->frames = m_malloc((size_t)queue * IEEE802154_FRAME_SLOT);
    self->meta = m_malloc((size_t)queue * sizeof(ieee802154_rx_meta_t));

    if (!_driver_enabled) {
        // Has to happen before the first enable: the driver refuses to change
        // the list once its MAC is initialised, and it is never de-initialised
        // here -- see the note on _driver_enabled above.
        check_esp_err(esp_ieee802154_event_callback_list_register(_callbacks));
        check_esp_err(esp_ieee802154_enable());
        _driver_enabled = true;
    }
    // Published before the first frame can arrive, and the driver only starts
    // delivering at esp_ieee802154_receive() below.
    _radio = self;

    // Wi-Fi, BLE and 802.15.4 share one 2.4 GHz front end, and the arbiter
    // breaks off 802.15.4 transmissions at its default priority: every send
    // comes back as ESP_IEEE802154_TX_ERR_COEXIST. Ask for high priority while
    // actually sending or receiving, and stay out of the way when idle.
    esp_ieee802154_coex_config_t coex = {
        .idle = IEEE802154_IDLE,
        .txrx = IEEE802154_HIGH,
        .txrx_at = IEEE802154_HIGH,
    };
    esp_ieee802154_set_coex_config(coex);

    // The driver comes up promiscuous, which means no address filtering at all.
    // Set it explicitly so the documented defaults are the real ones; this also
    // turns automatic acknowledgement on, which is what set_promiscuous(false)
    // does in the driver.
    esp_ieee802154_set_promiscuous(false);
    esp_ieee802154_set_channel((uint8_t)channel);
    esp_ieee802154_set_panid((uint16_t)pan_id);
    esp_ieee802154_set_short_address((uint16_t)short_address);
    esp_ieee802154_set_extended_address(ext_addr);
    // Go back to listening whenever the radio is otherwise idle, so a node does
    // not miss the reply to something it just sent.
    esp_ieee802154_set_rx_when_idle(true);
    check_esp_err(esp_ieee802154_receive());

    return MP_OBJ_FROM_PTR(self);
}

//|     def deinit(self) -> None:
//|         """Turn the radio off and release the receive queue."""
//|         ...
static mp_obj_t ieee802154_radio_deinit(mp_obj_t self_in) {
    ieee802154_radio_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->frames != NULL) {
        // Unhook first: the callbacks must not find a half-freed queue.
        _radio = NULL;
        esp_ieee802154_sleep();
        m_free(self->frames);
        m_free(self->meta);
        self->frames = NULL;
        self->meta = NULL;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ieee802154_radio_deinit_obj, ieee802154_radio_deinit);

static mp_obj_t ieee802154_radio_obj___exit__(size_t n_args, const mp_obj_t *args) {
    return ieee802154_radio_deinit(args[0]);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ieee802154_radio___exit___obj, 4, 4,
    ieee802154_radio_obj___exit__);

//|     def send(self, frame: ReadableBuffer, *, cca: bool = True, timeout: float = 0.1) -> bytes | None:
//|         """Transmit one frame and wait for the result.
//|
//|         ``frame`` is the MAC header followed by the payload, 3 to 125 bytes.
//|         The length byte and the checksum are added by the radio. If the header
//|         asks for an acknowledgement, the acknowledgement frame is returned in
//|         the same form, otherwise None.
//|
//|         :param bool cca: listen before transmitting and give up if the channel
//|           is busy.
//|         :param float timeout: seconds to wait for the radio to finish.
//|
//|         Raises `RuntimeError` if the transmission fails, with the reason in the
//|         message, and `OSError` on timeout."""
//|         ...
static mp_obj_t ieee802154_radio_send(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_frame, ARG_cca, ARG_timeout };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_frame, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_cca, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = true } },
        { MP_QSTR_timeout, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = MP_ROM_NONE } },
    };
    ieee802154_radio_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    check_for_deinit(self);

    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_buffer_info_t buf;
    mp_get_buffer_raise(args[ARG_frame].u_obj, &buf, MP_BUFFER_READ);
    if (buf.len < IEEE802154_MIN_FRAME || buf.len > IEEE802154_MAX_FRAME) {
        mp_raise_ValueError_varg(MP_ERROR_TEXT("frame must be %d to %d bytes"),
            IEEE802154_MIN_FRAME, IEEE802154_MAX_FRAME);
    }

    mp_float_t timeout = 0.1f;
    if (args[ARG_timeout].u_obj != mp_const_none) {
        timeout = mp_obj_get_float(args[ARG_timeout].u_obj);
    }

    // The radio wants the PHY length byte in front and the two checksum bytes
    // after, and it reads exactly length-byte-many bytes out of the buffer. The
    // checksum it computes itself; the room for it still has to be there, or it
    // reads past the frame and puts whatever it finds on the air -- which
    // transmits and reports success while every receiver drops the frame.
    _tx_buf[0] = (uint8_t)(buf.len + 2);
    memcpy(_tx_buf + 1, buf.buf, buf.len);
    _tx_buf[buf.len + 1] = 0;
    _tx_buf[buf.len + 2] = 0;

    self->tx_state = TX_RUNNING;
    self->ack_len = 0;
    esp_err_t err = esp_ieee802154_transmit(_tx_buf, args[ARG_cca].u_bool);
    if (err != ESP_OK) {
        self->tx_state = TX_IDLE;
        check_esp_err(err);
    }

    uint64_t deadline = supervisor_ticks_ms64() + (uint64_t)(timeout * 1000.0f);
    while (self->tx_state == TX_RUNNING) {
        RUN_BACKGROUND_TASKS;
        if (mp_hal_is_interrupted()) {
            self->tx_state = TX_IDLE;
            return mp_const_none;
        }
        if (supervisor_ticks_ms64() > deadline) {
            self->tx_state = TX_IDLE;
            mp_raise_OSError(MP_ETIMEDOUT);
        }
    }

    if (self->tx_state == TX_FAILED) {
        self->tx_state = TX_IDLE;
        // The reason matters to the caller: a busy channel is worth retrying,
        // a missing acknowledgement usually means the peer is not there.
        const char *reason;
        switch (self->tx_error) {
            case ESP_IEEE802154_TX_ERR_CCA_BUSY:
                reason = "channel busy";
                break;
            case ESP_IEEE802154_TX_ERR_NO_ACK:
                reason = "no acknowledgement";
                break;
            case ESP_IEEE802154_TX_ERR_INVALID_ACK:
                reason = "invalid acknowledgement";
                break;
            case ESP_IEEE802154_TX_ERR_ABORT:
                reason = "aborted";
                break;
            case ESP_IEEE802154_TX_ERR_COEXIST:
                reason = "rejected by coexistence";
                break;
            case ESP_IEEE802154_TX_ERR_SECURITY:
                reason = "security configuration";
                break;
            default:
                reason = "failed";
                break;
        }
        mp_raise_msg_varg(&mp_type_RuntimeError, MP_ERROR_TEXT("Transmit %s"), reason);
    }

    self->tx_state = TX_IDLE;
    if (self->ack_len == 0) {
        return mp_const_none;
    }
    return mp_obj_new_bytes(self->ack, self->ack_len);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(ieee802154_radio_send_obj, 2, ieee802154_radio_send);

//|     def receive(self) -> dict | None:
//|         """Take the oldest frame, or None when nothing is waiting.
//|
//|         The keys are ``data`` (the MAC header and payload), ``rssi``, ``lqi``,
//|         ``channel``, ``pending`` and ``timestamp``."""
//|         ...
static mp_obj_t ieee802154_radio_receive(mp_obj_t self_in) {
    ieee802154_radio_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    if (self->head == self->tail) {
        return mp_const_none;
    }

    ieee802154_rx_meta_t *m = &self->meta[self->tail];
    const uint8_t *frame = self->frames + (size_t)self->tail * IEEE802154_FRAME_SLOT;

    mp_obj_t result = mp_obj_new_dict(6);
    mp_obj_dict_store(result, MP_ROM_QSTR(MP_QSTR_data), mp_obj_new_bytes(frame, m->len));
    mp_obj_dict_store(result, MP_ROM_QSTR(MP_QSTR_rssi), MP_OBJ_NEW_SMALL_INT(m->rssi));
    mp_obj_dict_store(result, MP_ROM_QSTR(MP_QSTR_lqi), MP_OBJ_NEW_SMALL_INT(m->lqi));
    mp_obj_dict_store(result, MP_ROM_QSTR(MP_QSTR_channel), MP_OBJ_NEW_SMALL_INT(m->channel));
    mp_obj_dict_store(result, MP_ROM_QSTR(MP_QSTR_pending), mp_obj_new_bool(m->pending));
    mp_obj_dict_store(result, MP_ROM_QSTR(MP_QSTR_timestamp), mp_obj_new_int_from_ull(m->timestamp));

    self->tail = (self->tail + 1) % self->size;
    return result;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ieee802154_radio_receive_obj, ieee802154_radio_receive);

//|     def readinto(self, data: WriteableBuffer, meta: WriteableBuffer | None = None) -> int | None:
//|         """Copy the oldest frame into ``data`` and return its length, or None
//|         when nothing is waiting. Nothing is allocated, so this keeps up where
//|         `receive` starts feeding the collector.
//|
//|         ``data`` must be at least 125 bytes and is filled with the MAC header
//|         and payload, exactly as `receive` would return them.
//|
//|         ``meta``, if given, must be 16 bytes and is filled with the length,
//|         rssi as a signed byte, lqi, channel, pending as 0 or 1, three unused
//|         bytes, and the 8-byte timestamp, little endian."""
//|         ...
static mp_obj_t ieee802154_radio_readinto(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_data, ARG_meta };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_data, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_meta, MP_ARG_OBJ, { .u_obj = MP_ROM_NONE } },
    };
    ieee802154_radio_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    check_for_deinit(self);

    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    // Validated before looking at the queue, so a wrong-sized buffer is a
    // reliable error rather than one that waits for a frame to show up.
    mp_buffer_info_t data;
    mp_get_buffer_raise(args[ARG_data].u_obj, &data, MP_BUFFER_WRITE);
    if (data.len < IEEE802154_MAX_FRAME) {
        mp_raise_ValueError_varg(MP_ERROR_TEXT("data must be at least %d bytes"),
            IEEE802154_MAX_FRAME);
    }
    mp_buffer_info_t meta;
    bool want_meta = args[ARG_meta].u_obj != mp_const_none;
    if (want_meta) {
        mp_get_buffer_raise(args[ARG_meta].u_obj, &meta, MP_BUFFER_WRITE);
        if (meta.len < META_SIZE) {
            mp_raise_ValueError(MP_ERROR_TEXT("meta must be at least 16 bytes"));
        }
    }

    if (self->head == self->tail) {
        return mp_const_none;
    }

    ieee802154_rx_meta_t *m = &self->meta[self->tail];
    memcpy(data.buf, self->frames + (size_t)self->tail * IEEE802154_FRAME_SLOT, m->len);
    if (want_meta) {
        uint8_t *out = meta.buf;
        out[0] = m->len;
        out[1] = (uint8_t)m->rssi;
        out[2] = m->lqi;
        out[3] = m->channel;
        out[4] = m->pending ? 1 : 0;
        out[5] = 0;
        out[6] = 0;
        out[7] = 0;
        uint64_t ts = m->timestamp;
        for (size_t i = 0; i < 8; i++) {
            out[8 + i] = (uint8_t)(ts >> (8 * i));
        }
    }

    self->tail = (self->tail + 1) % self->size;
    return MP_OBJ_NEW_SMALL_INT(m->len);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(ieee802154_radio_readinto_obj, 2, ieee802154_radio_readinto);

//|     def energy_detect(self, duration: float = 0.001) -> int:
//|         """Measure the energy on the current channel over ``duration`` seconds,
//|         0.000016 to 1.0, and return it in dBm. Useful for picking a quiet
//|         channel.
//|
//|         The radio is not listening while it measures."""
//|         ...
static mp_obj_t ieee802154_radio_energy_detect(size_t n_args, const mp_obj_t *args) {
    ieee802154_radio_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    check_for_deinit(self);
    mp_float_t duration = n_args > 1 ? mp_obj_get_float(args[1]) : (mp_float_t)0.001;
    // The upper bound is this layer's, not the hardware's: the wait below blocks
    // the VM, and a measurement long enough to look like a hang is not useful.
    if (duration < (mp_float_t)0.000016 || duration > (mp_float_t)1.0) {
        mp_raise_ValueError(MP_ERROR_TEXT("duration must be 0.000016 to 1.0 seconds"));
    }

    // The measurement is asynchronous: the driver reports it through
    // esp_ieee802154_energy_detect_done(), so wait for that rather than handing
    // the caller a None it has no way to interpret.
    self->ed_done = false;
    // The driver counts in symbol periods of 16 us, not in microseconds. Passing
    // microseconds measures for sixteen times as long as asked, which still
    // returns for a short request and times out against the wait below for
    // anything from about 7 ms up -- so it looks like the radio refusing rather
    // than like a unit error.
    check_esp_err(esp_ieee802154_energy_detect((uint32_t)(duration * 62500.0f)));

    uint64_t deadline = supervisor_ticks_ms64() + (uint64_t)(duration * 1000.0f) + 100;
    while (!self->ed_done) {
        RUN_BACKGROUND_TASKS;
        if (mp_hal_is_interrupted()) {
            return mp_const_none;
        }
        if (supervisor_ticks_ms64() > deadline) {
            mp_raise_OSError(MP_ETIMEDOUT);
        }
    }
    // Listening stops for the measurement, so put the radio back.
    esp_ieee802154_receive();
    return MP_OBJ_NEW_SMALL_INT(self->ed_power);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ieee802154_radio_energy_detect_obj, 1, 2,
    ieee802154_radio_energy_detect);

//|     def add_pending(self, address: ReadableBuffer) -> None:
//|         """Record that there is data waiting for ``address``, so the frame
//|         pending bit is set in acknowledgements sent to it. Two bytes for a
//|         short address, eight for an extended one."""
//|         ...
static mp_obj_t ieee802154_radio_add_pending(mp_obj_t self_in, mp_obj_t address_in) {
    ieee802154_radio_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    mp_buffer_info_t buf;
    mp_get_buffer_raise(address_in, &buf, MP_BUFFER_READ);
    if (buf.len != 2 && buf.len != 8) {
        mp_raise_ValueError(MP_ERROR_TEXT("address must be 2 or 8 bytes"));
    }
    check_esp_err(esp_ieee802154_add_pending_addr(buf.buf, buf.len == 2));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(ieee802154_radio_add_pending_obj, ieee802154_radio_add_pending);

//|     def clear_pending(self, address: ReadableBuffer) -> None:
//|         """Undo `add_pending` for one address."""
//|         ...
static mp_obj_t ieee802154_radio_clear_pending(mp_obj_t self_in, mp_obj_t address_in) {
    ieee802154_radio_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    mp_buffer_info_t buf;
    mp_get_buffer_raise(address_in, &buf, MP_BUFFER_READ);
    if (buf.len != 2 && buf.len != 8) {
        mp_raise_ValueError(MP_ERROR_TEXT("address must be 2 or 8 bytes"));
    }
    check_esp_err(esp_ieee802154_clear_pending_addr(buf.buf, buf.len == 2));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(ieee802154_radio_clear_pending_obj, ieee802154_radio_clear_pending);

//|     def reset_pending(self, short: bool = True) -> None:
//|         """Empty the pending address table."""
//|         ...
static mp_obj_t ieee802154_radio_reset_pending(size_t n_args, const mp_obj_t *args) {
    ieee802154_radio_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    check_for_deinit(self);
    bool is_short = n_args > 1 ? mp_obj_is_true(args[1]) : true;
    check_esp_err(esp_ieee802154_reset_pending_table(is_short));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ieee802154_radio_reset_pending_obj, 1, 2,
    ieee802154_radio_reset_pending);

// --- properties --------------------------------------------------------------

// Settings written while the radio is listening do not reach it. The driver's
// setters only mark the parameter block pending; it is applied by
// ieee802154_pib_update(), which runs inside receive and transmit. So a channel
// assigned mid-run does nothing at all until the next send, and a monitor that
// walks the channels stays on the one it started on -- silently, since every
// getter reports the value that was written.
//
// esp_ieee802154_receive() applies it and is cheap when there is nothing
// pending: it returns immediately if the radio is already receiving and the
// parameter block is clean. Only re-armed when the radio is actually listening,
// so this does not wake a released radio out of sleep.
static void apply_pib(void) {
    if (esp_ieee802154_get_state() == ESP_IEEE802154_RADIO_RECEIVE) {
        esp_ieee802154_receive();
    }
}

#define SIMPLE_GETSET(name, getter, setter, lo, hi, cast)                                  \
    static mp_obj_t ieee802154_radio_get_##name(mp_obj_t self_in) {                        \
        ieee802154_radio_obj_t *self = MP_OBJ_TO_PTR(self_in);                             \
        check_for_deinit(self);                                                            \
        return MP_OBJ_NEW_SMALL_INT(getter());                                             \
    }                                                                                      \
    static MP_DEFINE_CONST_FUN_OBJ_1(ieee802154_radio_get_##name##_obj,                    \
        ieee802154_radio_get_##name);                                                      \
    static mp_obj_t ieee802154_radio_set_##name(mp_obj_t self_in, mp_obj_t value) {        \
        ieee802154_radio_obj_t *self = MP_OBJ_TO_PTR(self_in);                             \
        check_for_deinit(self);                                                            \
        mp_int_t v = mp_arg_validate_int_range(mp_obj_get_int(value), lo, hi, MP_QSTR_##name); \
        check_esp_err(setter((cast)v));                                                    \
        apply_pib();                                                                       \
        return mp_const_none;                                                              \
    }                                                                                      \
    static MP_DEFINE_CONST_FUN_OBJ_2(ieee802154_radio_set_##name##_obj,                    \
        ieee802154_radio_set_##name);                                                      \
    MP_PROPERTY_GETSET(ieee802154_radio_##name##_obj,                                      \
        (mp_obj_t)&ieee802154_radio_get_##name##_obj,                                      \
        (mp_obj_t)&ieee802154_radio_set_##name##_obj)

//|     channel: int
//|     """The channel in use, 11 to 26."""
SIMPLE_GETSET(channel, esp_ieee802154_get_channel, esp_ieee802154_set_channel, 11, 26, uint8_t);

// The radio supports a fixed set of transmit powers, and the blob knows which.
// The driver stores whatever it is handed and only maps it onto that set on the
// way to the hardware, so without this the property would echo back requests
// the radio never honoured -- -40 dBm and 30 dBm both read back unchanged.
extern const int8_t *bt_bb_get_tx_pwr_table(uint8_t *length);

static const int8_t *tx_power_table(uint8_t *length) {
    const int8_t *table = bt_bb_get_tx_pwr_table(length);
    if (table == NULL || *length == 0) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("no transmit power table"));
    }
    return table;
}

//|     tx_power: int
//|     """Transmit power in dBm.
//|
//|     The radio has a fixed set of powers, listed in `tx_powers`. A value
//|     between two of them is rounded down to the nearer supported one, so
//|     reading this back tells you what the radio is actually doing rather than
//|     what was asked for. Outside the supported range it raises `ValueError`."""
static mp_obj_t ieee802154_radio_get_tx_power(mp_obj_t self_in) {
    ieee802154_radio_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    return MP_OBJ_NEW_SMALL_INT(esp_ieee802154_get_txpower());
}
static MP_DEFINE_CONST_FUN_OBJ_1(ieee802154_radio_get_tx_power_obj,
    ieee802154_radio_get_tx_power);

static mp_obj_t ieee802154_radio_set_tx_power(mp_obj_t self_in, mp_obj_t value) {
    ieee802154_radio_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    mp_int_t want = mp_obj_get_int(value);

    uint8_t length = 0;
    const int8_t *table = tx_power_table(&length);
    if (want < table[0] || want > table[length - 1]) {
        mp_raise_ValueError_varg(MP_ERROR_TEXT("tx_power must be %d to %d dBm"),
            table[0], table[length - 1]);
    }
    // Round down to a supported step, the same choice the driver makes, and
    // store that -- so the value that comes back out is the one in use.
    int8_t chosen = table[0];
    for (uint8_t i = length; i-- > 0;) {
        if (table[i] <= want) {
            chosen = table[i];
            break;
        }
    }
    check_esp_err(esp_ieee802154_set_txpower(chosen));
    apply_pib();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(ieee802154_radio_set_tx_power_obj,
    ieee802154_radio_set_tx_power);
MP_PROPERTY_GETSET(ieee802154_radio_tx_power_obj,
    (mp_obj_t)&ieee802154_radio_get_tx_power_obj,
    (mp_obj_t)&ieee802154_radio_set_tx_power_obj);

//|     tx_powers: tuple[int, ...]
//|     """Every transmit power the radio supports, in dBm, lowest first."""
static mp_obj_t ieee802154_radio_get_tx_powers(mp_obj_t self_in) {
    ieee802154_radio_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    uint8_t length = 0;
    const int8_t *table = tx_power_table(&length);
    mp_obj_t items[length];
    for (uint8_t i = 0; i < length; i++) {
        items[i] = MP_OBJ_NEW_SMALL_INT(table[i]);
    }
    return mp_obj_new_tuple(length, items);
}
static MP_DEFINE_CONST_FUN_OBJ_1(ieee802154_radio_get_tx_powers_obj,
    ieee802154_radio_get_tx_powers);
MP_PROPERTY_GETTER(ieee802154_radio_tx_powers_obj,
    (mp_obj_t)&ieee802154_radio_get_tx_powers_obj);

//|     pan_id: int
//|     """The network this node belongs to."""
SIMPLE_GETSET(pan_id, esp_ieee802154_get_panid, esp_ieee802154_set_panid, 0, 0xFFFF, uint16_t);

//|     short_address: int
//|     """This node's 16-bit address, 0xFFFF when it has none."""
SIMPLE_GETSET(short_address, esp_ieee802154_get_short_address,
    esp_ieee802154_set_short_address, 0, 0xFFFF, uint16_t);

//|     cca_threshold: int
//|     """The level in dBm above which the channel counts as busy."""
SIMPLE_GETSET(cca_threshold, esp_ieee802154_get_cca_threshold,
    esp_ieee802154_set_cca_threshold, -128, 127, int8_t);

//|     ack_timeout: int
//|     """How long to wait for an acknowledgement, in microseconds."""
SIMPLE_GETSET(ack_timeout, esp_ieee802154_get_ack_timeout,
    esp_ieee802154_set_ack_timeout, 0, 0x7FFFFFFF, uint32_t);

#define BOOL_GETSET(name, getter, setter)                                                  \
    static mp_obj_t ieee802154_radio_get_##name(mp_obj_t self_in) {                        \
        ieee802154_radio_obj_t *self = MP_OBJ_TO_PTR(self_in);                             \
        check_for_deinit(self);                                                            \
        return mp_obj_new_bool(getter());                                                  \
    }                                                                                      \
    static MP_DEFINE_CONST_FUN_OBJ_1(ieee802154_radio_get_##name##_obj,                    \
        ieee802154_radio_get_##name);                                                      \
    static mp_obj_t ieee802154_radio_set_##name(mp_obj_t self_in, mp_obj_t value) {        \
        ieee802154_radio_obj_t *self = MP_OBJ_TO_PTR(self_in);                             \
        check_for_deinit(self);                                                            \
        check_esp_err(setter(mp_obj_is_true(value)));                                      \
        apply_pib();                                                                       \
        return mp_const_none;                                                              \
    }                                                                                      \
    static MP_DEFINE_CONST_FUN_OBJ_2(ieee802154_radio_set_##name##_obj,                    \
        ieee802154_radio_set_##name);                                                      \
    MP_PROPERTY_GETSET(ieee802154_radio_##name##_obj,                                      \
        (mp_obj_t)&ieee802154_radio_get_##name##_obj,                                      \
        (mp_obj_t)&ieee802154_radio_set_##name##_obj)

//|     promiscuous: bool
//|     """Accept every frame, whoever it is addressed to. Turning this on also
//|     stops the radio acknowledging anything, which is what a sniffer wants."""
BOOL_GETSET(promiscuous, esp_ieee802154_get_promiscuous, esp_ieee802154_set_promiscuous);

//|     coordinator: bool
//|     """Act as a PAN coordinator."""
BOOL_GETSET(coordinator, esp_ieee802154_get_coordinator, esp_ieee802154_set_coordinator);

//|     rx_when_idle: bool
//|     """Return to listening whenever the radio has nothing else to do."""
BOOL_GETSET(rx_when_idle, esp_ieee802154_get_rx_when_idle, esp_ieee802154_set_rx_when_idle);

//|     auto_ack: bool
//|     """Let the hardware answer frames that ask for an acknowledgement.
//|
//|     On by default, and there is no way to send an acknowledgement from Python
//|     instead: the standard allows 192 microseconds between the end of a frame
//|     and the start of its acknowledgement, which no interpreter can meet. What
//|     Python does control is what the acknowledgement says -- see `add_pending`
//|     for the frame pending bit -- and of course any reply sent as an ordinary
//|     frame, which has no deadline at all."""
static mp_obj_t ieee802154_radio_get_auto_ack(mp_obj_t self_in) {
    ieee802154_radio_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    return mp_obj_new_bool(esp_ieee802154_get_auto_ack_rx());
}
static MP_DEFINE_CONST_FUN_OBJ_1(ieee802154_radio_get_auto_ack_obj, ieee802154_radio_get_auto_ack);

static mp_obj_t ieee802154_radio_set_auto_ack(mp_obj_t self_in, mp_obj_t value) {
    ieee802154_radio_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    bool enable = mp_obj_is_true(value);
    // Both directions together: answering frames but not waiting for answers,
    // or the other way round, is never what a caller means by "acknowledge".
    check_esp_err(esp_ieee802154_set_auto_ack_rx(enable));
    check_esp_err(esp_ieee802154_set_auto_ack_tx(enable));
    apply_pib();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(ieee802154_radio_set_auto_ack_obj, ieee802154_radio_set_auto_ack);
MP_PROPERTY_GETSET(ieee802154_radio_auto_ack_obj,
    (mp_obj_t)&ieee802154_radio_get_auto_ack_obj,
    (mp_obj_t)&ieee802154_radio_set_auto_ack_obj);

//|     extended_address: bytes
//|     """This node's 8-byte address."""
static mp_obj_t ieee802154_radio_get_extended_address(mp_obj_t self_in) {
    ieee802154_radio_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    uint8_t addr[8];
    check_esp_err(esp_ieee802154_get_extended_address(addr));
    return mp_obj_new_bytes(addr, sizeof(addr));
}
static MP_DEFINE_CONST_FUN_OBJ_1(ieee802154_radio_get_extended_address_obj,
    ieee802154_radio_get_extended_address);

static mp_obj_t ieee802154_radio_set_extended_address(mp_obj_t self_in, mp_obj_t value) {
    ieee802154_radio_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    mp_buffer_info_t buf;
    mp_get_buffer_raise(value, &buf, MP_BUFFER_READ);
    if (buf.len != 8) {
        mp_raise_ValueError(MP_ERROR_TEXT("extended_address must be 8 bytes"));
    }
    check_esp_err(esp_ieee802154_set_extended_address(buf.buf));
    apply_pib();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(ieee802154_radio_set_extended_address_obj,
    ieee802154_radio_set_extended_address);
MP_PROPERTY_GETSET(ieee802154_radio_extended_address_obj,
    (mp_obj_t)&ieee802154_radio_get_extended_address_obj,
    (mp_obj_t)&ieee802154_radio_set_extended_address_obj);

//|     pending_mode: int
//|     """How the frame pending bit in outgoing acknowledgements is decided.
//|     One of the ``PENDING_`` constants."""
// The driver has no getter for this, so the object remembers what it was told.
static mp_obj_t ieee802154_radio_get_pending_mode(mp_obj_t self_in) {
    ieee802154_radio_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    return MP_OBJ_NEW_SMALL_INT(self->pending_mode);
}
static MP_DEFINE_CONST_FUN_OBJ_1(ieee802154_radio_get_pending_mode_obj,
    ieee802154_radio_get_pending_mode);

static mp_obj_t ieee802154_radio_set_pending_mode(mp_obj_t self_in, mp_obj_t value) {
    ieee802154_radio_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    mp_int_t mode = mp_arg_validate_int_range(mp_obj_get_int(value), 0, 3, MP_QSTR_pending_mode);
    check_esp_err(esp_ieee802154_set_pending_mode((esp_ieee802154_pending_mode_t)mode));
    self->pending_mode = (uint8_t)mode;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(ieee802154_radio_set_pending_mode_obj,
    ieee802154_radio_set_pending_mode);
MP_PROPERTY_GETSET(ieee802154_radio_pending_mode_obj,
    (mp_obj_t)&ieee802154_radio_get_pending_mode_obj,
    (mp_obj_t)&ieee802154_radio_set_pending_mode_obj);

//|     lost: int
//|     """Frames dropped because the queue was full."""
static mp_obj_t ieee802154_radio_get_lost(mp_obj_t self_in) {
    ieee802154_radio_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    return mp_obj_new_int_from_uint(self->lost);
}
static MP_DEFINE_CONST_FUN_OBJ_1(ieee802154_radio_get_lost_obj, ieee802154_radio_get_lost);
MP_PROPERTY_GETTER(ieee802154_radio_lost_obj, (mp_obj_t)&ieee802154_radio_get_lost_obj);

//|     callbacks: int
//|     """How many times the driver has handed a frame up, counted before this
//|     module looks at it. Compare against what `receive` returns: equal means
//|     nothing is being lost here, zero means the radio is not delivering at
//|     all."""
static mp_obj_t ieee802154_radio_get_callbacks(mp_obj_t self_in) {
    ieee802154_radio_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    return mp_obj_new_int_from_uint(_rx_callbacks);
}
static MP_DEFINE_CONST_FUN_OBJ_1(ieee802154_radio_get_callbacks_obj,
    ieee802154_radio_get_callbacks);
MP_PROPERTY_GETTER(ieee802154_radio_callbacks_obj,
    (mp_obj_t)&ieee802154_radio_get_callbacks_obj);

//|     last_rssi: int
//|     """Signal strength of the most recent frame, in dBm."""
static mp_obj_t ieee802154_radio_get_last_rssi(mp_obj_t self_in) {
    ieee802154_radio_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    return MP_OBJ_NEW_SMALL_INT(esp_ieee802154_get_recent_rssi());
}
static MP_DEFINE_CONST_FUN_OBJ_1(ieee802154_radio_get_last_rssi_obj,
    ieee802154_radio_get_last_rssi);
MP_PROPERTY_GETTER(ieee802154_radio_last_rssi_obj, (mp_obj_t)&ieee802154_radio_get_last_rssi_obj);

//|     last_lqi: int
//|     """Link quality of the most recent frame, 0 to 255."""
static mp_obj_t ieee802154_radio_get_last_lqi(mp_obj_t self_in) {
    ieee802154_radio_obj_t *self = MP_OBJ_TO_PTR(self_in);
    check_for_deinit(self);
    return MP_OBJ_NEW_SMALL_INT(esp_ieee802154_get_recent_lqi());
}
static MP_DEFINE_CONST_FUN_OBJ_1(ieee802154_radio_get_last_lqi_obj, ieee802154_radio_get_last_lqi);
MP_PROPERTY_GETTER(ieee802154_radio_last_lqi_obj, (mp_obj_t)&ieee802154_radio_get_last_lqi_obj);

static const mp_rom_map_elem_t ieee802154_radio_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&ieee802154_radio_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&ieee802154_radio___exit___obj) },

    { MP_ROM_QSTR(MP_QSTR_send), MP_ROM_PTR(&ieee802154_radio_send_obj) },
    { MP_ROM_QSTR(MP_QSTR_receive), MP_ROM_PTR(&ieee802154_radio_receive_obj) },
    { MP_ROM_QSTR(MP_QSTR_readinto), MP_ROM_PTR(&ieee802154_radio_readinto_obj) },
    { MP_ROM_QSTR(MP_QSTR_energy_detect), MP_ROM_PTR(&ieee802154_radio_energy_detect_obj) },
    { MP_ROM_QSTR(MP_QSTR_add_pending), MP_ROM_PTR(&ieee802154_radio_add_pending_obj) },
    { MP_ROM_QSTR(MP_QSTR_clear_pending), MP_ROM_PTR(&ieee802154_radio_clear_pending_obj) },
    { MP_ROM_QSTR(MP_QSTR_reset_pending), MP_ROM_PTR(&ieee802154_radio_reset_pending_obj) },

    { MP_ROM_QSTR(MP_QSTR_channel), MP_ROM_PTR(&ieee802154_radio_channel_obj) },
    { MP_ROM_QSTR(MP_QSTR_tx_power), MP_ROM_PTR(&ieee802154_radio_tx_power_obj) },
    { MP_ROM_QSTR(MP_QSTR_tx_powers), MP_ROM_PTR(&ieee802154_radio_tx_powers_obj) },
    { MP_ROM_QSTR(MP_QSTR_pan_id), MP_ROM_PTR(&ieee802154_radio_pan_id_obj) },
    { MP_ROM_QSTR(MP_QSTR_short_address), MP_ROM_PTR(&ieee802154_radio_short_address_obj) },
    { MP_ROM_QSTR(MP_QSTR_extended_address), MP_ROM_PTR(&ieee802154_radio_extended_address_obj) },
    { MP_ROM_QSTR(MP_QSTR_promiscuous), MP_ROM_PTR(&ieee802154_radio_promiscuous_obj) },
    { MP_ROM_QSTR(MP_QSTR_coordinator), MP_ROM_PTR(&ieee802154_radio_coordinator_obj) },
    { MP_ROM_QSTR(MP_QSTR_rx_when_idle), MP_ROM_PTR(&ieee802154_radio_rx_when_idle_obj) },
    { MP_ROM_QSTR(MP_QSTR_auto_ack), MP_ROM_PTR(&ieee802154_radio_auto_ack_obj) },
    { MP_ROM_QSTR(MP_QSTR_ack_timeout), MP_ROM_PTR(&ieee802154_radio_ack_timeout_obj) },
    { MP_ROM_QSTR(MP_QSTR_cca_threshold), MP_ROM_PTR(&ieee802154_radio_cca_threshold_obj) },
    { MP_ROM_QSTR(MP_QSTR_pending_mode), MP_ROM_PTR(&ieee802154_radio_pending_mode_obj) },
    { MP_ROM_QSTR(MP_QSTR_lost), MP_ROM_PTR(&ieee802154_radio_lost_obj) },
    { MP_ROM_QSTR(MP_QSTR_callbacks), MP_ROM_PTR(&ieee802154_radio_callbacks_obj) },
    { MP_ROM_QSTR(MP_QSTR_last_rssi), MP_ROM_PTR(&ieee802154_radio_last_rssi_obj) },
    { MP_ROM_QSTR(MP_QSTR_last_lqi), MP_ROM_PTR(&ieee802154_radio_last_lqi_obj) },
};
static MP_DEFINE_CONST_DICT(ieee802154_radio_locals_dict, ieee802154_radio_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    ieee802154_radio_type,
    MP_QSTR_Radio,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, ieee802154_radio_make_new,
    locals_dict, &ieee802154_radio_locals_dict
    );
