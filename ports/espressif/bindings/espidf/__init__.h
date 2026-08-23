// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2020 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#pragma once

#include "esp_err.h"
#include "py/mpconfig.h"
#include "py/obj.h"

#include "common-hal/espidf/__init__.h"

extern const mp_obj_type_t mp_type_espidf_IDFError;
extern const mp_obj_type_t mp_type_espidf_MemoryError;

MP_NORETURN void mp_raise_espidf_MemoryError(void);

void raise_esp_error(esp_err_t err) MP_NORETURN;
#define CHECK_ESP_RESULT(x) do { int res = (x); if (res != ESP_OK) raise_esp_error(res); } while (0)

size_t common_hal_espidf_get_total_psram(void);
intptr_t common_hal_espidf_get_psram_start(void);
intptr_t common_hal_espidf_get_psram_end(void);

// CIRCUITPY-CHANGE: esp_timer exposed to python. See __init__.c.
extern const mp_obj_type_t espidf_timer_type;
void espidf_timer_reset(void);
extern const mp_obj_type_t espidf_event_queue_type;
void espidf_event_reset(void);
#if CIRCUITPY_WIFI
void espidf_eap_reset(void);
void espidf_smartconfig_reset(void);
#endif

extern const mp_obj_type_t espidf_partition_type;
#if CIRCUITPY_ESPIDF_CSI
extern const mp_obj_type_t espidf_csi_type;
#endif
void espidf_csi_reset(void);

extern const mp_obj_type_t espidf_nvs_type;
