// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Petr Vavrin
//
// SPDX-License-Identifier: MIT

// Bluetooth settings _bleio has no argument for; see extended.h.

#include "py/obj.h"
#include "py/runtime.h"

#include "bindings/espidf/extended.h"

#if CIRCUITPY_BLEIO_NATIVE

#include "common-hal/_bleio/extended_adv.h"
#include "nimble/hci_common.h"

// The values follow the BLE_GAP_LE_PHY_*_MASK bits: 1 for 1M, 2 for 2M, 4 for
// Coded.
static uint8_t phy_from_python(mp_obj_t value, bool primary) {
    switch (mp_obj_get_int(value)) {
        case 1:
            return BLE_HCI_LE_PHY_1M;
        case 2:
            if (!primary) {
                return BLE_HCI_LE_PHY_2M;
            }
            break;
        case 4:
            return BLE_HCI_LE_PHY_CODED;
    }
    mp_raise_ValueError(primary
        ? MP_ERROR_TEXT("primary must be 1 (1M) or 4 (Coded)")
        : MP_ERROR_TEXT("secondary must be 1 (1M), 2 (2M) or 4 (Coded)"));
}

static mp_obj_t phy_to_python(uint8_t phy) {
    return MP_OBJ_NEW_SMALL_INT(phy == BLE_HCI_LE_PHY_CODED ? 4 : phy);
}

//| def ble_advertising_phy(primary: int | None = None, secondary: int | None = None) -> Tuple[int, int]:
//|     """Choose the PHYs `_bleio.Adapter.start_advertising` uses from its next
//|     call on, and return the setting as ``(primary, secondary)``. Called without
//|     arguments it only reports.
//|
//|     1 is the 1M PHY, 2 the 2M PHY and 4 the Coded PHY, Bluetooth 5's long range
//|     mode. ``primary`` carries the advertisement itself and may be 1 or 4;
//|     ``secondary`` carries the data of an extended advertisement and defaults to
//|     ``primary``. Anything but 1M makes the advertisement an extended one, which
//|     scanners that listen only for legacy advertisements do not see. A scan with
//|     ``start_scan(extended=True)`` on this firmware listens on the Coded PHY.
//|
//|     Back to 1M on every soft reset."""
//|     ...
//|
//|
static mp_obj_t espidf_ble_advertising_phy(size_t n_args, const mp_obj_t *args) {
    if (n_args > 0 && args[0] != mp_const_none) {
        uint8_t primary = phy_from_python(args[0], true);
        uint8_t secondary = (n_args > 1 && args[1] != mp_const_none)
            ? phy_from_python(args[1], false)
            : primary;
        bleio_extended_adv_primary_phy = primary;
        bleio_extended_adv_secondary_phy = secondary;
    } else if (n_args > 1 && args[1] != mp_const_none) {
        bleio_extended_adv_secondary_phy = phy_from_python(args[1], false);
    }
    mp_obj_t result[2] = {
        phy_to_python(bleio_extended_adv_primary_phy),
        phy_to_python(bleio_extended_adv_secondary_phy),
    };
    return mp_obj_new_tuple(2, result);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(espidf_ble_advertising_phy_obj, 0, 2, espidf_ble_advertising_phy);

#endif // CIRCUITPY_BLEIO_NATIVE
