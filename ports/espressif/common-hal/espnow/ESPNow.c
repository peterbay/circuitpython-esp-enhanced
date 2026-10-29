// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2017-2020 Nick Moore
// SPDX-FileCopyrightText: Copyright (c) 2018 shawwwn <shawwwn1@gmail.com>
// SPDX-FileCopyrightText: Copyright (c) 2020-2021 Glenn Moloney @glenn20
// SPDX-FileCopyrightText: Copyright (c) 2023 MicroDev
//
// SPDX-License-Identifier: MIT

#include <string.h>

#include "py/mperrno.h"
#include "py/runtime.h"

#include "bindings/espidf/__init__.h"
#include "bindings/espnow/ESPNowPacket.h"
#include "bindings/espnow/Peer.h"

#include "shared-bindings/wifi/__init__.h"

#include "common-hal/espnow/ESPNow.h"

#include "esp_now.h"
#include "freertos/FreeRTOS.h"
#include "mphalport.h"

#include "esp_now.h"

#define ESPNOW_MAGIC 0x99

// TODO: deinit wifi?

// CIRCUITPY-CHANGE: ESP-NOW v2, which this ESP-IDF speaks, carries up to 1470
// bytes; v1 carried 250, and a v1 receiver truncates or drops anything longer.
#ifdef ESP_NOW_MAX_DATA_LEN_V2
#define ESPNOW_MAX_MSG_LEN ESP_NOW_MAX_DATA_LEN_V2
#else
#define ESPNOW_MAX_MSG_LEN ESP_NOW_MAX_DATA_LEN
#endif

// The min/max length of an espnow packet (bytes)
#define MIN_PACKET_LEN (sizeof(espnow_packet_t))
#define MAX_PACKET_LEN (sizeof(espnow_packet_t) + ESPNOW_MAX_MSG_LEN)

// Enough for 2 full-size packets: 2 * (6 + 8 + 1470) = 2968 bytes
// Will allocate an additional 7 bytes for buffer overhead
#define DEFAULT_RECV_BUFFER_SIZE (2 * MAX_PACKET_LEN)

// Time to wait (millisec) for responses from sent packets: (2 seconds).
#define DEFAULT_SEND_TIMEOUT_MS (2000)

// ESPNow packet format for the receive buffer.
// Use this for peeking at the header of the next packet in the buffer.
typedef struct {
    uint8_t magic;              // = ESPNOW_MAGIC
    // CIRCUITPY-CHANGE: 16 bits. A v2 message longer than 255 bytes was stored
    // with its length truncated, which left every later read misaligned.
    uint16_t msg_len;           // Length of the message
    uint32_t time_ms;           // Timestamp (ms) when packet is received
    int8_t rssi;                // RSSI value (dBm) (-127 to 0)
} __attribute__((packed)) espnow_header_t;

typedef struct {
    espnow_header_t header;     // The header
    uint8_t peer[6];            // Peer address
    uint8_t msg[0];             // Message is up to ESPNOW_MAX_MSG_LEN bytes
} __attribute__((packed)) espnow_packet_t;

// --- The ESP-NOW send and recv callback routines ---

// Callback triggered when a sent packet is acknowledged by the peer (or not).
// Just count the number of responses and number of failures.
// These are used in the send() logic.
static void send_cb(const esp_now_send_info_t *tx_info, esp_now_send_status_t status) {
    espnow_obj_t *self = MP_STATE_PORT(espnow_singleton);
    if (status == ESP_NOW_SEND_SUCCESS) {
        self->send_success++;
    } else {
        self->send_failure++;
    }
}

// Callback triggered when an ESP-NOW packet is received.
// Write the peer MAC address and the message into the recv_buffer as an ESPNow packet.
// If the buffer is full, drop the message and increment the dropped count.
// CIRCUITPY-CHANGE: this callback runs in the Wi-Fi task and read() in the
// VM's, on the S3 on the other core, and the ring buffer's bookkeeping is not
// safe against that: put and get each change its fill count a byte at a time.
// Unlocked, read() saw the header of a record before its message had been
// written and raised "Invalid buffer" -- within seconds at a few hundred
// messages a second -- and the count itself could be corrupted. A record is
// now written whole under this lock, and read under it.
static portMUX_TYPE recv_lock = portMUX_INITIALIZER_UNLOCKED;

static void recv_cb(const esp_now_recv_info_t *esp_now_info, const uint8_t *msg, int msg_len) {
    espnow_obj_t *self = MP_STATE_PORT(espnow_singleton);
    ringbuf_t *buf = self->recv_buffer;

    espnow_header_t header;
    header.magic = ESPNOW_MAGIC;
    header.msg_len = msg_len;
    header.rssi = esp_now_info->rx_ctrl->rssi;
    header.time_ms = mp_hal_ticks_ms();

    taskENTER_CRITICAL(&recv_lock);
    if (sizeof(espnow_packet_t) + msg_len > ringbuf_num_empty(buf)) {
        taskEXIT_CRITICAL(&recv_lock);
        self->read_failure++;
        return;
    }
    ringbuf_put_n(buf, (uint8_t *)&header, sizeof(header));
    ringbuf_put_n(buf, esp_now_info->src_addr, ESP_NOW_ETH_ALEN);
    ringbuf_put_n(buf, msg, msg_len);
    taskEXIT_CRITICAL(&recv_lock);

    self->read_success++;
}

bool common_hal_espnow_deinited(espnow_obj_t *self) {
    return self == NULL || self->recv_buffer == NULL;
}

// CIRCUITPY-CHANGE: the PHY mode the driver wants alongside each rate, or -1
// for a value that is not a rate.
static int phy_mode_for(mp_int_t rate) {
    if (rate == WIFI_PHY_RATE_LORA_250K || rate == WIFI_PHY_RATE_LORA_500K) {
        return WIFI_PHY_MODE_LR;
    }
    if (rate <= WIFI_PHY_RATE_11M_L || (rate >= WIFI_PHY_RATE_2M_S && rate <= WIFI_PHY_RATE_11M_S)) {
        return WIFI_PHY_MODE_11B;
    }
    if (rate >= WIFI_PHY_RATE_48M && rate <= WIFI_PHY_RATE_9M) {
        return WIFI_PHY_MODE_11G;
    }
    // MCS8 and MCS9 exist only on Wi-Fi 6 parts and need HE20; not supported.
    if ((rate >= WIFI_PHY_RATE_MCS0_LGI && rate <= WIFI_PHY_RATE_MCS7_LGI) ||
        (rate >= WIFI_PHY_RATE_MCS0_SGI && rate <= WIFI_PHY_RATE_MCS7_SGI)) {
        return WIFI_PHY_MODE_HT20;
    }
    return -1;
}

// Interfaces the current Wi-Fi mode has; the driver refuses the protocol calls
// for any other.
static bool interface_up(wifi_interface_t ifx) {
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    return ifx == WIFI_IF_STA ? (mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA)
                              : (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA);
}

// The plural calls rather than esp_wifi_get/set_protocol(): on a dual-band part
// in its default band mode (2.4 + 5 GHz, as CircuitPython sets the C5) the
// singular ones refuse with ESP_ERR_NOT_SUPPORTED.
static void save_protocols(espnow_obj_t *self) {
    memset(&self->protocols_sta, 0, sizeof(self->protocols_sta));
    memset(&self->protocols_ap, 0, sizeof(self->protocols_ap));
    if (interface_up(WIFI_IF_STA)) {
        CHECK_ESP_RESULT(esp_wifi_get_protocols(WIFI_IF_STA, &self->protocols_sta));
    }
    if (interface_up(WIFI_IF_AP)) {
        CHECK_ESP_RESULT(esp_wifi_get_protocols(WIFI_IF_AP, &self->protocols_ap));
    }
}

static void set_protocols(wifi_interface_t ifx, const wifi_protocols_t *saved, bool lr) {
    if (!interface_up(ifx) || saved->ghz_2g == 0) {
        return;
    }
    wifi_protocols_t protocols = *saved;
    if (lr) {
        protocols.ghz_2g |= WIFI_PROTOCOL_LR;
    }
    CHECK_ESP_RESULT(esp_wifi_set_protocols(ifx, &protocols));
}

// For deinit, which also runs from espnow_reset() on a soft reset, where
// raising is not an option. A refusal here means the interface went away with
// the Wi-Fi mode, and its protocols with it.
static void restore_protocols(espnow_obj_t *self) {
    if (interface_up(WIFI_IF_STA) && self->protocols_sta.ghz_2g) {
        esp_wifi_set_protocols(WIFI_IF_STA, &self->protocols_sta);
    }
    if (interface_up(WIFI_IF_AP) && self->protocols_ap.ghz_2g) {
        esp_wifi_set_protocols(WIFI_IF_AP, &self->protocols_ap);
    }
}

// CIRCUITPY-CHANGE: Espressif's Long Range rates are only sent, and only
// received, on an interface that has WIFI_PROTOCOL_LR among its protocols --
// on both ends, so the receiver sets the same phy_rate. Any other rate puts
// the protocols back as they were.
static void set_long_range(espnow_obj_t *self) {
    bool lr = phy_mode_for(self->phy_rate) == WIFI_PHY_MODE_LR;
    set_protocols(WIFI_IF_STA, &self->protocols_sta, lr);
    set_protocols(WIFI_IF_AP, &self->protocols_ap, lr);
}

// Construct the ESPNow object
void common_hal_espnow_construct(espnow_obj_t *self, mp_int_t buffer_size, mp_int_t phy_rate) {
    common_hal_espnow_set_phy_rate(self, phy_rate);
    self->recv_buffer_size = mp_arg_validate_int_min(buffer_size, MIN_PACKET_LEN, MP_QSTR_buffer_size);
    self->peers = espnow_peers_new();
    common_hal_espnow_init(self);
}

// Initialize the ESP-NOW software stack,
// register callbacks and allocate the recv data buffers.
void common_hal_espnow_init(espnow_obj_t *self) {
    if (!common_hal_espnow_deinited(self)) {
        return;
    }

    self->recv_buffer = m_new_obj(ringbuf_t);
    if (!ringbuf_alloc(self->recv_buffer, self->recv_buffer_size /*, true*/)) {
        m_malloc_fail(self->recv_buffer_size);
    }

    if (!common_hal_wifi_radio_get_enabled(&common_hal_wifi_radio_obj)) {
        common_hal_wifi_init(false);
        common_hal_wifi_radio_set_enabled(&common_hal_wifi_radio_obj, true);
    }

    CHECK_ESP_RESULT(esp_now_init());

    // CIRCUITPY-CHANGE: phy_rate used to be stored and never applied: the call
    // here was commented out, with a NULL peer that the driver refuses -- the
    // rate is configured per peer. Peers.append() now applies it to each peer
    // as it is added, and setting phy_rate applies it to all of them.
    save_protocols(self);
    set_long_range(self);

    CHECK_ESP_RESULT(esp_now_register_send_cb(send_cb));
    CHECK_ESP_RESULT(esp_now_register_recv_cb(recv_cb));
}

// De-initialize the ESP-NOW software stack,
// disable callbacks and deallocate the recv data buffer.
void common_hal_espnow_deinit(espnow_obj_t *self) {
    if (common_hal_espnow_deinited(self)) {
        return;
    }

    CHECK_ESP_RESULT(esp_now_unregister_send_cb());
    CHECK_ESP_RESULT(esp_now_unregister_recv_cb());
    CHECK_ESP_RESULT(esp_now_deinit());
    // CIRCUITPY-CHANGE: take WIFI_PROTOCOL_LR off again if a Long Range rate
    // put it on.
    restore_protocols(self);

    self->recv_buffer->buf = NULL;
    self->recv_buffer = NULL;
}

void espnow_reset(void) {
    common_hal_espnow_deinit(MP_STATE_PORT(espnow_singleton));
    MP_STATE_PORT(espnow_singleton) = NULL;
}

void common_hal_espnow_set_phy_rate(espnow_obj_t *self, mp_int_t value) {
    // CIRCUITPY-CHANGE: the enum has holes (0x04, 0x18-0x28 on parts without
    // Wi-Fi 6), and a value in one of them used to pass the range check.
    if (value < 0 || value >= WIFI_PHY_RATE_MAX || phy_mode_for(value) < 0) {
        mp_raise_ValueError_varg(MP_ERROR_TEXT("Invalid %q"), MP_QSTR_phy_rate);
    }
    self->phy_rate = value;
    if (common_hal_espnow_deinited(self)) {
        return;
    }
    set_long_range(self);
    size_t len;
    mp_obj_t *peers;
    mp_obj_list_get(self->peers->list, &len, &peers);
    for (size_t i = 0; i < len; i++) {
        espnow_peer_obj_t *peer = MP_OBJ_TO_PTR(peers[i]);
        common_hal_espnow_apply_phy_rate(self, peer->peer_info.peer_addr);
    }
};

void common_hal_espnow_apply_phy_rate(espnow_obj_t *self, const uint8_t *peer_addr) {
    esp_now_rate_config_t config = {
        .phymode = phy_mode_for(self->phy_rate),
        .rate = self->phy_rate,
    };
    CHECK_ESP_RESULT(esp_now_set_peer_rate_config(peer_addr, &config));
}

void common_hal_espnow_set_pmk(espnow_obj_t *self, const uint8_t *key) {
    CHECK_ESP_RESULT(esp_now_set_pmk(key));
}

// --- Send and Receive ESP-NOW data ---


mp_obj_t common_hal_espnow_send(espnow_obj_t *self, const mp_buffer_info_t *message, const uint8_t *mac) {
    // Send the packet - keep trying until timeout if the internal esp-now buffers are full.
    esp_err_t err;
    mp_uint_t start = mp_hal_ticks_ms();

    while ((ESP_ERR_ESPNOW_NO_MEM == (err = esp_now_send(mac, message->buf, message->len))) &&
           (mp_hal_ticks_ms() - start) <= DEFAULT_SEND_TIMEOUT_MS) {
        RUN_BACKGROUND_TASKS;
    }
    CHECK_ESP_RESULT(err);

    return mp_const_none;
}

mp_obj_t common_hal_espnow_read(espnow_obj_t *self) {
    // Read the packet header from the incoming buffer. Under recv_lock: see
    // recv_cb(). Once a header is out, the rest of its record is in the buffer,
    // since records go in whole and this is the only reader.
    espnow_header_t header;
    taskENTER_CRITICAL(&recv_lock);
    if (!ringbuf_num_filled(self->recv_buffer)) {
        taskEXIT_CRITICAL(&recv_lock);
        return mp_const_none;
    }
    size_t got = ringbuf_get_n(self->recv_buffer, (uint8_t *)&header, sizeof(header));
    taskEXIT_CRITICAL(&recv_lock);
    if (got != sizeof(header) || header.magic != ESPNOW_MAGIC || header.msg_len > ESPNOW_MAX_MSG_LEN) {
        mp_arg_error_invalid(MP_QSTR_buffer);
    }

    uint16_t msg_len = header.msg_len;

    uint8_t mac_buf[ESP_NOW_ETH_ALEN];
    // CIRCUITPY-CHANGE: read straight into the bytes object rather than a
    // 1.5K array on the stack. Allocated outside the lock: no allocation in a
    // critical section.
    vstr_t msg;
    vstr_init_len(&msg, msg_len);

    taskENTER_CRITICAL(&recv_lock);
    bool complete = ringbuf_get_n(self->recv_buffer, mac_buf, ESP_NOW_ETH_ALEN) == ESP_NOW_ETH_ALEN &&
        ringbuf_get_n(self->recv_buffer, (uint8_t *)msg.buf, msg_len) == msg_len;
    taskEXIT_CRITICAL(&recv_lock);
    if (!complete) {
        vstr_clear(&msg);
        mp_arg_error_invalid(MP_QSTR_buffer);
    }

    mp_obj_t elems[4] = {
        mp_obj_new_bytes(mac_buf, ESP_NOW_ETH_ALEN),
        mp_obj_new_bytes_from_vstr(&msg),
        MP_OBJ_NEW_SMALL_INT(header.rssi),
        mp_obj_new_int(header.time_ms),
    };

    return namedtuple_make_new((const mp_obj_type_t *)&espnow_packet_type_obj, 4, 0, elems);
}
