// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2023 MicroDev
//
// SPDX-License-Identifier: MIT

#pragma once

#include "py/obj.h"
#include "py/ringbuf.h"

#include "bindings/espnow/Peers.h"

#include "esp_wifi.h"

typedef struct _espnow_obj_t {
    mp_obj_base_t base;
    ringbuf_t *recv_buffer;
    size_t recv_buffer_size;
    wifi_phy_rate_t phy_rate;
    // CIRCUITPY-CHANGE: the interfaces' protocols as they were before ESP-NOW
    // started, put back when it stops; the Long Range rates add
    // WIFI_PROTOCOL_LR to their 2.4 GHz side meanwhile.
    wifi_protocols_t protocols_sta;
    wifi_protocols_t protocols_ap;
    espnow_peers_obj_t *peers;
    volatile size_t send_success;
    volatile size_t send_failure;
    volatile size_t read_success;
    volatile size_t read_failure;
} espnow_obj_t;

extern void espnow_reset(void);

extern void common_hal_espnow_construct(espnow_obj_t *self, mp_int_t buffer_size, mp_int_t phy_rate);
extern void common_hal_espnow_init(espnow_obj_t *self);
extern void common_hal_espnow_deinit(espnow_obj_t *self);
extern bool common_hal_espnow_deinited(espnow_obj_t *self);

extern void common_hal_espnow_set_phy_rate(espnow_obj_t *self, mp_int_t value);
// CIRCUITPY-CHANGE: the rate is set per peer in ESP-IDF; called for each peer
// as it is added.
extern void common_hal_espnow_apply_phy_rate(espnow_obj_t *self, const uint8_t *peer_addr);
extern void common_hal_espnow_set_pmk(espnow_obj_t *self, const uint8_t *key);

extern mp_obj_t common_hal_espnow_send(espnow_obj_t *self, const mp_buffer_info_t *message, const uint8_t *mac);
extern mp_obj_t common_hal_espnow_read(espnow_obj_t *self);
