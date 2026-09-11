// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-License-Identifier: MIT

// CIRCUITPY-CHANGE: USB network interface. The board appears to the host as an
// Ethernet adapter, hands it an address of its own and answers on it, so that
// a browser on the host reaches a server on the board with no network of any
// kind in between.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "supervisor/usb.h"

// A DNS label may be 63 characters; a name longer than that is not something
// anyone types into a browser.
#define USB_NET_HOSTNAME_MAX_LEN (63)

// Everything boot.py can set about the link. Addresses are in the order they
// have on the wire.
typedef struct {
    uint8_t ipv4_address[4];
    uint8_t netmask[4];
    // The one address the DHCP server gives away.
    uint8_t host_ipv4_address[4];
    // Offered to the host as its router. All zero offers none, so the host
    // keeps sending everything that is not for the board where it did before.
    uint8_t gateway[4];
    uint8_t mac_address[6];
    uint8_t host_mac_address[6];
    // The name the board answers for in DNS. Empty answers for none.
    char hostname[USB_NET_HOSTNAME_MAX_LEN + 1];
} usb_net_config_t;

// What the link uses when boot.py sets nothing: 192.168.7.1/24 for the board,
// .2 for the host, and addresses derived from the processor's unique id.
void usb_net_default_config(usb_net_config_t *config);

// Whether the interface will be in the descriptor, decided before it is
// built, which is after boot.py has run.
bool usb_net_enabled(void);
bool common_hal_usb_net_enable(const usb_net_config_t *config);
bool common_hal_usb_net_disable(void);

// The configuration in force: boot.py's, or the defaults.
const usb_net_config_t *usb_net_config(void);

size_t usb_net_descriptor_length(void);
size_t usb_net_add_descriptor(uint8_t *descriptor_buf, descriptor_counts_t *descriptor_counts,
    uint8_t *current_interface_string);

// Implemented by the port: hand one received Ethernet frame to the network
// stack. The frame is only valid for the duration of the call.
void usb_net_port_receive(const uint8_t *frame, size_t len);

// Implemented by the port: bring the interface up. Called once, after the
// descriptors are built, if the interface is in them.
void usb_net_port_init(void);

// Called by the port, from the task that runs tud_task(), to send one Ethernet
// frame. Returns false when the class cannot take it right now, in which case
// the caller must keep or drop it.
bool usb_net_send(const uint8_t *frame, size_t len);
