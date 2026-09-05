// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2021 Patrick Van Oosterwijck
// SPDX-FileCopyrightText: Copyright (c) 2022 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include "py/ringbuf.h"
#include "py/runtime.h"
#include "py/mphal.h"
#include "supervisor/port.h"
#include "supervisor/usb_serial_jtag.h"

#include "hal/usb_serial_jtag_ll.h"
#include "driver/gpio.h"
#include "esp_rom_gpio.h"
#include "hal/gpio_ll.h"
#include "esp_intr_alloc.h"
#include "esp_timer.h"
#include "esp_private/periph_ctrl.h"
#include "soc/periph_defs.h"

#define USB_SERIAL_JTAG_BUF_SIZE (64)

static ringbuf_t ringbuf;
static uint8_t buf[128];
static volatile bool connected;

#if CIRCUITPY_ESP_USB_SERIAL_JTAG && defined(SOC_WIFI_PHY_NEEDS_USB_WORKAROUND) && !defined(CONFIG_ESP_PHY_ENABLE_USB)
#error "CONFIG_ESP_PHY_ENABLE_USB must be enabled in sdkconfig"
#endif

// Make sure the recv interrupt is disabled during this. Otherwise, it could reorder data if it
// interrupts itself.
static void _copy_out_of_fifo(void) {
    uint8_t rx_buf[USB_SERIAL_JTAG_BUF_SIZE];

    // Take only what the ringbuf can hold, and leave the rest in the FIFO for
    // usb_serial_jtag_rx_tick() to pick up once the ringbuf has drained. Reading
    // more and dropping the excess keeps the endpoint moving, but it silently
    // loses bytes out of the middle of anything longer than the ringbuf -- which
    // is every file transfer over the raw REPL. Leaving them in the FIFO is only
    // safe because that poll exists; on the interrupt alone it deadlocks.
    size_t room = ringbuf_num_empty(&ringbuf);
    if (room > sizeof(rx_buf)) {
        room = sizeof(rx_buf);
    }
    size_t len = usb_serial_jtag_ll_read_rxfifo(rx_buf, room);

    for (size_t i = 0; i < len; ++i) {
        if (rx_buf[i] == mp_interrupt_char) {
            mp_sched_keyboard_interrupt();
            ringbuf_clear(&ringbuf);
        } else {
            ringbuf_put(&ringbuf, rx_buf[i]);
        }
    }
}

static void usb_serial_jtag_isr_handler(void *arg) {
    uint32_t flags = usb_serial_jtag_ll_get_intsts_mask();

    if (flags & USB_SERIAL_JTAG_INTR_SOF) {
        usb_serial_jtag_ll_clr_intsts_mask(USB_SERIAL_JTAG_INTR_SOF);
    }

    if (flags & USB_SERIAL_JTAG_INTR_TOKEN_REC_IN_EP1) {
        usb_serial_jtag_ll_clr_intsts_mask(USB_SERIAL_JTAG_INTR_TOKEN_REC_IN_EP1);
        connected = true;
        // CIRCUITPY-CHANGE: stop asking once the answer is known.
        //
        // This fires every time the host polls the IN endpoint, which for an
        // idle bulk endpoint is continuous: measured at roughly 20000 interrupts
        // a second against single digits that carried data. The only thing it
        // feeds is `connected`, so once that is set there is nothing further to
        // learn, and leaving it on buries the CPU.
        //
        // It was suspected of being what notices a host reopening the port, but
        // measurement says otherwise: with this line in or out, a reopened port
        // gets no response either way. That is a separate fault. This change is
        // neutral with respect to it and removes the storm, so it stays.
        usb_serial_jtag_ll_disable_intr_mask(USB_SERIAL_JTAG_INTR_TOKEN_REC_IN_EP1);
    }

    if (flags & USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT) {
        // New bytes are in the FIFO. Read them and check for keyboard interrupt.
        usb_serial_jtag_ll_clr_intsts_mask(USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT);
        // This is executed at interrupt level, so we don't explicitly need to make it atomic.
        _copy_out_of_fifo();
        port_wake_main_task_from_isr();
    }
}

void usb_serial_jtag_init(void) {
    ringbuf_init(&ringbuf, buf, sizeof(buf));

    // CIRCUITPY-CHANGE: bring the controller up the way the IDF's own driver
    // does. On the parts this file has run on until now the peripheral was
    // already clocked and its PHY already configured, so skipping this was
    // harmless. The C5 is not: without it the host's writes are never drained,
    // which shows up as output working and input silently not.
    //
    // Guarded to the C5 only because the other users of this file -- C2, C3, C6,
    // C61, H2 -- are not here to test against, and they work as they are. It
    // most likely belongs on all of them.
    #if defined(CONFIG_IDF_TARGET_ESP32C5)
    // USJ_RCC_ATOMIC() is private to the IDF's driver; PERIPH_RCC_ATOMIC() is
    // the general form of the same guard.
    PERIPH_RCC_ATOMIC() {
        usb_serial_jtag_ll_enable_bus_clock(true);
    }
    // Not calling usb_serial_jtag_ll_phy_set_defaults() here on purpose. The IDF
    // driver does, but it starts from a controller nobody has spoken to yet; by
    // this point the host has already enumerated the device, and resetting the
    // PHY underneath that connection silences the outgoing direction.
    #endif
    // The IDF driver deliberately leaves the status bits alone, and dropping the
    // clear here did make the host's writes go through -- but it also stopped
    // anything coming back, because usb_serial_jtag_write() will not send until
    // the ISR has seen TOKEN_REC_IN_EP1 and set `connected`. Clearing stays;
    // what the C5 was missing is the clock and PHY setup above.
    usb_serial_jtag_ll_clr_intsts_mask(USB_SERIAL_JTAG_INTR_SOF | USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT | USB_SERIAL_JTAG_INTR_TOKEN_REC_IN_EP1);
    // CIRCUITPY-CHANGE: SOF is not enabled on this part. Nothing in this file
    // uses it -- the handler's only reaction is to clear it again -- and the
    // IDF's own driver never asks for it either.
    //
    // Turning it off was first tried as a fix for the interrupt storm on this
    // part and did not help: the rate stayed at about 20000 a second. That
    // turned out to be TOKEN_REC_IN_EP1, handled in the ISR. Dropping SOF is
    // kept anyway because asking for an interrupt nobody reads is pure cost.
    #if defined(CONFIG_IDF_TARGET_ESP32C5)
    usb_serial_jtag_ll_ena_intr_mask(USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT | USB_SERIAL_JTAG_INTR_TOKEN_REC_IN_EP1);
    #else
    usb_serial_jtag_ll_ena_intr_mask(USB_SERIAL_JTAG_INTR_SOF | USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT | USB_SERIAL_JTAG_INTR_TOKEN_REC_IN_EP1);
    #endif
    ESP_ERROR_CHECK(esp_intr_alloc(ETS_USB_SERIAL_JTAG_INTR_SOURCE, ESP_INTR_FLAG_LEVEL1,
        usb_serial_jtag_isr_handler, NULL, NULL));

}

// CIRCUITPY-CHANGE: poll the endpoint as well as waiting for its interrupt.
//
// SERIAL_OUT_RECV_PKT is raised by the arrival of a packet, not by the FIFO
// being non-empty, and the two are not simultaneous: the handler can clear the
// status and read the FIFO before the byte has surfaced in it. The byte is then
// sitting in the FIFO with no interrupt left pending, and since the endpoint
// stays occupied the host cannot send the next packet either -- so no further
// interrupt can ever arrive. Input is dead from then on, and because a CPU reset
// does not reset this peripheral, it survives reflashing.
//
// The other way out, usb_serial_jtag_read_char(), only runs for code that reads
// from the console; a code.py that just computes and prints never calls it.
//
// Measured on an ESP32-C5 in that state over JTAG: EP1_CONF = 0x06, so
// SERIAL_OUT_EP_DATA_AVAIL was set; INT_ENA = 0x104, so the receive interrupt
// was enabled; and INT_RAW = 0xf2fb, with bit 2 clear -- data waiting, interrupt
// armed, nothing left to trigger it. Reading the FIFO over JTAG returned a
// single 0x03, the ctrl-C that had been sent, and cleared the flag.
//
// One register read per call removes the dependency on the edge.
void usb_serial_jtag_rx_tick(void) {
    if (!usb_serial_jtag_ll_rxfifo_data_available()) {
        return;
    }
    usb_serial_jtag_ll_disable_intr_mask(USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT);
    _copy_out_of_fifo();
    usb_serial_jtag_ll_ena_intr_mask(USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT);
}

bool usb_serial_jtag_connected(void) {
    return connected;
}

char usb_serial_jtag_read_char(void) {
    uint32_t num_filled = ringbuf_num_filled(&ringbuf);

    if (num_filled == 0 && !usb_serial_jtag_ll_rxfifo_data_available()) {
        // CIRCUITPY-CHANGE: re-enable the receive interrupt before giving up.
        //
        // It gets switched off in _copy_out_of_fifo() whenever the ringbuf is
        // full, on the understanding that this function turns it back on once
        // the ringbuf has been drained. There is a second way to drain it
        // though: a ctrl-C in the interrupt handler calls ringbuf_clear(), which
        // empties it without going anywhere near that path. After that the
        // ringbuf is empty, the interrupt is still off, and this early return
        // used to leave it off -- so nothing ever accepted another packet and
        // every host write blocked forever. Sending ctrl-C is the first thing
        // ampy and most other tools do, which is why one command was enough.
        usb_serial_jtag_ll_ena_intr_mask(USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT);
        return -1;
    }
    char c = -1;

    if (num_filled > 0) {
        common_hal_mcu_disable_interrupts();
        c = ringbuf_get(&ringbuf);
        common_hal_mcu_enable_interrupts();

        num_filled--;
    }

    // Maybe re-enable the recv interrupt if we've emptied the ringbuf.
    if (num_filled == 0) {
        usb_serial_jtag_ll_disable_intr_mask(USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT);
        _copy_out_of_fifo();

        // May have only been ctrl-c.
        if (c == -1 && ringbuf_num_filled(&ringbuf) > 0) {
            c = ringbuf_get(&ringbuf);
        }
        usb_serial_jtag_ll_ena_intr_mask(USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT);
    }
    return c;
}

uint32_t usb_serial_jtag_bytes_available(void) {
    // Atomically get the number of bytes in the ringbuf plus what is not yet in the ringbuf.
    common_hal_mcu_disable_interrupts();
    const uint32_t count = ringbuf_num_filled(&ringbuf) + usb_serial_jtag_ll_rxfifo_data_available();
    common_hal_mcu_enable_interrupts();
    return count;
}

void usb_serial_jtag_write(const char *text, uint32_t length) {
    if (!usb_serial_jtag_connected()) {
        return;
    }
    size_t total_written = 0;
    while (total_written < length) {
        uint32_t start_time = supervisor_ticks_ms32();
        // Wait until we can write to the FIFO again. If it takes too long, then
        // assume we're disconnected.
        while (!usb_serial_jtag_ll_txfifo_writable()) {
            uint32_t now = supervisor_ticks_ms32();
            if (now - start_time > 200) {
                connected = false;
                // The host looks gone, so watch for it arriving again.
                usb_serial_jtag_ll_ena_intr_mask(USB_SERIAL_JTAG_INTR_TOKEN_REC_IN_EP1);
                return;
            }
        }
        total_written += usb_serial_jtag_ll_write_txfifo((const uint8_t *)(text + total_written), length - total_written);
        RUN_BACKGROUND_TASKS;
    }
    usb_serial_jtag_ll_txfifo_flush();
}
