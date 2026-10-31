// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Petr Vavrin
//
// SPDX-License-Identifier: MIT

// espidf.L2CAPServer and espidf.L2CAPChannel; see extended.h.

#include <stddef.h>
#include <string.h>

#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/obj.h"
#include "py/objproperty.h"
#include "py/objstr.h"
#include "py/runtime.h"

#include "bindings/espidf/__init__.h"
#include "bindings/espidf/extended.h"
#include "shared-bindings/_bleio/Adapter.h"
#include "shared-bindings/_bleio/Connection.h"
#include "shared-bindings/_bleio/__init__.h"
#include "shared-bindings/util.h"
#include "shared/runtime/context_manager_helpers.h"
#include "supervisor/shared/tick.h"

#include "freertos/FreeRTOS.h"
// The headers below exist only where the switch brings them in.
#if CIRCUITPY_ESPIDF_BLE_L2CAP

#include "common-hal/_bleio/Connection.h"
#include "esp_heap_caps.h"
#include "host/ble_hs.h"
#include "host/ble_l2cap.h"
#include "nimble/nimble_npl.h"
#include "nimble/nimble_port.h"
#include "os/os_mbuf.h"
// NimBLE's own: the channel struct and the LE Credits command. See below.
#include "ble_l2cap_priv.h"
#include "ble_l2cap_sig_priv.h"

// L2CAP connection-oriented channels (LE credit based flow control): a
// stream of messages (SDUs) over a BLE connection, without GATT. The peer
// sends one frame per credit, so a reader that falls behind slows the sender
// down instead of losing data.
//
// NimBLE gives its credits for one message, with no regard for how long the
// messages are, and expects a new receive buffer from the event that
// delivers one. Holding the buffer back to pace the sender crashed it: the
// peer still had credits from short messages, and the next one arrived into
// no buffer at all (a NULL dereference in ble_l2cap_coc_rx_fn). So here the
// buffer goes back at once, NimBLE's automatic credits are switched off
// (disable_auto_credit_update, which has no public setter), and credits are
// given for as many frames as the ring has room for, up to a window. That
// also lets several messages be under way at once, where NimBLE waited for
// each.
//
// Credits are only touched from the NimBLE host task, which decrements them;
// the Python task asks for an update through an event on its queue.
//
// What arrives is copied out of NimBLE's mbufs into the ring, kept outside
// the Python heap: the mbuf pool is shared with every other Bluetooth
// operation, and holding messages there until Python reads them would starve
// it.

#define MAX_SERVERS (2)
#define MAX_CHANNELS (MYNEWT_VAL(BLE_L2CAP_COC_MAX_NUM))
#define MTU_MIN (23)
// A message is held in NimBLE's mbuf pool while it goes out; see
// sdkconfig-ble-l2cap.defaults.
#define MTU_MAX (4096)
// Enough for the credits that keep the 2M PHY busy, and for two messages.
#define RING_MIN (8192)
// The most credits a peer holds, unless one message needs more frames. The
// sender queues a frame, in the shared mbuf pool, for every credit it has:
// with the 33 the ring allows, an S3 sending short messages ran out of mbufs
// (NimBLE error 6) about every twentieth one. Ten at a 7.5 ms interval still
// allow ~160 kB/s.
#define CREDIT_WINDOW (10)
#define CLOSE_WAIT_MS (1000)
#define FLUSH_WAIT_MS (2000)

typedef struct {
    uint16_t psm;
    uint16_t mtu;
    volatile bool listening;
} l2cap_server_t;

typedef struct {
    volatile enum { CH_FREE, CH_CONNECTING, CH_OPEN, CH_CLOSED } state;
    struct ble_l2cap_chan *volatile chan;
    // The server that accepted it, or -1 for a channel this side opened.
    int8_t server;
    volatile bool claimed;
    volatile int status;
    uint16_t mtu;
    uint16_t peer_mtu;
    // Under _lock: messages as a 2-byte length and the bytes, wrapping.
    uint8_t *ring;
    size_t size;
    volatile size_t head;
    volatile size_t used;
    // Given up on by slot_release() while NimBLE still had it: the event that
    // closes it frees it. Under _lock.
    bool orphaned;
    // Posted by recv() for grant_credits() to run in the host task.
    struct ble_npl_event credit_event;
    volatile bool tx_stalled;
    volatile int tx_status;
} l2cap_slot_t;

static l2cap_server_t _servers[MAX_SERVERS];
static l2cap_slot_t _slots[MAX_CHANNELS];
static portMUX_TYPE _lock = portMUX_INITIALIZER_UNLOCKED;

typedef struct {
    mp_obj_base_t base;
    l2cap_server_t *server;
} espidf_l2cap_server_obj_t;

typedef struct {
    mp_obj_base_t base;
    l2cap_slot_t *slot;
} espidf_l2cap_channel_obj_t;

static void check_nimble(int rc) {
    if (rc != 0) {
        mp_raise_OSError_msg_varg(MP_ERROR_TEXT("NimBLE error %d"), rc);
    }
}

static size_t ring_free(const l2cap_slot_t *slot) {
    return slot->size - slot->used;
}

// --- NimBLE task --------------------------------------------------------------

static void credit_event_fn(struct ble_npl_event *ev);

static bool slot_alloc_ring(l2cap_slot_t *slot, uint16_t mtu) {
    slot->size = MAX(2 * (mtu + 2), RING_MIN);
    slot->ring = heap_caps_malloc(slot->size, MALLOC_CAP_8BIT);
    slot->head = slot->used = 0;
    slot->mtu = mtu;
    slot->tx_stalled = false;
    slot->tx_status = 0;
    slot->status = 0;
    slot->claimed = false;
    slot->orphaned = false;
    if (slot->ring == NULL) {
        return false;
    }
    ble_npl_event_init(&slot->credit_event, credit_event_fn, slot);
    return true;
}

static void slot_free(l2cap_slot_t *slot) {
    ble_npl_eventq_remove(nimble_port_get_dflt_eventq(), &slot->credit_event);
    ble_npl_event_deinit(&slot->credit_event);
    heap_caps_free(slot->ring);
    slot->ring = NULL;
    slot->orphaned = false;
    slot->state = CH_FREE;
}

// The channel is gone; frees the slot too if nothing holds it any more.
static void slot_closed(l2cap_slot_t *slot) {
    taskENTER_CRITICAL(&_lock);
    slot->chan = NULL;
    slot->tx_stalled = false;
    slot->state = CH_CLOSED;
    bool orphaned = slot->orphaned;
    taskEXIT_CRITICAL(&_lock);
    if (orphaned) {
        slot_free(slot);
    }
}

// Credits for as many frames as fit in the ring beside what is already under
// way. A frame carries at most the MPS, and at most one message with its
// 2-byte length, which is what the ring keeps of it too.
static void grant_credits(l2cap_slot_t *slot) {
    struct ble_l2cap_chan *chan = slot->chan;
    if (chan == NULL || slot->state != CH_OPEN) {
        return;
    }
    const size_t unit = MIN(MYNEWT_VAL(BLE_L2CAP_COC_MPS), slot->mtu + 2);
    const struct os_mbuf *partial = chan->coc_rx.sdus[chan->coc_rx.current_sdu_idx];
    // A message coming in sits in NimBLE's buffer, without its length yet.
    size_t pending = partial != NULL && OS_MBUF_PKTLEN(partial) > 0 ? OS_MBUF_PKTLEN(partial) + 2 : 0;
    taskENTER_CRITICAL(&_lock);
    size_t room = ring_free(slot);
    taskEXIT_CRITICAL(&_lock);
    room = room > pending ? room - pending : 0;
    // Credits are given between messages only (and when the reader makes
    // room), so a window smaller than one message would never finish it.
    // NimBLE's own was exactly one message.
    const size_t window = MAX(CREDIT_WINDOW, (slot->mtu + 2 + unit - 1) / unit);
    size_t frames = MIN(room / unit, window);
    if (frames > chan->coc_rx.credits) {
        uint16_t more = frames - chan->coc_rx.credits;
        chan->coc_rx.credits += more;
        ble_l2cap_sig_le_credits(chan->conn_handle, chan->scid, more);
    }
}

static void credit_event_fn(struct ble_npl_event *ev) {
    grant_credits(ble_npl_event_get_arg(ev));
}

// Hands NimBLE the buffer for the next message. Without one, the next frame
// would crash it; closing the channel is the lesser harm.
static void give_rx_buffer(l2cap_slot_t *slot, struct ble_l2cap_chan *chan) {
    struct os_mbuf *om = os_msys_get_pkthdr(slot->mtu, 0);
    int rc = om == NULL ? BLE_HS_ENOMEM : ble_l2cap_recv_ready(chan, om);
    if (rc != 0) {
        if (om != NULL) {
            os_mbuf_free_chain(om);
        }
        slot->status = rc;
        ble_l2cap_disconnect(chan);
    }
}

static void ring_put(l2cap_slot_t *slot, size_t at, const struct os_mbuf *om, size_t off, size_t len) {
    size_t first = slot->size - at;
    if (first > len) {
        first = len;
    }
    os_mbuf_copydata(om, off, first, slot->ring + at);
    if (len > first) {
        os_mbuf_copydata(om, off + first, len - first, slot->ring);
    }
}

static void receive(l2cap_slot_t *slot, struct ble_l2cap_chan *chan, struct os_mbuf *om) {
    size_t len = OS_MBUF_PKTLEN(om);
    taskENTER_CRITICAL(&_lock);
    // The credits only allowed what fits.
    bool fits = ring_free(slot) >= len + 2;
    if (fits) {
        size_t tail = (slot->head + slot->used) % slot->size;
        slot->ring[tail] = len & 0xff;
        slot->ring[(tail + 1) % slot->size] = len >> 8;
        ring_put(slot, (tail + 2) % slot->size, om, 0, len);
        slot->used += len + 2;
    }
    taskEXIT_CRITICAL(&_lock);
    os_mbuf_free_chain(om);
    if (!fits) {
        slot->status = BLE_HS_ENOMEM;
        ble_l2cap_disconnect(chan);
        return;
    }
    give_rx_buffer(slot, chan);
    grant_credits(slot);
}

static void channel_event(l2cap_slot_t *slot, struct ble_l2cap_event *event) {
    switch (event->type) {
        case BLE_L2CAP_EVENT_COC_CONNECTED:
            if (event->connect.status == 0) {
                struct ble_l2cap_chan_info info;
                struct ble_l2cap_chan *chan = event->connect.chan;
                chan->disable_auto_credit_update = true;
                slot->chan = chan;
                if (ble_l2cap_get_chan_info(chan, &info) == 0) {
                    slot->peer_mtu = info.peer_coc_mtu;
                }
                slot->state = CH_OPEN;
                // NimBLE gave the credits for one message; the ring takes more.
                grant_credits(slot);
            } else {
                slot->status = event->connect.status;
                slot_closed(slot);
            }
            break;
        case BLE_L2CAP_EVENT_COC_DISCONNECTED:
            slot_closed(slot);
            break;
        case BLE_L2CAP_EVENT_COC_DATA_RECEIVED:
            receive(slot, event->receive.chan, event->receive.sdu_rx);
            break;
        case BLE_L2CAP_EVENT_COC_TX_UNSTALLED:
            slot->tx_status = event->tx_unstalled.status;
            slot->tx_stalled = false;
            break;
        default:
            break;
    }
}

static l2cap_slot_t *slot_of(const struct ble_l2cap_chan *chan) {
    for (size_t i = 0; i < MAX_CHANNELS; i++) {
        if (_slots[i].state != CH_FREE && _slots[i].chan == chan) {
            return &_slots[i];
        }
    }
    return NULL;
}

static int client_event(struct ble_l2cap_event *event, void *arg) {
    channel_event(arg, event);
    return 0;
}

// The events of every channel a server accepted come here too.
static int server_event(struct ble_l2cap_event *event, void *arg) {
    l2cap_server_t *server = arg;
    if (event->type != BLE_L2CAP_EVENT_COC_ACCEPT) {
        l2cap_slot_t *slot = slot_of(event->type == BLE_L2CAP_EVENT_COC_CONNECTED
            ? event->connect.chan : event->receive.chan);
        if (slot != NULL) {
            channel_event(slot, event);
        }
        return 0;
    }
    // Refusing, which a non-zero return does, is the answer to a server that
    // is not listening or has no room.
    if (!server->listening) {
        return BLE_HS_ENOTSUP;
    }
    l2cap_slot_t *slot = NULL;
    for (size_t i = 0; i < MAX_CHANNELS; i++) {
        if (_slots[i].state == CH_FREE) {
            slot = &_slots[i];
            break;
        }
    }
    if (slot == NULL || !slot_alloc_ring(slot, server->mtu)) {
        return BLE_HS_ENOMEM;
    }
    struct os_mbuf *om = os_msys_get_pkthdr(server->mtu, 0);
    int rc = om == NULL ? BLE_HS_ENOMEM : ble_l2cap_recv_ready(event->accept.chan, om);
    if (rc != 0) {
        if (om != NULL) {
            os_mbuf_free_chain(om);
        }
        slot_free(slot);
        return rc;
    }
    slot->server = server - _servers;
    slot->chan = event->accept.chan;
    slot->state = CH_CONNECTING;
    return 0;
}

// Every event but connect and accept has chan where receive has it, which
// server_event() relies on; check that it stays so.
MP_STATIC_ASSERT(offsetof(struct ble_l2cap_event, disconnect.chan) == offsetof(struct ble_l2cap_event, receive.chan));
MP_STATIC_ASSERT(offsetof(struct ble_l2cap_event, tx_unstalled.chan) == offsetof(struct ble_l2cap_event, receive.chan));

// --- Python task --------------------------------------------------------------

static uint64_t deadline_of(mp_obj_t timeout_in, mp_float_t fallback) {
    if (timeout_in == mp_const_none) {
        return UINT64_MAX;
    }
    mp_float_t timeout = timeout_in == MP_OBJ_NULL ? fallback : mp_obj_get_float(timeout_in);
    return supervisor_ticks_ms64() + (uint64_t)(timeout * 1000);
}

// Runs background tasks once; raises on Ctrl-C, false past the deadline.
static bool keep_waiting(uint64_t deadline) {
    RUN_BACKGROUND_TASKS;
    if (mp_hal_is_interrupted()) {
        mp_handle_pending(true);
    }
    return supervisor_ticks_ms64() <= deadline;
}

// Closes the channel, if it is open, and gives the slot back.
static void slot_release(l2cap_slot_t *slot) {
    if (slot->state == CH_OPEN) {
        // What send() handed over goes out first.
        uint64_t until = supervisor_ticks_ms64() + FLUSH_WAIT_MS;
        while (slot->tx_stalled && slot->state == CH_OPEN && supervisor_ticks_ms64() < until) {
            RUN_BACKGROUND_TASKS;
        }
    }
    if ((slot->state == CH_OPEN || slot->state == CH_CONNECTING) && slot->chan != NULL) {
        ble_l2cap_disconnect(slot->chan);
    }
    uint64_t until = supervisor_ticks_ms64() + CLOSE_WAIT_MS;
    while (slot->state != CH_CLOSED && supervisor_ticks_ms64() < until) {
        RUN_BACKGROUND_TASKS;
    }
    // A channel still being opened is known to NimBLE only, which closes it at
    // its own timeout; until then it may write to the ring.
    taskENTER_CRITICAL(&_lock);
    bool closed = slot->state == CH_CLOSED;
    if (!closed) {
        slot->orphaned = true;
    }
    taskEXIT_CRITICAL(&_lock);
    if (closed) {
        slot_free(slot);
    }
}

static mp_obj_t channel_new(l2cap_slot_t *slot) {
    espidf_l2cap_channel_obj_t *self = mp_obj_malloc_with_finaliser(espidf_l2cap_channel_obj_t, &espidf_l2cap_channel_type);
    self->slot = slot;
    return MP_OBJ_FROM_PTR(self);
}

static mp_int_t validate_mtu(mp_int_t mtu) {
    return mp_arg_validate_int_range(mtu, MTU_MIN, MTU_MAX, MP_QSTR_mtu);
}

static mp_int_t validate_psm(mp_obj_t psm_in) {
    // The range LE leaves to applications.
    return mp_arg_validate_int_range(mp_obj_get_int(psm_in), 0x80, 0xff, MP_QSTR_psm);
}

//| class L2CAPServer:
//|     """Accepts L2CAP connection-oriented channels on a PSM: a stream of
//|     messages over a BLE connection, without GATT and faster than
//|     notifications. The peer opens them with `L2CAPChannel`. Starts nothing
//|     by itself: the BLE connection is made with `_bleio` as usual, in either
//|     role."""
//|
//|     def __init__(self, psm: int, *, mtu: int = 2048) -> None:
//|         """
//|         :param int psm: 0x80 to 0xFF, agreed with the peer.
//|         :param int mtu: the longest message this side receives, 23 to 4096.
//|           NimBLE keeps a PSM registered until BLE is turned off; a later
//|           server on the same PSM has to use the same ``mtu``."""
//|         ...
//|
static mp_obj_t espidf_l2cap_server_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_psm, ARG_mtu };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_psm, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_mtu, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 2048 } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    mp_int_t psm = validate_psm(args[ARG_psm].u_obj);
    mp_int_t mtu = validate_mtu(args[ARG_mtu].u_int);
    if (!common_hal_bleio_adapter_get_enabled(&common_hal_bleio_adapter_obj)) {
        common_hal_bleio_adapter_set_enabled(&common_hal_bleio_adapter_obj, true);
    }

    l2cap_server_t *server = NULL;
    for (size_t i = 0; i < MAX_SERVERS; i++) {
        if (_servers[i].psm == psm) {
            server = &_servers[i];
            break;
        }
    }
    if (server != NULL && server->listening) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("PSM in use"));
    }
    if (server == NULL) {
        for (size_t i = 0; i < MAX_SERVERS; i++) {
            if (_servers[i].psm == 0) {
                server = &_servers[i];
                break;
            }
        }
        if (server == NULL) {
            mp_raise_RuntimeError(MP_ERROR_TEXT("Too many L2CAP servers"));
        }
    }
    int rc = ble_l2cap_create_server(psm, mtu, server_event, server);
    if (rc == BLE_HS_EALREADY) {
        // Registered before, by a server this table remembers.
        if (server->psm != psm || server->mtu != mtu) {
            mp_raise_ValueError_varg(MP_ERROR_TEXT("%q must be %d"), MP_QSTR_mtu, server->mtu);
        }
    } else {
        check_nimble(rc);
    }
    server->psm = psm;
    server->mtu = mtu;
    server->listening = true;

    espidf_l2cap_server_obj_t *self = mp_obj_malloc_with_finaliser(espidf_l2cap_server_obj_t, &espidf_l2cap_server_type);
    self->server = server;
    return MP_OBJ_FROM_PTR(self);
}

//|     def accept(self, *, timeout: Optional[float] = None) -> L2CAPChannel:
//|         """The next channel a peer opened, waiting for one up to ``timeout``
//|         seconds (None: for ever); raises OSError ETIMEDOUT. Up to two
//|         channels are open at a time."""
//|         ...
//|
static mp_obj_t espidf_l2cap_server_accept(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_timeout };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_timeout, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = mp_const_none } },
    };
    espidf_l2cap_server_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    if (self->server == NULL) {
        raise_deinited_error();
    }
    uint64_t deadline = deadline_of(args[ARG_timeout].u_obj, 0);
    const int8_t index = self->server - _servers;
    do {
        for (size_t i = 0; i < MAX_CHANNELS; i++) {
            l2cap_slot_t *slot = &_slots[i];
            // A channel that closed again before being taken still holds what
            // it received.
            if (slot->server == index && !slot->claimed &&
                (slot->state == CH_OPEN || slot->state == CH_CLOSED)) {
                slot->claimed = true;
                return channel_new(slot);
            }
        }
    } while (keep_waiting(deadline));
    mp_raise_OSError(MP_ETIMEDOUT);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(espidf_l2cap_server_accept_obj, 1, espidf_l2cap_server_accept);

//|     psm: int
//|     """The PSM it listens on."""
//|
static mp_obj_t espidf_l2cap_server_get_psm(mp_obj_t self_in) {
    espidf_l2cap_server_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->server == NULL) {
        raise_deinited_error();
    }
    return MP_OBJ_NEW_SMALL_INT(self->server->psm);
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_l2cap_server_get_psm_obj, espidf_l2cap_server_get_psm);
MP_PROPERTY_GETTER(espidf_l2cap_server_psm_obj, (mp_obj_t)&espidf_l2cap_server_get_psm_obj);

//|     def deinit(self) -> None:
//|         """Stops accepting. Channels already accepted stay open."""
//|         ...
//|
//|
static mp_obj_t espidf_l2cap_server_deinit(mp_obj_t self_in) {
    espidf_l2cap_server_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->server != NULL) {
        self->server->listening = false;
        self->server = NULL;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_l2cap_server_deinit_obj, espidf_l2cap_server_deinit);

static mp_obj_t espidf_l2cap_channel_deinit(mp_obj_t self_in);

static mp_obj_t espidf_l2cap___exit__(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    return mp_obj_get_type(args[0]) == &espidf_l2cap_server_type
        ? espidf_l2cap_server_deinit(args[0])
        : espidf_l2cap_channel_deinit(args[0]);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(espidf_l2cap___exit___obj, 4, 4, espidf_l2cap___exit__);

static const mp_rom_map_elem_t espidf_l2cap_server_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_accept), MP_ROM_PTR(&espidf_l2cap_server_accept_obj) },
    { MP_ROM_QSTR(MP_QSTR_psm), MP_ROM_PTR(&espidf_l2cap_server_psm_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&espidf_l2cap_server_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&espidf_l2cap_server_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&espidf_l2cap___exit___obj) },
};
static MP_DEFINE_CONST_DICT(espidf_l2cap_server_locals_dict, espidf_l2cap_server_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    espidf_l2cap_server_type,
    MP_QSTR_L2CAPServer,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, espidf_l2cap_server_make_new,
    locals_dict, &espidf_l2cap_server_locals_dict
    );

//| class L2CAPChannel:
//|     """An L2CAP connection-oriented channel: messages of up to `peer_mtu`
//|     bytes each way, delivered whole and in order, with flow control. Made
//|     by `L2CAPServer.accept` on one side and by the constructor on the
//|     other."""
//|
//|     def __init__(
//|         self, connection: _bleio.Connection, psm: int, *, mtu: int = 2048, timeout: float = 5.0
//|     ) -> None:
//|         """Opens a channel to the `L2CAPServer` on ``psm`` at the other end
//|         of ``connection``.
//|
//|         :param int mtu: the longest message this side receives, 23 to 4096.
//|         :param float timeout: seconds to wait for the peer; raises OSError
//|           ETIMEDOUT, and ConnectionError if it refuses."""
//|         ...
//|
static mp_obj_t espidf_l2cap_channel_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_connection, ARG_psm, ARG_mtu, ARG_timeout };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_connection, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_psm, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_mtu, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 2048 } },
        { MP_QSTR_timeout, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = MP_OBJ_NULL } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    bleio_connection_obj_t *connection = mp_arg_validate_type(args[ARG_connection].u_obj, &bleio_connection_type, MP_QSTR_connection);
    uint16_t conn_handle = bleio_connection_get_conn_handle(connection);
    bleio_check_connected(conn_handle);
    mp_int_t psm = validate_psm(args[ARG_psm].u_obj);
    mp_int_t mtu = validate_mtu(args[ARG_mtu].u_int);
    uint64_t deadline = deadline_of(args[ARG_timeout].u_obj, 5);

    l2cap_slot_t *slot = NULL;
    for (size_t i = 0; i < MAX_CHANNELS; i++) {
        if (_slots[i].state == CH_FREE) {
            slot = &_slots[i];
            break;
        }
    }
    if (slot == NULL) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("Too many L2CAP channels"));
    }
    if (!slot_alloc_ring(slot, mtu)) {
        mp_raise_msg(&mp_type_MemoryError, NULL);
    }
    struct os_mbuf *om = os_msys_get_pkthdr(mtu, 0);
    if (om == NULL) {
        slot_free(slot);
        mp_raise_msg(&mp_type_MemoryError, NULL);
    }
    slot->server = -1;
    slot->claimed = true;
    slot->chan = NULL;
    slot->state = CH_CONNECTING;
    // Once NimBLE has made the channel, a failure frees the buffer with it.
    // The failures before (no connection, no channel left) cannot happen
    // here: the connection was checked, and there are as many slots as
    // channels.
    int rc = ble_l2cap_connect(conn_handle, psm, mtu, om, client_event, slot);
    if (rc != 0) {
        slot_free(slot);
        check_nimble(rc);
    }
    espidf_l2cap_channel_obj_t *self = MP_OBJ_TO_PTR(channel_new(slot));

    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        while (slot->state == CH_CONNECTING) {
            if (!keep_waiting(deadline)) {
                mp_raise_OSError(MP_ETIMEDOUT);
            }
        }
        nlr_pop();
    } else {
        self->slot = NULL;
        slot_release(slot);
        nlr_jump(nlr.ret_val);
    }
    if (slot->state != CH_OPEN) {
        int status = slot->status;
        self->slot = NULL;
        slot_release(slot);
        mp_raise_msg_varg(&mp_type_ConnectionError, MP_ERROR_TEXT("L2CAP channel refused: %d"), status);
    }
    return MP_OBJ_FROM_PTR(self);
}

static l2cap_slot_t *get_slot(mp_obj_t self_in) {
    espidf_l2cap_channel_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->slot == NULL) {
        raise_deinited_error();
    }
    return self->slot;
}

//|     def send(self, data: ReadableBuffer, *, timeout: float = 5.0) -> None:
//|         """Sends ``data`` as one message, at most `peer_mtu` bytes. Returns
//|         once NimBLE has taken it, which waits for the message before to have
//|         gone out: the peer's credits pace the sender. Raises OSError
//|         ETIMEDOUT if that takes longer than ``timeout`` seconds, and
//|         ConnectionError once the channel is closed. If NimBLE lost the
//|         previous message (it ran out of memory for its frames), raises
//|         OSError instead of sending this one."""
//|         ...
//|
static mp_obj_t espidf_l2cap_channel_send(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_data, ARG_timeout };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_data, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_timeout, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = MP_OBJ_NULL } },
    };
    l2cap_slot_t *slot = get_slot(pos_args[0]);
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    mp_buffer_info_t data;
    mp_get_buffer_raise(args[ARG_data].u_obj, &data, MP_BUFFER_READ);
    uint64_t deadline = deadline_of(args[ARG_timeout].u_obj, 5);

    while (slot->tx_stalled && slot->state == CH_OPEN) {
        if (!keep_waiting(deadline)) {
            mp_raise_OSError(MP_ETIMEDOUT);
        }
    }
    if (slot->state != CH_OPEN) {
        mp_raise_ConnectionError(MP_ERROR_TEXT("L2CAP channel closed"));
    }
    if (slot->tx_status != 0) {
        // NimBLE dropped the rest of the last message when it ran out of
        // mbufs for its frames.
        int status = slot->tx_status;
        slot->tx_status = 0;
        mp_raise_OSError_msg_varg(MP_ERROR_TEXT("Previous message lost: NimBLE error %d"), status);
    }
    mp_arg_validate_length_max(data.len, slot->peer_mtu, MP_QSTR_data);

    // The pool is shared and drains as packets go out. NimBLE cuts the frames
    // from it as it sends, and a frame that finds no mbuf loses the message,
    // so wait until there is room for the message and for a window of frames.
    const int frames = (data.len + 2) / MYNEWT_VAL(BLE_L2CAP_COC_MPS) + 1;
    const int needed = data.len / MYNEWT_VAL(MSYS_1_BLOCK_SIZE) + 1 + 2 * frames;
    struct os_mbuf *om = NULL;
    for (;;) {
        if (os_msys_num_free() >= needed) {
            om = os_msys_get_pkthdr(data.len, 0);
            if (om != NULL) {
                if (os_mbuf_append(om, data.buf, data.len) == 0) {
                    break;
                }
                os_mbuf_free_chain(om);
            }
        }
        if (!keep_waiting(deadline)) {
            mp_raise_OSError(MP_ETIMEDOUT);
        }
    }
    // Stalled until NimBLE says otherwise; it may, from its own task, before
    // ble_l2cap_send() returns.
    slot->tx_stalled = true;
    int rc = ble_l2cap_send(slot->chan, om);
    if (rc == 0) {
        slot->tx_stalled = false;
    } else if (rc != BLE_HS_ESTALLED) {
        slot->tx_stalled = false;
        // Refused before NimBLE took the mbuf; any later failure freed it.
        if (rc == BLE_HS_EBUSY || rc == BLE_HS_EBADDATA) {
            os_mbuf_free_chain(om);
        }
        check_nimble(rc);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(espidf_l2cap_channel_send_obj, 1, espidf_l2cap_channel_send);

//|     def recv(self, *, timeout: Optional[float] = 0) -> Optional[bytes]:
//|         """The next message, waiting up to ``timeout`` seconds for one (0:
//|         not at all, None: for ever); None if none came. Once the messages
//|         received are read, raises ConnectionError if the channel has closed.
//|         What arrives waits in a buffer of 8 KB, or of two messages if they
//|         are longer; while it is full, the peer waits."""
//|         ...
//|
static mp_obj_t espidf_l2cap_channel_recv(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_timeout };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_timeout, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = MP_OBJ_NULL } },
    };
    l2cap_slot_t *slot = get_slot(pos_args[0]);
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    uint64_t deadline = deadline_of(args[ARG_timeout].u_obj, 0);

    while (slot->used == 0) {
        if (slot->state == CH_CLOSED) {
            mp_raise_ConnectionError(MP_ERROR_TEXT("L2CAP channel closed"));
        }
        if (!keep_waiting(deadline)) {
            return mp_const_none;
        }
    }
    // Only this task takes messages out, so the one at head stays put while
    // the bytes object is allocated.
    size_t head = slot->head;
    size_t len = slot->ring[head] | (slot->ring[(head + 1) % slot->size] << 8);
    vstr_t out;
    vstr_init_len(&out, len);
    size_t at = (head + 2) % slot->size;
    size_t first = slot->size - at;
    if (first > len) {
        first = len;
    }
    memcpy(out.buf, slot->ring + at, first);
    memcpy(out.buf + first, slot->ring, len - first);
    taskENTER_CRITICAL(&_lock);
    slot->head = (head + 2 + len) % slot->size;
    slot->used -= len + 2;
    taskEXIT_CRITICAL(&_lock);
    if (slot->state == CH_OPEN) {
        // The room just made becomes credits, in the host task.
        ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &slot->credit_event);
    }
    return mp_obj_new_bytes_from_vstr(&out);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(espidf_l2cap_channel_recv_obj, 1, espidf_l2cap_channel_recv);

//|     mtu: int
//|     """The longest message this side receives."""
//|
static mp_obj_t espidf_l2cap_channel_get_mtu(mp_obj_t self_in) {
    return MP_OBJ_NEW_SMALL_INT(get_slot(self_in)->mtu);
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_l2cap_channel_get_mtu_obj, espidf_l2cap_channel_get_mtu);
MP_PROPERTY_GETTER(espidf_l2cap_channel_mtu_obj, (mp_obj_t)&espidf_l2cap_channel_get_mtu_obj);

//|     peer_mtu: int
//|     """The longest message `send` may send."""
//|
static mp_obj_t espidf_l2cap_channel_get_peer_mtu(mp_obj_t self_in) {
    return MP_OBJ_NEW_SMALL_INT(get_slot(self_in)->peer_mtu);
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_l2cap_channel_get_peer_mtu_obj, espidf_l2cap_channel_get_peer_mtu);
MP_PROPERTY_GETTER(espidf_l2cap_channel_peer_mtu_obj, (mp_obj_t)&espidf_l2cap_channel_get_peer_mtu_obj);

//|     connected: bool
//|     """Whether the channel is still open."""
//|
static mp_obj_t espidf_l2cap_channel_get_connected(mp_obj_t self_in) {
    espidf_l2cap_channel_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_bool(self->slot != NULL && self->slot->state == CH_OPEN);
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_l2cap_channel_get_connected_obj, espidf_l2cap_channel_get_connected);
MP_PROPERTY_GETTER(espidf_l2cap_channel_connected_obj, (mp_obj_t)&espidf_l2cap_channel_get_connected_obj);

//|     def deinit(self) -> None:
//|         """Closes the channel, after what `send` handed over has gone out.
//|         The BLE connection stays."""
//|         ...
//|
//|
static mp_obj_t espidf_l2cap_channel_deinit(mp_obj_t self_in) {
    espidf_l2cap_channel_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->slot != NULL) {
        slot_release(self->slot);
        self->slot = NULL;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_l2cap_channel_deinit_obj, espidf_l2cap_channel_deinit);

static const mp_rom_map_elem_t espidf_l2cap_channel_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_send), MP_ROM_PTR(&espidf_l2cap_channel_send_obj) },
    { MP_ROM_QSTR(MP_QSTR_recv), MP_ROM_PTR(&espidf_l2cap_channel_recv_obj) },
    { MP_ROM_QSTR(MP_QSTR_mtu), MP_ROM_PTR(&espidf_l2cap_channel_mtu_obj) },
    { MP_ROM_QSTR(MP_QSTR_peer_mtu), MP_ROM_PTR(&espidf_l2cap_channel_peer_mtu_obj) },
    { MP_ROM_QSTR(MP_QSTR_connected), MP_ROM_PTR(&espidf_l2cap_channel_connected_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&espidf_l2cap_channel_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&espidf_l2cap_channel_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&espidf_l2cap___exit___obj) },
};
static MP_DEFINE_CONST_DICT(espidf_l2cap_channel_locals_dict, espidf_l2cap_channel_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    espidf_l2cap_channel_type,
    MP_QSTR_L2CAPChannel,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, espidf_l2cap_channel_make_new,
    locals_dict, &espidf_l2cap_channel_locals_dict
    );

void espidf_ble_l2cap_reset(void) {
    for (size_t i = 0; i < MAX_SERVERS; i++) {
        _servers[i].listening = false;
    }
    for (size_t i = 0; i < MAX_CHANNELS; i++) {
        if (_slots[i].state != CH_FREE) {
            slot_release(&_slots[i]);
        }
    }
}

#endif
