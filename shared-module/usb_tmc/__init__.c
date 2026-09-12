// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-License-Identifier: MIT

// CIRCUITPY-CHANGE: see shared-module/usb_tmc/__init__.h.

#include <string.h>

#include "shared-bindings/microcontroller/__init__.h"
#include "shared-module/usb_tmc/__init__.h"
#include "supervisor/usb.h"

#include "tusb.h"

// The TinyUSB callbacks below run in whatever context pumps tud_task(), a
// separate task on Espressif, while Python reads and writes from the VM. Both
// sides bracket their changes to the queues in a no-interrupt section, which
// on the one core the two tasks share is enough to keep them apart.

static bool usb_tmc_is_enabled = CIRCUITPY_USB_TMC_ENABLED_DEFAULT;

// Commands from the host, as a ring of bytes: a two-byte length, then the
// message. rx_head is where the message being received grows, rx_committed
// where the last complete one ended, rx_tail the next one for Python. A
// message is not visible to Python until its EOM has arrived.
static uint8_t rx_ring[USB_TMC_BUFFER_SIZE];
static size_t rx_head;
static size_t rx_committed;
static size_t rx_tail;
static size_t rx_count;             // bytes from rx_tail to rx_committed
static size_t rx_message_start;     // offset of the length prefix being filled
static bool rx_in_message;
// The bulk OUT endpoint is left NAKing while the ring is nearly full, so the
// host waits instead of a command being dropped. Set when the read was not
// restarted for that reason.
static bool rx_read_held;

// The response, kept whole because TinyUSB sends straight from this buffer.
static uint8_t tx_buf[USB_TMC_BUFFER_SIZE];
static size_t tx_len;
static size_t tx_sent;              // bytes handed to TinyUSB so far
static bool tx_queued;
static bool tx_in_flight;
// A request for a response the host sent before Python had one ready.
static bool in_requested;
static uint32_t in_request_size;

static uint8_t status_byte;

#define IEEE4882_STB_MAV (0x10u)

static const usbtmc_response_capabilities_488_t capabilities = {
    .USBTMC_status = USBTMC_STATUS_SUCCESS,
    .bcdUSBTMC = USBTMC_VERSION,
    .bmIntfcCapabilities = {
        .listenOnly = 0,
        .talkOnly = 0,
        .supportsIndicatorPulse = 0,
    },
    .bmDevCapabilities = {
        // TermChar handling would need every response to end in the
        // requested character; the host copes without it.
        .canEndBulkInOnTermChar = 0,
    },
    .bcdUSB488 = USBTMC_488_VERSION,
    .bmIntfcCapabilities488 = {
        .supportsTrigger = 0,
        .supportsREN_GTL_LLO = 0,
        // Full 488.2 and service requests both need the interrupt endpoint,
        // which would cost a second IN endpoint.
        .is488_2 = 0,
    },
    .bmDevCapabilities488 = {
        .DT1 = 0,
        .RL1 = 0,
        .SR1 = 0,
        .SCPI = 1,
    },
};

bool usb_tmc_enabled(void) {
    return usb_tmc_is_enabled;
}

bool common_hal_usb_tmc_enable(void) {
    if (tud_connected()) {
        return false;
    }
    usb_tmc_is_enabled = true;
    return true;
}

bool common_hal_usb_tmc_disable(void) {
    if (tud_connected()) {
        return false;
    }
    usb_tmc_is_enabled = false;
    return true;
}

// One interface with a bulk endpoint each way, on one endpoint number. No
// interrupt endpoint: see the capabilities above.
static const uint8_t usb_tmc_descriptor_template[] = {
    TUD_USBTMC_IF_DESCRIPTOR(/*_itfnum*/ 0xFF, /*_bNumEndpoints*/ 2, /*_stridx*/ 0xFF, TUD_USBTMC_PROTOCOL_USB488),
    TUD_USBTMC_BULK_DESCRIPTORS(/*_epout*/ 0xFF, /*_epin*/ 0xFF, /*_bulk_epsize*/ 64),
};
#define TMC_INTERFACE_INDEX (2)
#define TMC_INTERFACE_STRING_INDEX (8)
#define TMC_OUT_ENDPOINT_INDEX (9 + 2)
#define TMC_IN_ENDPOINT_INDEX (9 + 7 + 2)

static const char usb_tmc_interface_name[] = USB_INTERFACE_NAME " USBTMC";

size_t usb_tmc_descriptor_length(void) {
    return sizeof(usb_tmc_descriptor_template);
}

size_t usb_tmc_add_descriptor(uint8_t *descriptor_buf, descriptor_counts_t *descriptor_counts,
    uint8_t *current_interface_string) {
    memcpy(descriptor_buf, usb_tmc_descriptor_template, sizeof(usb_tmc_descriptor_template));

    descriptor_buf[TMC_INTERFACE_INDEX] = descriptor_counts->current_interface;
    descriptor_counts->current_interface++;

    descriptor_buf[TMC_IN_ENDPOINT_INDEX] = 0x80 | descriptor_counts->current_endpoint;
    descriptor_counts->num_in_endpoints++;
    // Some TinyUSB devices have issues with bi-directional endpoints
    #ifdef TUD_ENDPOINT_ONE_DIRECTION_ONLY
    descriptor_counts->current_endpoint++;
    #endif
    descriptor_buf[TMC_OUT_ENDPOINT_INDEX] = descriptor_counts->current_endpoint;
    descriptor_counts->num_out_endpoints++;
    descriptor_counts->current_endpoint++;

    usb_add_interface_string(*current_interface_string, usb_tmc_interface_name);
    descriptor_buf[TMC_INTERFACE_STRING_INDEX] = *current_interface_string;
    (*current_interface_string)++;

    return sizeof(usb_tmc_descriptor_template);
}

// --------------------------------------------------------------------+
// Queues
// --------------------------------------------------------------------+

static size_t rx_free(void) {
    // One byte is kept unused so full and empty tell apart.
    return USB_TMC_BUFFER_SIZE - 1 - ((rx_head + USB_TMC_BUFFER_SIZE - rx_tail) % USB_TMC_BUFFER_SIZE);
}

static void rx_put(const uint8_t *data, size_t len) {
    size_t first = USB_TMC_BUFFER_SIZE - rx_head;
    if (first > len) {
        first = len;
    }
    memcpy(&rx_ring[rx_head], data, first);
    memcpy(&rx_ring[0], data + first, len - first);
    rx_head = (rx_head + len) % USB_TMC_BUFFER_SIZE;
}

static void rx_get(size_t offset, uint8_t *buf, size_t len) {
    size_t first = USB_TMC_BUFFER_SIZE - offset;
    if (first > len) {
        first = len;
    }
    memcpy(buf, &rx_ring[offset], first);
    memcpy(buf + first, &rx_ring[0], len - first);
}

static void rx_reset(void) {
    rx_head = rx_committed = rx_tail = 0;
    rx_count = 0;
    rx_in_message = false;
    rx_read_held = false;
}

static void tx_reset(void) {
    tx_len = tx_sent = 0;
    tx_queued = tx_in_flight = false;
    in_requested = false;
}

// A command is worth reading once a header and a short line fit; the host
// waits on the endpoint for the rest.
#define RX_RESTART_MIN (2 + 64)

static void rx_restart_read(void) {
    if (rx_free() >= RX_RESTART_MIN) {
        rx_read_held = false;
        tud_usbtmc_start_bus_read();
    } else {
        rx_read_held = true;
    }
}

size_t usb_tmc_message_available(void) {
    common_hal_mcu_disable_interrupts();
    size_t len = 0;
    if (rx_count > 0) {
        uint8_t prefix[2];
        rx_get(rx_tail, prefix, 2);
        len = prefix[0] | (prefix[1] << 8);
    }
    common_hal_mcu_enable_interrupts();
    return len;
}

size_t usb_tmc_read_message(uint8_t *buf, size_t len) {
    common_hal_mcu_disable_interrupts();
    size_t message_len = 0;
    if (rx_count > 0) {
        uint8_t prefix[2];
        rx_get(rx_tail, prefix, 2);
        message_len = prefix[0] | (prefix[1] << 8);
        if (message_len <= len) {
            rx_get((rx_tail + 2) % USB_TMC_BUFFER_SIZE, buf, message_len);
            rx_tail = (rx_tail + 2 + message_len) % USB_TMC_BUFFER_SIZE;
            rx_count -= 2 + message_len;
        } else {
            message_len = 0;
        }
    }
    bool restart = rx_read_held;
    common_hal_mcu_enable_interrupts();
    if (restart) {
        rx_restart_read();
    }
    return message_len;
}

bool usb_tmc_response_pending(void) {
    return tx_queued || tx_in_flight;
}

// Sends the next piece of the queued response, as much as the host asked
// for. Called with a response queued and a request from the host in hand.
static void tx_send_chunk(void) {
    size_t remaining = tx_len - tx_sent;
    size_t chunk = remaining < in_request_size ? remaining : in_request_size;
    bool last = (tx_sent + chunk) == tx_len;
    in_requested = false;
    tx_in_flight = true;
    if (!tud_usbtmc_transmit_dev_msg_data(&tx_buf[tx_sent], chunk, last, false)) {
        // The class was not in the requesting state after all; the host will
        // ask again or abort, and the response stays queued for that.
        tx_in_flight = false;
        return;
    }
    tx_sent += chunk;
}

void usb_tmc_write_message(const uint8_t *data, size_t len) {
    memcpy(tx_buf, data, len);
    common_hal_mcu_disable_interrupts();
    tx_len = len;
    tx_sent = 0;
    tx_queued = true;
    bool send_now = in_requested;
    common_hal_mcu_enable_interrupts();
    if (send_now) {
        tx_send_chunk();
    }
}

uint8_t usb_tmc_get_status_byte(void) {
    return status_byte | (usb_tmc_response_pending() ? IEEE4882_STB_MAV : 0);
}

void usb_tmc_set_status_byte(uint8_t value) {
    status_byte = value & (uint8_t) ~IEEE4882_STB_MAV;
}

// --------------------------------------------------------------------+
// TinyUSB USBTMC class callbacks
// --------------------------------------------------------------------+

usbtmc_response_capabilities_488_t const *tud_usbtmc_get_capabilities_cb(void) {
    return &capabilities;
}

void tud_usbtmc_open_cb(uint8_t interface_id) {
    (void)interface_id;
    common_hal_mcu_disable_interrupts();
    rx_reset();
    tx_reset();
    common_hal_mcu_enable_interrupts();
    tud_usbtmc_start_bus_read();
}

bool tud_usbtmc_msgBulkOut_start_cb(usbtmc_msg_request_dev_dep_out const *msgHeader) {
    common_hal_mcu_disable_interrupts();
    bool ok;
    if (!rx_in_message) {
        // Reserve the length prefix; it is filled in at the end of the message.
        ok = msgHeader->TransferSize + 2 <= rx_free();
        if (ok) {
            rx_message_start = rx_head;
            uint8_t prefix[2] = { 0, 0 };
            rx_put(prefix, 2);
            rx_in_message = true;
        }
    } else {
        // A continuation of a message whose earlier transfer had no EOM.
        ok = msgHeader->TransferSize <= rx_free();
    }
    common_hal_mcu_enable_interrupts();
    // Refusing halts the endpoint, which the host reports as an error: the
    // message could never have fit.
    return ok;
}

bool tud_usbtmc_msg_data_cb(void *data, size_t len, bool transfer_complete) {
    common_hal_mcu_disable_interrupts();
    if (rx_in_message && len <= rx_free()) {
        rx_put(data, len);
    }
    if (transfer_complete) {
        // TinyUSB does not pass the EOM bit on; a transfer that does not end
        // the message is rare in practice (VISA sends whole lines) and would
        // only split the message in two for Python.
        size_t message_len = (rx_head + USB_TMC_BUFFER_SIZE - rx_message_start - 2) % USB_TMC_BUFFER_SIZE;
        rx_ring[rx_message_start] = message_len & 0xff;
        rx_ring[(rx_message_start + 1) % USB_TMC_BUFFER_SIZE] = message_len >> 8;
        rx_committed = rx_head;
        rx_count += 2 + message_len;
        rx_in_message = false;
    }
    common_hal_mcu_enable_interrupts();
    rx_restart_read();
    return true;
}

void tud_usbtmc_bulkOut_clearFeature_cb(void) {
    common_hal_mcu_disable_interrupts();
    // Whatever was half received is gone with the halt.
    if (rx_in_message) {
        rx_head = rx_committed;
        rx_in_message = false;
    }
    common_hal_mcu_enable_interrupts();
    rx_restart_read();
}

bool tud_usbtmc_msgBulkIn_request_cb(usbtmc_msg_request_dev_dep_in const *request) {
    common_hal_mcu_disable_interrupts();
    in_requested = true;
    in_request_size = request->TransferSize;
    bool have_response = tx_queued;
    common_hal_mcu_enable_interrupts();
    if (have_response) {
        tx_send_chunk();
    }
    // Otherwise the endpoint NAKs until Python writes a response, as the
    // specification asks for a request that arrives before the answer exists.
    return true;
}

bool tud_usbtmc_msgBulkIn_complete_cb(void) {
    common_hal_mcu_disable_interrupts();
    tx_in_flight = false;
    if (tx_sent >= tx_len) {
        tx_queued = false;
        tx_len = tx_sent = 0;
    }
    common_hal_mcu_enable_interrupts();
    tud_usbtmc_start_bus_read();
    return true;
}

void tud_usbtmc_bulkIn_clearFeature_cb(void) {
    common_hal_mcu_disable_interrupts();
    tx_reset();
    common_hal_mcu_enable_interrupts();
}

bool tud_usbtmc_initiate_abort_bulk_in_cb(uint8_t *tmcResult) {
    common_hal_mcu_disable_interrupts();
    tx_reset();
    common_hal_mcu_enable_interrupts();
    *tmcResult = USBTMC_STATUS_SUCCESS;
    return true;
}

bool tud_usbtmc_check_abort_bulk_in_cb(usbtmc_check_abort_bulk_rsp_t *rsp) {
    (void)rsp;
    tud_usbtmc_start_bus_read();
    return true;
}

bool tud_usbtmc_initiate_abort_bulk_out_cb(uint8_t *tmcResult) {
    common_hal_mcu_disable_interrupts();
    if (rx_in_message) {
        rx_head = rx_committed;
        rx_in_message = false;
    }
    common_hal_mcu_enable_interrupts();
    *tmcResult = USBTMC_STATUS_SUCCESS;
    return true;
}

bool tud_usbtmc_check_abort_bulk_out_cb(usbtmc_check_abort_bulk_rsp_t *rsp) {
    (void)rsp;
    tud_usbtmc_start_bus_read();
    return true;
}

bool tud_usbtmc_initiate_clear_cb(uint8_t *tmcResult) {
    common_hal_mcu_disable_interrupts();
    rx_reset();
    tx_reset();
    common_hal_mcu_enable_interrupts();
    *tmcResult = USBTMC_STATUS_SUCCESS;
    return true;
}

bool tud_usbtmc_check_clear_cb(usbtmc_get_clear_status_rsp_t *rsp) {
    rsp->USBTMC_status = USBTMC_STATUS_SUCCESS;
    rsp->bmClear.BulkInFifoBytes = 0;
    // The class goes back to idle on this answer and waits for a read to be
    // started; nothing else does that here.
    tud_usbtmc_start_bus_read();
    return true;
}

uint8_t tud_usbtmc_get_stb_cb(uint8_t *tmcResult) {
    *tmcResult = USBTMC_STATUS_SUCCESS;
    return usb_tmc_get_status_byte();
}
