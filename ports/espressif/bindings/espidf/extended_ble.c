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

#include "common-hal/_bleio/Connection.h"
#include "common-hal/_bleio/extended_adv.h"
#include "shared-bindings/_bleio/Connection.h"
#include "host/ble_gap.h"
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

//| def ble_prefer_phy(phy: int) -> None:
//|     """Ask NimBLE to prefer a PHY for future BLE *connections* (not
//|     advertising -- that is `ble_advertising_phy`). 1 for 1M, 2 for 2M, 4 for
//|     Coded / long range. Existing connections are unaffected and the peer has to
//|     agree."""
//|     ...
//|
//|
static mp_obj_t espidf_ble_prefer_phy(mp_obj_t phy_in) {
    mp_int_t phy = mp_obj_get_int(phy_in);
    if (phy != 1 && phy != 2 && phy != 4) {
        mp_raise_ValueError(MP_ERROR_TEXT("phy must be 1, 2 or 4"));
    }
    // The masks happen to equal the values: 1M=0x01, 2M=0x02, Coded=0x04.
    int rc = ble_gap_set_prefered_default_le_phy((uint8_t)phy, (uint8_t)phy);
    if (rc != 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("could not set PHY"));
    }
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(espidf_ble_prefer_phy_obj, espidf_ble_prefer_phy);

static void check_nimble(int rc) {
    if (rc != 0) {
        mp_raise_OSError_msg_varg(MP_ERROR_TEXT("NimBLE error %d"), rc);
    }
}

static bleio_connection_internal_t *connection_of(mp_obj_t connection_in) {
    bleio_connection_obj_t *connection = mp_arg_validate_type(connection_in, &bleio_connection_type, MP_QSTR_connection);
    bleio_check_connected(bleio_connection_get_conn_handle(connection));
    return connection->connection;
}

static uint8_t phy_mask_from_python(mp_obj_t phy_in) {
    mp_int_t phy = mp_obj_get_int(phy_in);
    if (phy != 1 && phy != 2 && phy != 4) {
        mp_raise_ValueError(MP_ERROR_TEXT("phy must be 1, 2 or 4"));
    }
    // The masks equal the values: 1M=0x01, 2M=0x02, Coded=0x04.
    return phy;
}

//| def ble_connect_phy(phy: int | None = None) -> int:
//|     """Choose how `_bleio.Adapter.connect` connects from its next call on,
//|     and return the setting. 1 connects on the 1M PHY, as upstream does. 4
//|     listens on both 1M and Coded, and connects on Coded -- long range --
//|     to a peer that advertises there (``ble_advertising_phy(4)`` on another
//|     board of this firmware). Called without an argument it only reports.
//|
//|     Back to 1 on every soft reset."""
//|     ...
//|
//|
static mp_obj_t espidf_ble_connect_phy(size_t n_args, const mp_obj_t *args) {
    if (n_args > 0 && args[0] != mp_const_none) {
        mp_int_t phy = mp_obj_get_int(args[0]);
        if (phy != 1 && phy != 4) {
            mp_raise_ValueError(MP_ERROR_TEXT("phy must be 1 or 4"));
        }
        bleio_extended_connect_phy = phy == 4 ? BLE_HCI_LE_PHY_CODED : BLE_HCI_LE_PHY_1M;
    }
    return phy_to_python(bleio_extended_connect_phy);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(espidf_ble_connect_phy_obj, 0, 1, espidf_ble_connect_phy);

//| def ble_connection_info(connection: _bleio.Connection) -> dict:
//|     """What a connection runs at, as a dict:
//|
//|     - ``rssi``: signal strength of the peer's packets, dBm
//|     - ``phy``: ``(tx, rx)``, 1 for 1M, 2 for 2M, 4 for Coded
//|     - ``data_length``: ``(tx, rx)``, the most payload bytes per link layer
//|       packet; 27 unless both sides agreed on more, which the controllers do
//|       by themselves (251 between two ESP32s)
//|     - ``mtu``: the ATT MTU, which bounds one notification or write
//|     - ``interval_ms``, ``latency``, ``supervision_timeout_ms``: the
//|       connection parameters
//|     """
//|     ...
//|
//|
static mp_obj_t espidf_ble_connection_info(mp_obj_t connection_in) {
    bleio_connection_internal_t *connection = connection_of(connection_in);
    const uint16_t handle = connection->conn_handle;
    struct ble_gap_conn_desc desc;
    check_nimble(ble_gap_conn_find(handle, &desc));
    int8_t rssi;
    check_nimble(ble_gap_conn_rssi(handle, &rssi));
    uint8_t tx_phy, rx_phy;
    check_nimble(ble_gap_read_le_phy(handle, &tx_phy, &rx_phy));

    mp_obj_t dict = mp_obj_new_dict(7);
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_rssi), MP_OBJ_NEW_SMALL_INT(rssi));
    mp_obj_t phy[2] = { phy_to_python(tx_phy), phy_to_python(rx_phy) };
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_phy), mp_obj_new_tuple(2, phy));
    mp_obj_t data_length[2] = {
        MP_OBJ_NEW_SMALL_INT(connection->data_len_tx),
        MP_OBJ_NEW_SMALL_INT(connection->data_len_rx),
    };
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_data_length), mp_obj_new_tuple(2, data_length));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_mtu), MP_OBJ_NEW_SMALL_INT(connection->mtu));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_interval_ms), mp_obj_new_float(desc.conn_itvl * 1.25f));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_latency), MP_OBJ_NEW_SMALL_INT(desc.conn_latency));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_supervision_timeout_ms), MP_OBJ_NEW_SMALL_INT(desc.supervision_timeout * 10));
    return dict;
}
MP_DEFINE_CONST_FUN_OBJ_1(espidf_ble_connection_info_obj, espidf_ble_connection_info);

//| def ble_connection_phy(connection: _bleio.Connection, phy: int) -> None:
//|     """Ask to move an existing connection to another PHY: 1 for 1M, 2 for 2M
//|     (twice the bit rate, half the air time), 4 for Coded (long range). The
//|     peer has to agree; `ble_connection_info` shows the result once the link
//|     layer has switched. The link layer runs one such change at a time, and
//|     each takes effect some connection events later (about 2 s at a 300 ms
//|     interval): asked for together with a new ``connection_interval``, one
//|     of the two was dropped, so let the first finish. On the Coded PHY a
//|     packet takes longer on air than 7.5 ms, and that interval dropped the
//|     link."""
//|     ...
//|
//|
static mp_obj_t espidf_ble_connection_phy(mp_obj_t connection_in, mp_obj_t phy_in) {
    bleio_connection_internal_t *connection = connection_of(connection_in);
    uint8_t mask = phy_mask_from_python(phy_in);
    check_nimble(ble_gap_set_prefered_le_phy(connection->conn_handle, mask, mask, BLE_GAP_LE_PHY_CODED_ANY));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(espidf_ble_connection_phy_obj, espidf_ble_connection_phy);

#endif // CIRCUITPY_BLEIO_NATIVE
