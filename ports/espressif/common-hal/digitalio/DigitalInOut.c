// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2017-2020 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include "shared-bindings/digitalio/DigitalInOut.h"
#include "py/runtime.h"

#include "driver/gpio.h"
#include "hal/gpio_hal.h"

// CIRCUITPY-CHANGE: this used to call gpio_get_io_config(), which fills a whole
// gpio_io_config_t -- pulls, output enable, open drain, drive strength, function
// select, sleep mode -- and on a pin in the RTC range makes three further HAL
// calls on top, all so that one bit of it could be read. get_value() asks on
// every single read, and measured against the same property read without it, that
// cost 1505 ns a time. The input enable bit is one field of one IO MUX register,
// which is what gpio_ll_get_io_config() reads it from as well.
// The C5's soc component does not export GPIO_PIN_MUX_REG at all -- its
// gpio_periph.c defines only GPIO_HOLD_MASK -- so the cheap path above has
// nothing to index. It pays the full gpio_get_io_config() cost until someone
// works out the IO MUX register address from the pin number on that part.
#if defined(CONFIG_IDF_TARGET_ESP32C5)
static bool _pin_is_input(uint8_t pin_number) {
    gpio_io_config_t config;
    if (gpio_get_io_config(pin_number, &config) != ESP_OK) {
        return false;
    }
    return config.ie;
}
#else
static bool _pin_is_input(uint8_t pin_number) {
    return (REG_READ(GPIO_PIN_MUX_REG[pin_number]) & FUN_IE_M) != 0;
}
#endif

// Same reasoning for the open drain bit, which is one field of one GPIO register.
static bool _pin_is_open_drain(uint8_t pin_number) {
    return GPIO.pin[pin_number].pad_driver != 0;
}

void digitalio_digitalinout_preserve_for_deep_sleep(size_t n_dios, digitalio_digitalinout_obj_t *preserve_dios[]) {
    // Mark the pin states of the given DigitalInOuts for preservation during deep sleep
    for (size_t i = 0; i < n_dios; i++) {
        if (!common_hal_digitalio_digitalinout_deinited(preserve_dios[i])) {
            preserve_pin_number(preserve_dios[i]->pin->number);
        }
    }
}

void common_hal_digitalio_digitalinout_never_reset(
    digitalio_digitalinout_obj_t *self) {
    never_reset_pin_number(self->pin->number);
}

digitalinout_result_t common_hal_digitalio_digitalinout_construct(
    digitalio_digitalinout_obj_t *self, const mcu_pin_obj_t *pin) {
    claim_pin(pin);
    self->pin = pin;

    gpio_config_t config;
    config.pin_bit_mask = 1ull << pin->number;
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    if (gpio_config(&config) != ESP_OK) {
        return DIGITALINOUT_PIN_BUSY;
    }

    return DIGITALINOUT_OK;
}

bool common_hal_digitalio_digitalinout_deinited(digitalio_digitalinout_obj_t *self) {
    return self->pin == NULL;
}

void common_hal_digitalio_digitalinout_deinit(digitalio_digitalinout_obj_t *self) {
    if (common_hal_digitalio_digitalinout_deinited(self)) {
        return;
    }

    reset_pin_number(self->pin->number);
    self->pin = NULL;
}

digitalinout_result_t common_hal_digitalio_digitalinout_switch_to_input(
    digitalio_digitalinout_obj_t *self, digitalio_pull_t pull) {
    common_hal_digitalio_digitalinout_set_pull(self, pull);
    gpio_set_direction(self->pin->number, GPIO_MODE_INPUT);
    return DIGITALINOUT_OK;
}

digitalinout_result_t common_hal_digitalio_digitalinout_switch_to_output(
    digitalio_digitalinout_obj_t *self, bool value,
    digitalio_drive_mode_t drive_mode) {
    common_hal_digitalio_digitalinout_set_value(self, value);
    return common_hal_digitalio_digitalinout_set_drive_mode(self, drive_mode);
}

digitalio_direction_t common_hal_digitalio_digitalinout_get_direction(
    digitalio_digitalinout_obj_t *self) {
    if (_pin_is_input(self->pin->number)) {
        return DIRECTION_INPUT;
    }
    return DIRECTION_OUTPUT;
}

void common_hal_digitalio_digitalinout_set_value(
    digitalio_digitalinout_obj_t *self, bool value) {
    self->output_value = value;
    gpio_set_level(self->pin->number, value);
}

bool common_hal_digitalio_digitalinout_get_value(
    digitalio_digitalinout_obj_t *self) {
    if (common_hal_digitalio_digitalinout_get_direction(self) == DIRECTION_INPUT) {
        return gpio_get_level(self->pin->number) == 1;
    }
    return self->output_value;
}

digitalinout_result_t common_hal_digitalio_digitalinout_set_drive_mode(
    digitalio_digitalinout_obj_t *self,
    digitalio_drive_mode_t drive_mode) {
    gpio_num_t number = self->pin->number;
    gpio_mode_t mode = GPIO_MODE_OUTPUT;
    if (drive_mode == DRIVE_MODE_OPEN_DRAIN) {
        mode |= GPIO_MODE_OUTPUT_OD;
    }
    esp_err_t result = gpio_set_direction(number, mode);
    if (result != ESP_OK) {
        return DIGITALINOUT_INPUT_ONLY;
    }
    return DIGITALINOUT_OK;
}

digitalio_drive_mode_t common_hal_digitalio_digitalinout_get_drive_mode(
    digitalio_digitalinout_obj_t *self) {
    return _pin_is_open_drain(self->pin->number) ? DRIVE_MODE_OPEN_DRAIN : DRIVE_MODE_PUSH_PULL;
}

digitalinout_result_t common_hal_digitalio_digitalinout_set_pull(
    digitalio_digitalinout_obj_t *self, digitalio_pull_t pull) {
    gpio_num_t number = self->pin->number;
    gpio_pullup_dis(number);
    gpio_pulldown_dis(number);
    if (pull == PULL_UP) {
        gpio_pullup_en(number);
    } else if (pull == PULL_DOWN) {
        gpio_pulldown_en(number);
    }
    return DIGITALINOUT_OK;
}

digitalio_pull_t common_hal_digitalio_digitalinout_get_pull(
    digitalio_digitalinout_obj_t *self) {
    gpio_io_config_t config;
    if (gpio_get_io_config((gpio_num_t)self->pin->number, &config) != ESP_OK) {
        // Should it fail closed or open?
        return PULL_NONE;
    }
    // CIRCUITPY-CHANGE: this read config.pu and then ignored it, returning PULL_UP
    // for anything that was not pulled down -- so a pin with no pull at all, which
    // is what construct() leaves and what switch_to_input(PULL_NONE) sets, reported
    // PULL_UP. PULL_NONE was only ever returned when the HAL call itself failed.
    if (config.pu) {
        return PULL_UP;
    }
    if (config.pd) {
        return PULL_DOWN;
    }
    return PULL_NONE;
}
