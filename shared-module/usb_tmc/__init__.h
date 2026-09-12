// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-License-Identifier: MIT

// CIRCUITPY-CHANGE: USB Test and Measurement Class (USBTMC, USB488 subclass).
// The board appears to the host as an instrument: VISA on the host sends it
// command messages and reads response messages, the way it talks to a scope
// or a meter. Python on the board takes the commands off a queue and hands
// back the responses.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "supervisor/usb.h"

// Room for the commands waiting to be read and for the one response waiting
// to be sent. SCPI traffic is short lines, so a kilobyte each is plenty; a
// message that cannot fit is refused to the host rather than cut.
#define USB_TMC_BUFFER_SIZE (1024)

// Whether the interface will be in the descriptor, decided before it is
// built, which is after boot.py has run.
bool usb_tmc_enabled(void);
bool common_hal_usb_tmc_enable(void);
bool common_hal_usb_tmc_disable(void);

size_t usb_tmc_descriptor_length(void);
size_t usb_tmc_add_descriptor(uint8_t *descriptor_buf, descriptor_counts_t *descriptor_counts,
    uint8_t *current_interface_string);

// Length of the oldest complete command message waiting, 0 when none.
size_t usb_tmc_message_available(void);
// Copies the oldest complete message into buf, which must hold it whole, and
// takes it off the queue. Returns the length copied.
size_t usb_tmc_read_message(uint8_t *buf, size_t len);

// Whether a response is queued or on its way to the host.
bool usb_tmc_response_pending(void);
// Queues one response message, sending it at once if the host is already
// waiting for it. Only when no response is pending; len at most
// USB_TMC_BUFFER_SIZE.
void usb_tmc_write_message(const uint8_t *data, size_t len);

// The IEEE 488.2 status byte the host reads. The MAV bit, 0x10, is added by
// the module while a response is pending and cannot be set from here.
uint8_t usb_tmc_get_status_byte(void);
void usb_tmc_set_status_byte(uint8_t value);
