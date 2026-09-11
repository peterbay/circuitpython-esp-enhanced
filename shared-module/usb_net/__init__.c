// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-License-Identifier: MIT

// CIRCUITPY-CHANGE: see shared-module/usb_net/__init__.h.

#include <string.h>

#include "tusb.h"

#include "shared-module/usb_net/__init__.h"
#include "supervisor/usb.h"
#include "shared-bindings/microcontroller/Processor.h"

#if CIRCUITPY_USB_NET

static bool usb_net_is_enabled = CIRCUITPY_USB_NET_ENABLED_DEFAULT;

static usb_net_config_t config;
static bool config_is_set;

#if CIRCUITPY_USB_NET_RNDIS
// RNDIS reports the host's address through its own control protocol, out of
// this variable, rather than through a string descriptor.
uint8_t tud_network_mac_address[6];
#else
// The host's address as the twelve upper-case hex digits the CDC Ethernet
// functional descriptor wants, in a string descriptor of its own.
static char mac_address_string[13];
#endif

// The frame usb_net_send() is handing over, borrowed for the length of one
// tud_network_xmit() call.
static const uint8_t *pending_frame;
static uint16_t pending_frame_len;

void usb_net_default_config(usb_net_config_t *c) {
    static const uint8_t ipv4_address[4] = { 192, 168, 7, 1 };
    static const uint8_t netmask[4] = { 255, 255, 255, 0 };
    static const uint8_t host_ipv4_address[4] = { 192, 168, 7, 2 };

    memset(c, 0, sizeof(*c));
    memcpy(c->ipv4_address, ipv4_address, sizeof(ipv4_address));
    memcpy(c->netmask, netmask, sizeof(netmask));
    memcpy(c->host_ipv4_address, host_ipv4_address, sizeof(host_ipv4_address));

    uint8_t raw_id[COMMON_HAL_MCU_PROCESSOR_UID_LENGTH];
    common_hal_mcu_processor_get_uid(raw_id);

    // Locally administered and unicast: bit 1 of the first byte set, bit 0
    // clear. The two ends differ in the last bit; if they were the same, the
    // host would drop the board's frames as its own coming back.
    c->host_mac_address[0] = 0x02;
    for (size_t i = 1; i < sizeof(c->host_mac_address); i++) {
        c->host_mac_address[i] = raw_id[(COMMON_HAL_MCU_PROCESSOR_UID_LENGTH - i) %
            COMMON_HAL_MCU_PROCESSOR_UID_LENGTH];
    }
    memcpy(c->mac_address, c->host_mac_address, sizeof(c->mac_address));
    c->mac_address[5] ^= 0x01;
}

const usb_net_config_t *usb_net_config(void) {
    if (!config_is_set) {
        usb_net_default_config(&config);
        config_is_set = true;
    }
    return &config;
}

bool usb_net_enabled(void) {
    return usb_net_is_enabled;
}

// The descriptors cannot change once the host has seen them.
bool common_hal_usb_net_enable(const usb_net_config_t *new_config) {
    if (tud_connected()) {
        return false;
    }
    config = *new_config;
    config_is_set = true;
    usb_net_is_enabled = true;
    return true;
}

bool common_hal_usb_net_disable(void) {
    if (tud_connected()) {
        return false;
    }
    usb_net_is_enabled = false;
    return true;
}

#if CIRCUITPY_USB_DEVICE_HIGH_SPEED
#define NET_EP_SIZE (512)
#else
#define NET_EP_SIZE (64)
#endif
#define NET_EP_NOTIF_SIZE (64)
#define NET_EP_NOTIF_INTERVAL (50)
#define NET_MAX_SEGMENT_SIZE (1514)

#if CIRCUITPY_USB_NET_RNDIS

// Every field the descriptor takes at run time is 0xFF here; the offsets
// below are counted through the parts the macro emits: 8 association, 9
// control interface, 5 header, 5 call management, 4 acm, 5 union, 7
// notification endpoint, 9 data interface, 7 in endpoint, 7 out endpoint, 66
// bytes in all.
static const uint8_t usb_net_descriptor_template[] = {
    TUD_RNDIS_DESCRIPTOR(0xFF, 0xFF, 0xFF, NET_EP_NOTIF_SIZE, 0xFF, 0xFF, NET_EP_SIZE)
};

#define NET_IAD_FIRST_INTERFACE_INDEX (2)
#define NET_CONTROL_INTERFACE_NUMBER_INDEX (8 + 2)
#define NET_CONTROL_INTERFACE_STRING_INDEX (8 + 8)
#define NET_CALL_MANAGEMENT_DATA_INDEX (22 + 4)
#define NET_UNION_MASTER_INDEX (31 + 3)
#define NET_UNION_SLAVE_INDEX (31 + 4)
#define NET_NOTIFICATION_ENDPOINT_INDEX (36 + 2)
#define NET_DATA_INTERFACE_NUMBER_INDEX (43 + 2)
#define NET_IN_ENDPOINT_INDEX (52 + 2)
#define NET_OUT_ENDPOINT_INDEX (59 + 2)

#else

// Every field the descriptor takes at run time is 0xFF here; the offsets
// below are counted through the parts the macro emits: 8 association, 9
// control interface, 5 header, 5 union, 13 ethernet, 6 ncm, 7 notification
// endpoint, 9 data interface, 9 data alternate, 7 in endpoint, 7 out
// endpoint, 85 bytes in all.
static const uint8_t usb_net_descriptor_template[] = {
    TUD_CDC_NCM_DESCRIPTOR(0xFF, 0xFF, 0xFF, 0xFF, NET_EP_NOTIF_SIZE, 0xFF, 0xFF, NET_EP_SIZE,
        NET_MAX_SEGMENT_SIZE, NET_EP_NOTIF_INTERVAL,
        (uint8_t)((uint8_t)NCM_NETWORK_CAPS_ETH_FILTER | (uint8_t)NCM_NETWORK_CAPS_NTB_INPUT_SIZE))
};

#define NET_IAD_FIRST_INTERFACE_INDEX (2)
#define NET_CONTROL_INTERFACE_NUMBER_INDEX (8 + 2)
#define NET_CONTROL_INTERFACE_STRING_INDEX (8 + 8)
#define NET_UNION_MASTER_INDEX (22 + 3)
#define NET_UNION_SLAVE_INDEX (22 + 4)
#define NET_MAC_STRING_INDEX (27 + 3)
#define NET_NOTIFICATION_ENDPOINT_INDEX (46 + 2)
#define NET_DATA_INTERFACE_NUMBER_INDEX (53 + 2)
#define NET_DATA_INTERFACE_ALT_NUMBER_INDEX (62 + 2)
#define NET_IN_ENDPOINT_INDEX (71 + 2)
#define NET_OUT_ENDPOINT_INDEX (78 + 2)

#endif

size_t usb_net_descriptor_length(void) {
    return sizeof(usb_net_descriptor_template);
}

static const char net_interface_name[] = USB_INTERFACE_NAME " Network";

size_t usb_net_add_descriptor(uint8_t *descriptor_buf, descriptor_counts_t *descriptor_counts,
    uint8_t *current_interface_string) {
    const usb_net_config_t *c = usb_net_config();

    memcpy(descriptor_buf, usb_net_descriptor_template, sizeof(usb_net_descriptor_template));

    const uint8_t control_interface = descriptor_counts->current_interface;
    descriptor_buf[NET_IAD_FIRST_INTERFACE_INDEX] = control_interface;
    descriptor_buf[NET_CONTROL_INTERFACE_NUMBER_INDEX] = control_interface;
    descriptor_buf[NET_UNION_MASTER_INDEX] = control_interface;
    descriptor_counts->current_interface++;

    const uint8_t data_interface = descriptor_counts->current_interface;
    descriptor_buf[NET_UNION_SLAVE_INDEX] = data_interface;
    descriptor_buf[NET_DATA_INTERFACE_NUMBER_INDEX] = data_interface;
    #if CIRCUITPY_USB_NET_RNDIS
    descriptor_buf[NET_CALL_MANAGEMENT_DATA_INDEX] = data_interface;
    #else
    descriptor_buf[NET_DATA_INTERFACE_ALT_NUMBER_INDEX] = data_interface;
    #endif
    descriptor_counts->current_interface++;

    // The notification endpoint is interrupt IN and takes a number of its own.
    descriptor_buf[NET_NOTIFICATION_ENDPOINT_INDEX] = 0x80 | descriptor_counts->current_endpoint;
    descriptor_counts->num_in_endpoints++;
    descriptor_counts->current_endpoint++;

    // The bulk pair shares one number, as CDC data does.
    descriptor_buf[NET_IN_ENDPOINT_INDEX] = 0x80 | descriptor_counts->current_endpoint;
    descriptor_counts->num_in_endpoints++;
    #ifdef TUD_ENDPOINT_ONE_DIRECTION_ONLY
    descriptor_counts->current_endpoint++;
    #endif
    descriptor_buf[NET_OUT_ENDPOINT_INDEX] = descriptor_counts->current_endpoint;
    descriptor_counts->num_out_endpoints++;
    descriptor_counts->current_endpoint++;

    usb_add_interface_string(*current_interface_string, net_interface_name);
    descriptor_buf[NET_CONTROL_INTERFACE_STRING_INDEX] = *current_interface_string;
    (*current_interface_string)++;

    #if CIRCUITPY_USB_NET_RNDIS
    memcpy(tud_network_mac_address, c->host_mac_address, sizeof(tud_network_mac_address));
    #else
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < sizeof(c->host_mac_address); i++) {
        mac_address_string[i * 2] = hex[c->host_mac_address[i] >> 4];
        mac_address_string[i * 2 + 1] = hex[c->host_mac_address[i] & 0xf];
    }
    mac_address_string[12] = '\0';
    usb_add_interface_string(*current_interface_string, mac_address_string);
    descriptor_buf[NET_MAC_STRING_INDEX] = *current_interface_string;
    (*current_interface_string)++;
    #endif

    return sizeof(usb_net_descriptor_template);
}

//--------------------------------------------------------------------+
// TinyUSB callbacks
//--------------------------------------------------------------------+

// A frame arrived from the host. src is ours only until we renew, and the
// port copies what it needs before returning.
bool tud_network_recv_cb(const uint8_t *src, uint16_t size) {
    if (size > 0) {
        usb_net_port_receive(src, size);
    }
    tud_network_recv_renew();
    return true;
}

// The class is ready to take the frame usb_net_send() offered.
uint16_t tud_network_xmit_cb(uint8_t *dst, void *ref, uint16_t arg) {
    (void)ref;
    (void)arg;
    if (pending_frame == NULL) {
        return 0;
    }
    memcpy(dst, pending_frame, pending_frame_len);
    return pending_frame_len;
}

// Only the ECM/RNDIS driver calls this, but it must exist to link.
void tud_network_init_cb(void) {
    pending_frame = NULL;
    pending_frame_len = 0;
}

bool usb_net_send(const uint8_t *frame, size_t len) {
    if (!tud_ready() || len == 0 || len > NET_MAX_SEGMENT_SIZE) {
        return false;
    }
    if (!tud_network_can_xmit(len)) {
        return false;
    }
    pending_frame = frame;
    pending_frame_len = (uint16_t)len;
    // tud_network_xmit() calls tud_network_xmit_cb() before it returns, so
    // the frame is copied out before the caller's buffer goes away.
    tud_network_xmit((void *)frame, 0);
    pending_frame = NULL;
    return true;
}

#endif // CIRCUITPY_USB_NET
