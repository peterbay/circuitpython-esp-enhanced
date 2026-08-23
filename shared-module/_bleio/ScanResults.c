// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2019 Dan Halbert for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2018 Artur Pacholec
// SPDX-FileCopyrightText: Copyright (c) 2017 Glenn Ruben Bakke
//
// SPDX-License-Identifier: MIT

#include <string.h>

#include "shared/runtime/interrupt_char.h"
#include "py/objstr.h"
#include "py/runtime.h"
#include "shared-bindings/_bleio/ScanEntry.h"
#include "shared-bindings/_bleio/ScanResults.h"

// Layout of one packet in the ring buffer, written by shared_module_bleio_scanresults_append() and
// read back by common_hal_bleio_scanresults_next(): type, ticks_ms, rssi, address, address type,
// data length, data. Both sides derive their sizes from these so the two cannot drift apart.
#define SCAN_PACKET_LENGTH_OFFSET (sizeof(uint8_t) + sizeof(uint64_t) + sizeof(int8_t) + NUM_BLEIO_ADDRESS_BYTES + sizeof(uint8_t))
#define SCAN_PACKET_HEADER_SIZE (SCAN_PACKET_LENGTH_OFFSET + sizeof(uint16_t))

bleio_scanresults_obj_t *shared_module_bleio_new_scanresults(size_t buffer_size, uint8_t *prefixes, size_t prefixes_len, mp_int_t minimum_rssi) {
    bleio_scanresults_obj_t *self = mp_obj_malloc(bleio_scanresults_obj_t, &bleio_scanresults_type);
    ringbuf_alloc(&self->buf, buffer_size);
    self->prefixes = prefixes;
    self->prefix_length = prefixes_len;
    self->minimum_rssi = minimum_rssi;
    return self;
}

mp_obj_t common_hal_bleio_scanresults_next(bleio_scanresults_obj_t *self) {
    while (ringbuf_num_filled(&self->buf) == 0 && !self->done && !mp_hal_is_interrupted()) {
        RUN_BACKGROUND_TASKS;
    }
    if (ringbuf_num_filled(&self->buf) == 0 || mp_hal_is_interrupted()) {
        return mp_const_none;
    }

    // Create a ScanEntry out of the data on the buffer.

    // CIRCUITPY-CHANGE: the advertisement data object was allocated between
    // common_hal_mcu_disable_interrupts() and common_hal_mcu_enable_interrupts(). That allocation
    // can run the collector while the BLE host task is locked out, and when it fails it raises
    // MemoryError, which skips the enable and leaves the lock held and the nesting count stuck: the
    // interrupt watchdog then resets the board instead of the error reaching Python. Size the object
    // from a peek and allocate before anything is removed, so a raise leaves the packet queued, the
    // buffer consistent and the lock free.
    uint16_t len = 0;
    common_hal_mcu_disable_interrupts();
    // append() writes each packet inside one critical section, so a non-empty buffer always starts
    // with a whole packet and this peek cannot come up short.
    ringbuf_peek_n(&self->buf, SCAN_PACKET_LENGTH_OFFSET, (uint8_t *)&len, sizeof(len));
    common_hal_mcu_enable_interrupts();

    mp_obj_str_t *o = MP_OBJ_TO_PTR(mp_obj_new_bytes_of_zeros(len));

    // Remove data atomically.
    common_hal_mcu_disable_interrupts();

    uint8_t type = ringbuf_get(&self->buf);
    bool connectable = (type & (1 << 0)) != 0;
    bool scan_response = (type & (1 << 1)) != 0;
    uint64_t ticks_ms;
    ringbuf_get_n(&self->buf, (uint8_t *)&ticks_ms, sizeof(ticks_ms));
    int8_t rssi = ringbuf_get(&self->buf);
    uint8_t peer_addr[NUM_BLEIO_ADDRESS_BYTES];
    ringbuf_get_n(&self->buf, peer_addr, sizeof(peer_addr));
    uint8_t addr_type = ringbuf_get(&self->buf);
    // Take the length back out of the buffer, but copy o->len bytes: that is what was allocated.
    ringbuf_get_n(&self->buf, (uint8_t *)&len, sizeof(len));
    ringbuf_get_n(&self->buf, (uint8_t *)o->data, o->len);

    common_hal_mcu_enable_interrupts();

    bleio_scanentry_obj_t *entry = mp_obj_malloc(bleio_scanentry_obj_t, &bleio_scanentry_type);
    entry->rssi = rssi;

    bleio_address_obj_t *address = mp_obj_malloc(bleio_address_obj_t, &bleio_address_type);
    common_hal_bleio_address_construct(MP_OBJ_TO_PTR(address), peer_addr, addr_type);
    entry->address = address;

    entry->data = o;
    entry->time_received = ticks_ms;
    entry->connectable = connectable;
    entry->scan_response = scan_response;

    return MP_OBJ_FROM_PTR(entry);
}


void shared_module_bleio_scanresults_append(bleio_scanresults_obj_t *self,
    uint64_t ticks_ms,
    bool connectable,
    bool scan_response,
    int8_t rssi,
    const uint8_t *peer_addr,
    uint8_t addr_type,
    const uint8_t *data,
    uint16_t len) {
    // Filter the packet.
    if (rssi < self->minimum_rssi) {
        return;
    }

    // If any prefixes are provided, then only include packets that include at least one of them.
    if (!bleio_scanentry_data_matches(data, len, self->prefixes, self->prefix_length, true)) {
        return;
    }
    uint8_t type = 0;
    if (connectable) {
        type |= 1 << 0;
    }
    if (scan_response) {
        type |= 1 << 1;
    }

    // Add the packet to the buffer, atomically.
    common_hal_mcu_disable_interrupts();

    // Check whether  will fit.
    int32_t packet_size = SCAN_PACKET_HEADER_SIZE + len;
    int32_t empty_space = self->buf.size - ringbuf_num_filled(&self->buf);

    if (packet_size <= empty_space) {
        // Packet will fit.
        ringbuf_put(&self->buf, type);
        ringbuf_put_n(&self->buf, (uint8_t *)&ticks_ms, sizeof(ticks_ms));
        ringbuf_put(&self->buf, rssi);
        ringbuf_put_n(&self->buf, peer_addr, NUM_BLEIO_ADDRESS_BYTES);
        ringbuf_put(&self->buf, addr_type);
        ringbuf_put_n(&self->buf, (uint8_t *)&len, sizeof(len));
        ringbuf_put_n(&self->buf, data, len);
    }

    common_hal_mcu_enable_interrupts();
}

bool shared_module_bleio_scanresults_get_done(bleio_scanresults_obj_t *self) {
    return self->done;
}

void shared_module_bleio_scanresults_set_done(bleio_scanresults_obj_t *self, bool done) {
    self->done = done;
    self->common_hal_data = NULL;
}
