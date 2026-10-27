// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Petr Vavrin
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "host/ble_gap.h"
#include "shared-bindings/_bleio/ScanResults.h"

// Extended advertising reports for Adapter.start_scan(). See extended_scan.c.

// Called on the Python thread before a scan starts. Raises if the reassembly
// buffer cannot be allocated.
void bleio_extended_scan_prepare(bool extended);

// Starts an extended scan on the 1M and the Coded PHY.
int bleio_extended_scan_start(uint8_t own_addr_type, int32_t duration_ms,
    const struct ble_gap_disc_params *params, ble_gap_event_fn *cb, void *cb_arg);

// Called from the NimBLE host task for every BLE_GAP_EVENT_EXT_DISC.
void bleio_extended_scan_report(bleio_scanresults_obj_t *results, const struct ble_gap_ext_disc_desc *desc);
