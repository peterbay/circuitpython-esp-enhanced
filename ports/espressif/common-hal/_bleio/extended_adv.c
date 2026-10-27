// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Petr Vavrin
//
// SPDX-License-Identifier: MIT

#include "common-hal/_bleio/extended_adv.h"

#include "nimble/hci_common.h"

uint8_t bleio_extended_adv_primary_phy = BLE_HCI_LE_PHY_1M;
uint8_t bleio_extended_adv_secondary_phy = BLE_HCI_LE_PHY_1M;

void bleio_extended_adv_reset(void) {
    bleio_extended_adv_primary_phy = BLE_HCI_LE_PHY_1M;
    bleio_extended_adv_secondary_phy = BLE_HCI_LE_PHY_1M;
}
