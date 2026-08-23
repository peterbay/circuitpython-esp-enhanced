// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2021 microDev
//
// SPDX-License-Identifier: MIT

#include <string.h>

#include "freertos/projdefs.h"
#include "py/runtime.h"
#include "shared-bindings/busio/SPI.h"
#include "shared-bindings/microcontroller/Pin.h"

#include "esp_private/spi_common_internal.h"

#define SPI_MAX_DMA_BITS (SPI_MAX_DMA_LEN * 8)
#define MAX_SPI_TRANSACTIONS 10

static spi_device_handle_t spi_handle[SOC_SPI_PERIPH_NUM];

static bool spi_bus_is_free(spi_host_device_t host_id) {
    return spi_bus_get_attr(host_id) == NULL;
}

static void set_spi_config(busio_spi_obj_t *self,
    uint32_t baudrate, uint8_t polarity, uint8_t phase, uint8_t bits) {
    // 128 is a 50% duty cycle.
    const int closest_clock = spi_get_actual_clock(APB_CLK_FREQ, baudrate, 128);
    const spi_device_interface_config_t device_config = {
        .clock_speed_hz = closest_clock,
        .mode = phase | (polarity << 1),
        .spics_io_num = -1, // No CS pin
        .queue_size = MAX_SPI_TRANSACTIONS,
        .pre_cb = NULL
    };
    esp_err_t result = spi_bus_add_device(self->host_id, &device_config, &spi_handle[self->host_id]);
    if (result != ESP_OK) {
        // ESP-IDF does not write the handle on failure, so it still holds whatever
        // configure() just removed. Nothing may transmit through that.
        spi_handle[self->host_id] = NULL;
        self->baudrate = 0;
        return result;
    }
    // CIRCUITPY-CHANGE: this used to store the achieved clock, which is what
    // configure() then compared the caller's requested rate against. Any rate that
    // does not come back unchanged -- 24, 30, 12, 15 MHz are the usual displayio
    // choices -- therefore never matched, so the device was removed and re-added on
    // every single transaction. With MAX_SPI_TRANSACTIONS raised to 20 in this fork
    // that is two FreeRTOS queues of 20 plus a device allocation each time, on a
    // board with no PSRAM. Remember what was asked for; the achieved clock is
    // reported separately.
    self->baudrate = baudrate;
    self->achieved_baudrate = closest_clock;
    self->polarity = polarity;
    self->phase = phase;
    self->bits = bits;
    return ESP_OK;
}

void common_hal_busio_spi_construct(busio_spi_obj_t *self,
    const mcu_pin_obj_t *clock, const mcu_pin_obj_t *mosi,
    const mcu_pin_obj_t *miso, bool half_duplex) {

    // Ensure the object starts in its deinit state.
    common_hal_busio_spi_mark_deinit(self);

    const spi_bus_config_t bus_config = {
        .mosi_io_num = mosi != NULL ? mosi->number : -1,
        .miso_io_num = miso != NULL ? miso->number : -1,
        .sclk_io_num = clock != NULL ? clock->number : -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };

    if (half_duplex) {
        mp_raise_NotImplementedError_varg(MP_ERROR_TEXT("%q"), MP_QSTR_half_duplex);
    }

    for (spi_host_device_t host_id = SPI2_HOST; host_id < SOC_SPI_PERIPH_NUM; host_id++) {
        if (spi_bus_is_free(host_id)) {
            self->host_id = host_id;
        }
    }

    if (self->host_id == 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("All SPI peripherals are in use"));
    }

    esp_err_t result = spi_bus_initialize(self->host_id, &bus_config, SPI_DMA_CH_AUTO);
    if (result == ESP_ERR_NO_MEM) {
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("ESP-IDF memory allocation failed"));
    } else if (result == ESP_ERR_INVALID_ARG) {
        raise_ValueError_invalid_pins();
    }

    self->mutex = xSemaphoreCreateMutex();
    if (self->mutex == NULL) {
        spi_bus_free(self->host_id);
        mp_raise_RuntimeError(MP_ERROR_TEXT("Unable to create lock"));
    }

    if (set_spi_config(self, 250000, 0, 0, 8) != ESP_OK) {
        vSemaphoreDelete(self->mutex);
        self->mutex = NULL;
        spi_bus_free(self->host_id);
        mp_raise_RuntimeError(MP_ERROR_TEXT("SPI configuration failed"));
    }

    self->MOSI = mosi;
    self->MISO = miso;
    self->clock = clock;

    if (mosi != NULL) {
        claim_pin(mosi);
    }
    if (miso != NULL) {
        claim_pin(miso);
    }
    claim_pin(clock);
}

void common_hal_busio_spi_never_reset(busio_spi_obj_t *self) {
    common_hal_never_reset_pin(self->clock);
    if (self->MOSI != NULL) {
        common_hal_never_reset_pin(self->MOSI);
    }
    if (self->MISO != NULL) {
        common_hal_never_reset_pin(self->MISO);
    }
}

bool common_hal_busio_spi_deinited(busio_spi_obj_t *self) {
    return self->clock == NULL;
}

void common_hal_busio_spi_mark_deinit(busio_spi_obj_t *self) {
    self->clock = NULL;
}

void common_hal_busio_spi_deinit(busio_spi_obj_t *self) {
    if (common_hal_busio_spi_deinited(self)) {
        return;
    }

    // Wait for any other users of this to finish.
    while (!common_hal_busio_spi_try_lock(self)) {
        RUN_BACKGROUND_TASKS;
    }

    // Mark as deinit early in case we are used in an interrupt.
    common_hal_reset_pin(self->clock);
    common_hal_busio_spi_mark_deinit(self);

    spi_bus_remove_device(spi_handle[self->host_id]);
    spi_bus_free(self->host_id);

    // Release the mutex before we delete it. Otherwise FreeRTOS gets unhappy.
    xSemaphoreGive(self->mutex);
    vSemaphoreDelete(self->mutex);
    self->mutex = NULL;

    common_hal_reset_pin(self->MOSI);
    common_hal_reset_pin(self->MISO);
}

bool common_hal_busio_spi_configure(busio_spi_obj_t *self,
    uint32_t baudrate, uint8_t polarity, uint8_t phase, uint8_t bits) {
    if (baudrate == self->baudrate &&
        polarity == self->polarity &&
        phase == self->phase &&
        bits == self->bits) {
        return true;
    }
    // Invalidate before removing: if the re-add below fails there must be no way
    // back to the freed device, and no stored rate that a later configure() with
    // the same request could match and then transmit through it.
    spi_device_handle_t old_handle = spi_handle[self->host_id];
    spi_handle[self->host_id] = NULL;
    self->baudrate = 0;
    spi_bus_remove_device(old_handle);
    return set_spi_config(self, baudrate, polarity, phase, bits) == ESP_OK;
}

// Wait as long as needed for the lock. This is used by SD card access from USB.
// Overrides the default busy-wait implementation in shared-bindings/busio/SPI.c
bool common_hal_busio_spi_wait_for_lock(busio_spi_obj_t *self, uint32_t timeout_ms) {
    if (common_hal_busio_spi_deinited(self)) {
        return false;
    }
    return xSemaphoreTake(self->mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

bool common_hal_busio_spi_try_lock(busio_spi_obj_t *self) {
    return common_hal_busio_spi_wait_for_lock(self, 0);
}

bool common_hal_busio_spi_has_lock(busio_spi_obj_t *self) {
    return (self->mutex != NULL) && (xSemaphoreGetMutexHolder(self->mutex) == xTaskGetCurrentTaskHandle());
}

void common_hal_busio_spi_unlock(busio_spi_obj_t *self) {
    if (self->mutex != NULL) {
        xSemaphoreGive(self->mutex);
    }
}

bool common_hal_busio_spi_write(busio_spi_obj_t *self,
    const uint8_t *data, size_t len) {
    if (self->MOSI == NULL) {
        mp_raise_ValueError_varg(MP_ERROR_TEXT("No %q pin"), MP_QSTR_mosi);
    }
    return common_hal_busio_spi_transfer(self, data, NULL, len);
}

bool common_hal_busio_spi_read(busio_spi_obj_t *self,
    uint8_t *data, size_t len, uint8_t write_value) {
    if (self->MISO == NULL) {
        mp_raise_ValueError_varg(MP_ERROR_TEXT("No %q pin"), MP_QSTR_miso);
    }
    if (self->MOSI == NULL) {
        return common_hal_busio_spi_transfer(self, NULL, data, len);
    } else {
        memset(data, write_value, len);
        return common_hal_busio_spi_transfer(self, data, data, len);
    }
}

bool common_hal_busio_spi_transfer(busio_spi_obj_t *self,
    const uint8_t *data_out, uint8_t *data_in, size_t len) {
    if (len == 0) {
        return true;
    }
    if (self->MOSI == NULL && data_out != NULL) {
        mp_raise_ValueError_varg(MP_ERROR_TEXT("No %q pin"), MP_QSTR_mosi);
    }
    if (self->MISO == NULL && data_in != NULL) {
        mp_raise_ValueError_varg(MP_ERROR_TEXT("No %q pin"), MP_QSTR_miso);
    }

    spi_transaction_t transactions[MAX_SPI_TRANSACTIONS];

    // Round to nearest whole set of bits
    int bits_to_send = len * 8 / self->bits * self->bits;

    if (len <= 4) {
        memset(&transactions[0], 0, sizeof(spi_transaction_t));
        if (data_out != NULL) {
            memcpy(&transactions[0].tx_data, data_out, len);
        }

        transactions[0].flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;
        transactions[0].length = bits_to_send;
        // Poll instead of queueing: at four bytes or fewer the transfer itself is
        // under half a microsecond, while going through the queue costs an
        // interrupt and two context switches. Measured on a display bus, where
        // setting the address window is four such transfers, this is the
        // difference between 255 us and the time the bytes actually take.
        // Safe only because the queued path below always drains before returning;
        // ESP-IDF forbids mixing polling with transactions still in the queue.
        esp_err_t result = spi_device_polling_transmit(spi_handle[self->host_id], &transactions[0]);
        if (result != ESP_OK) {
            return false;
        }

        if (data_in != NULL) {
            memcpy(data_in, &transactions[0].rx_data, len);
        }
    } else {
        int offset = 0;
        int bits_remaining = bits_to_send;
        int cur_trans = 0;

        // CIRCUITPY-CHANGE: this loop also tested mp_hal_is_interrupted() and then fell
        // through to the "return true" below with bits_remaining still set, so any
        // pending exception -- Ctrl-C, but also the reload exception reload_initiate()
        // posts the moment a file lands on CIRCUITPY -- turned a transfer bigger than one
        // queue batch into a short write reported as success. Returning false would not
        // be enough: fourwire's send() is void, sdcardio discards the result of its block
        // write, and BusDisplay now writes one window as chunks joined by
        // MIPI_COMMAND_WRITE_MEMORY_CONTINUE, so the bytes that were dropped are not a
        // gap -- every chunk after them lands at the wrong offset in the controller's
        // RAM. Always shift out what was asked for. RUN_BACKGROUND_TASKS still runs while
        // draining each batch, and the pending exception is taken as soon as we return.
        while (bits_remaining) {

            cur_trans = 0;
            while (bits_remaining && (cur_trans != MAX_SPI_TRANSACTIONS)) {
                memset(&transactions[cur_trans], 0, sizeof(spi_transaction_t));

                transactions[cur_trans].length =
                    bits_remaining > SPI_MAX_DMA_BITS ? SPI_MAX_DMA_BITS : bits_remaining;

                if (data_out != NULL) {
                    transactions[cur_trans].tx_buffer = data_out + offset;
                }
                if (data_in != NULL) {
                    transactions[cur_trans].rx_buffer = data_in + offset;
                }

                bits_remaining -= transactions[cur_trans].length;

                // doesn't need ceil(); loop ends when bits_remaining is 0
                offset += transactions[cur_trans].length / 8;
                cur_trans++;
            }

            // CIRCUITPY-CHANGE: spi_device_queue_trans can refuse the transaction
            // without enqueueing it, most plausibly with ESP_ERR_NO_MEM when the
            // driver needs a DMA bounce buffer and the internal heap is under
            // pressure from WiFi or BLE. The result of that call used to be
            // discarded and the drain loop below still waited for cur_trans
            // results, so one refusal blocked the board forever in
            // spi_device_get_trans_result with the bus mutex held: no traceback,
            // no Ctrl-C, reset only. Count what actually made it into the queue,
            // drain exactly that many -- leaving one behind would break the
            // polling path of the next transfer -- and then report the failure.
            int queued = 0;
            esp_err_t queue_result = ESP_OK;
            for (int i = 0; i < cur_trans; i++) {
                queue_result = spi_device_queue_trans(spi_handle[self->host_id], &transactions[i], portMAX_DELAY);
                if (queue_result != ESP_OK) {
                    break;
                }
                queued++;
            }

            spi_transaction_t *rtrans;
            for (int x = 0; x < queued; x++) {
                RUN_BACKGROUND_TASKS;
                spi_device_get_trans_result(spi_handle[self->host_id], &rtrans, portMAX_DELAY);
            }

            if (queue_result != ESP_OK) {
                return false;
            }
        }
    }
    return true;
}

uint32_t common_hal_busio_spi_get_frequency(busio_spi_obj_t *self) {
    // The property is documented as the actual bus frequency, not the requested one.
    return self->achieved_baudrate;
}

uint8_t common_hal_busio_spi_get_polarity(busio_spi_obj_t *self) {
    return self->polarity;
}

uint8_t common_hal_busio_spi_get_phase(busio_spi_obj_t *self) {
    return self->phase;
}
