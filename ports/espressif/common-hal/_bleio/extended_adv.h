// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Petr Vavrin
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdint.h>

// The PHYs Adapter.start_advertising() uses, as BLE_HCI_LE_PHY_* values. Set
// through espidf.ble_advertising_phy(); 1M for both by default and after every
// soft reset, which is what upstream always used.
extern uint8_t bleio_extended_adv_primary_phy;
extern uint8_t bleio_extended_adv_secondary_phy;

// The PHY Adapter.connect() initiates on: BLE_HCI_LE_PHY_1M, or
// BLE_HCI_LE_PHY_CODED to listen on both and connect long range when the peer
// advertises there. Set through espidf.ble_connect_phy(); 1M after every soft
// reset.
extern uint8_t bleio_extended_connect_phy;

void bleio_extended_adv_reset(void);
