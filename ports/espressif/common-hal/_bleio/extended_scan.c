// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Petr Vavrin
//
// SPDX-License-Identifier: MIT

// Extended advertising reports for Adapter.start_scan().
//
// With BLE_EXT_ADV on, NimBLE turns every scan into an extended one and
// reports every advertisement, legacy ones included, as BLE_GAP_EVENT_EXT_DISC.
// An extended advertisement longer than one HCI event arrives in pieces: on the
// ESP32-C5, whose CONFIG_BT_NIMBLE_TRANSPORT_EVT_SIZE is 70, 42 bytes at a time,
// all but the last marked incomplete. Appending each piece as its own ScanEntry
// handed Python fragments that no parser could use. This puts them back
// together.
//
// One advertisement is reassembled at a time. Its pieces arrive back to back
// from the controller; if a report from another advertiser comes in between,
// the partial one is dropped rather than mixed with it.

#include <string.h>

#include "py/runtime.h"

#include "common-hal/_bleio/extended_scan.h"
#include "shared-module/_bleio/ScanResults.h"
#include "supervisor/shared/tick.h"

#include "esp_heap_caps.h"
#include "nimble/hci_common.h"

#if MYNEWT_VAL(BLE_EXT_ADV)

// The longest advertising data an extended advertisement can carry.
#define EXTENDED_ADV_MAX_DATA (1650)

static bool _extended;
static uint8_t *_data;
static uint16_t _length;
static bool _pending;
static ble_addr_t _address;
static uint8_t _sid;
static uint8_t _props;
static int8_t _rssi;

void bleio_extended_scan_prepare(bool extended) {
    _extended = extended;
    _pending = false;
    if (extended && _data == NULL) {
        // Kept for the rest of the session: the host task may still be in the
        // callback when a scan is cancelled. PSRAM where there is some, since
        // internal RAM is what the radio drivers run short of.
        _data = heap_caps_malloc_prefer(EXTENDED_ADV_MAX_DATA, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_8BIT);
        if (_data == NULL) {
            m_malloc_fail(EXTENDED_ADV_MAX_DATA);
        }
    }
}

int bleio_extended_scan_start(uint8_t own_addr_type, int32_t duration_ms,
    const struct ble_gap_disc_params *params, ble_gap_event_fn *cb, void *cb_arg) {
    const struct ble_gap_ext_disc_params phy_params = {
        .itvl = params->itvl,
        .window = params->window,
        .passive = params->passive,
    };
    // ble_gap_ext_disc() takes the duration in units of 10 ms, 0 for forever.
    uint16_t duration = 0;
    if (duration_ms != BLE_HS_FOREVER) {
        duration = MIN(duration_ms / 10, UINT16_MAX);
    }
    // The same parameters for both PHYs: the controller alternates between them,
    // so Coded PHY (long range) advertisers are heard as well as 1M ones.
    return ble_gap_ext_disc(own_addr_type, duration, 0,
        params->filter_duplicates, params->filter_policy, params->limited,
        &phy_params, &phy_params, cb, cb_arg);
}

static void append(bleio_scanresults_obj_t *results, uint8_t props, int8_t rssi,
    const ble_addr_t *address, const uint8_t *data, uint16_t length) {
    shared_module_bleio_scanresults_append(results,
        supervisor_ticks_ms64(),
        (props & BLE_HCI_ADV_CONN_MASK) != 0,
        // SCAN_RSP, not SCAN: the latter says the advertisement is scannable.
        (props & BLE_HCI_ADV_SCAN_RSP_MASK) != 0,
        rssi,
        address->val,
        address->type,
        data,
        length);
}

void bleio_extended_scan_report(bleio_scanresults_obj_t *results, const struct ble_gap_ext_disc_desc *desc) {
    bool legacy = (desc->props & BLE_HCI_ADV_LEGACY_MASK) != 0;
    if (!_extended) {
        // A scan without extended=True reports legacy advertisements only.
        if (legacy) {
            append(results, desc->props, desc->rssi, &desc->addr, desc->data, desc->length_data);
        }
        return;
    }

    bool same = _pending &&
        _sid == desc->sid &&
        _address.type == desc->addr.type &&
        memcmp(_address.val, desc->addr.val, sizeof(_address.val)) == 0;
    if (!same) {
        _pending = false;
    }

    if (desc->data_status == BLE_GAP_EXT_ADV_DATA_STATUS_COMPLETE && !_pending) {
        append(results, desc->props, desc->rssi, &desc->addr, desc->data, desc->length_data);
        return;
    }

    if (!_pending) {
        _pending = true;
        _address = desc->addr;
        _sid = desc->sid;
        _props = desc->props;
        _rssi = desc->rssi;
        _length = 0;
    }
    if (_length + desc->length_data > EXTENDED_ADV_MAX_DATA) {
        _pending = false;
        return;
    }
    memcpy(_data + _length, desc->data, desc->length_data);
    _length += desc->length_data;

    if (desc->data_status == BLE_GAP_EXT_ADV_DATA_STATUS_INCOMPLETE) {
        return;
    }
    // Truncated: the controller lost the rest, and what is here would only
    // mislead a parser.
    if (desc->data_status == BLE_GAP_EXT_ADV_DATA_STATUS_COMPLETE) {
        append(results, _props, _rssi, &_address, _data, _length);
    }
    _pending = false;
}

#endif // MYNEWT_VAL(BLE_EXT_ADV)
