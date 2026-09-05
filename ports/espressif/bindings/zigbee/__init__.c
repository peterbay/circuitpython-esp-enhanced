// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-License-Identifier: MIT

#include "py/runtime.h"

#include "bindings/zigbee/Endpoint.h"
#include "bindings/zigbee/Stack.h"

#include "ezbee/nwk.h"
#include "ezbee/zcl/zcl_common.h"
#include "ezbee/zha.h"

//| """Zigbee, on the chip's own 802.15.4 radio.
//|
//| This is the stack, not the radio: it forms or joins a network, keeps it, and
//| speaks Zigbee on it. For bare frames with no protocol at all, see
//| `ieee802154` -- but only one of the two can hold the radio at a time.
//|
//| The network survives a reset, because the stack keeps it in the chip's
//| non-volatile storage rather than in the script.
//|
//| A device is one or more `Endpoint` objects, each of a ZHA device type, handed
//| to a `Stack`::
//|
//|     import zigbee
//|
//|     sensor = zigbee.Endpoint(1, zigbee.TEMPERATURE_SENSOR)
//|     stack = zigbee.Stack(role=zigbee.END_DEVICE, endpoints=(sensor,))
//|     stack.start()
//|     sensor[zigbee.TEMPERATURE_MEASUREMENT, 0x0000] = 2350
//|
//| and a coordinator reads it back with `Stack.read`."""

//| COORDINATOR: int
//| """Forms a network and decides who may join it. One per network."""
//| ROUTER: int
//| """Joins a network, relays for others, and is always listening."""
//| END_DEVICE: int
//| """Joins a network through a parent and may sleep."""

//| ON_OFF_SWITCH: int
//| """ZHA device type 0x0000. Sends on/off, holds nothing."""
//| CONFIGURATION_TOOL: int
//| """ZHA device type 0x0005. Manages a network rather than being an
//| appliance, and is what a coordinator gets when no endpoint is given."""
//| MAINS_POWER_OUTLET: int
//| """ZHA device type 0x0009. A switchable socket."""
//| DOOR_LOCK: int
//| """ZHA device type 0x000A."""
//| DOOR_LOCK_CONTROLLER: int
//| """ZHA device type 0x000B. Operates a lock elsewhere."""
//| ON_OFF_LIGHT: int
//| """ZHA device type 0x0100."""
//| DIMMABLE_LIGHT: int
//| """ZHA device type 0x0101. On/off plus a level."""
//| COLOR_DIMMABLE_LIGHT: int
//| """ZHA device type 0x0102. On/off, level and colour."""
//| DIMMER_SWITCH: int
//| """ZHA device type 0x0104. Sends on/off and level."""
//| COLOR_DIMMER_SWITCH: int
//| """ZHA device type 0x0105."""
//| LIGHT_SENSOR: int
//| """ZHA device type 0x0106. Reports illuminance."""
//| SHADE: int
//| """ZHA device type 0x0200."""
//| SHADE_CONTROLLER: int
//| """ZHA device type 0x0201."""
//| WINDOW_COVERING: int
//| """ZHA device type 0x0202."""
//| WINDOW_COVERING_CONTROLLER: int
//| """ZHA device type 0x0203."""
//| THERMOSTAT: int
//| """ZHA device type 0x0301."""
//| TEMPERATURE_SENSOR: int
//| """ZHA device type 0x0302. Reports temperature."""
//| CUSTOM_GATEWAY: int
//| """A gateway with Basic and Identify and nothing else, for a device that
//| does not fit a standard type."""

//| SERVER: int
//| """The cluster role that holds attribute values and answers reads."""
//| CLIENT: int
//| """The cluster role that sends commands to a server elsewhere."""

//| BASIC: int
//| """Cluster 0x0000. Manufacturer, model, power source, and the reset
//| command."""
//| POWER_CONFIG: int
//| """Cluster 0x0001. Battery voltage and percentage."""
//| DEVICE_TEMP_CONFIG: int
//| """Cluster 0x0002."""
//| IDENTIFY: int
//| """Cluster 0x0003. Makes a device announce which one it is."""
//| GROUPS: int
//| """Cluster 0x0004."""
//| SCENES: int
//| """Cluster 0x0005."""
//| ON_OFF: int
//| """Cluster 0x0006. Attribute 0x0000 is the state."""
//| ON_OFF_SWITCH_CONFIG: int
//| """Cluster 0x0007."""
//| LEVEL: int
//| """Cluster 0x0008. Attribute 0x0000 is the current level."""
//| ALARMS: int
//| """Cluster 0x0009."""
//| TIME: int
//| """Cluster 0x000A."""
//| ANALOG_INPUT: int
//| """Cluster 0x000C."""
//| ANALOG_OUTPUT: int
//| """Cluster 0x000D."""
//| ANALOG_VALUE: int
//| """Cluster 0x000E."""
//| BINARY_INPUT: int
//| """Cluster 0x000F."""
//| BINARY_OUTPUT: int
//| """Cluster 0x0010."""
//| BINARY_VALUE: int
//| """Cluster 0x0011."""
//| MULTISTATE_INPUT: int
//| """Cluster 0x0012."""
//| MULTISTATE_OUTPUT: int
//| """Cluster 0x0013."""
//| MULTISTATE_VALUE: int
//| """Cluster 0x0014."""
//| COMMISSIONING: int
//| """Cluster 0x0015."""
//| OTA_UPGRADE: int
//| """Cluster 0x0019."""
//| POLL_CONTROL: int
//| """Cluster 0x0020."""
//| GREEN_POWER: int
//| """Cluster 0x0021."""
//| SHADE_CONFIG: int
//| """Cluster 0x0100."""
//| DOOR_LOCK_CLUSTER: int
//| """Cluster 0x0101."""
//| WINDOW_COVERING_CLUSTER: int
//| """Cluster 0x0102."""
//| THERMOSTAT_CLUSTER: int
//| """Cluster 0x0201."""
//| FAN_CONTROL: int
//| """Cluster 0x0202."""
//| DEHUMIDIFICATION_CONTROL: int
//| """Cluster 0x0203."""
//| THERMOSTAT_UI_CONFIG: int
//| """Cluster 0x0204."""
//| COLOR_CONTROL: int
//| """Cluster 0x0300."""
//| ILLUMINANCE_MEASUREMENT: int
//| """Cluster 0x0400. Attribute 0x0000 is the measured value."""
//| TEMPERATURE_MEASUREMENT: int
//| """Cluster 0x0402. Attribute 0x0000 is hundredths of a degree Celsius."""
//| PRESSURE_MEASUREMENT: int
//| """Cluster 0x0403."""
//| FLOW_MEASUREMENT: int
//| """Cluster 0x0404."""
//| HUMIDITY_MEASUREMENT: int
//| """Cluster 0x0405. Attribute 0x0000 is hundredths of a percent."""
//| OCCUPANCY_SENSING: int
//| """Cluster 0x0406."""
//| PH_MEASUREMENT: int
//| """Cluster 0x0409."""
//| EC_MEASUREMENT: int
//| """Cluster 0x040A. Electrical conductivity."""
//| WIND_SPEED_MEASUREMENT: int
//| """Cluster 0x040B."""
//| CARBON_DIOXIDE_MEASUREMENT: int
//| """Cluster 0x040D."""
//| PM2_5_MEASUREMENT: int
//| """Cluster 0x042A."""
//| IAS_ZONE: int
//| """Cluster 0x0500. Door, motion and leak sensors report here."""
//| IAS_ACE: int
//| """Cluster 0x0501."""
//| IAS_WD: int
//| """Cluster 0x0502. Sirens and strobes."""
//| PRICE: int
//| """Cluster 0x0700."""
//| METERING: int
//| """Cluster 0x0702."""
//| METER_IDENTIFICATION: int
//| """Cluster 0x0B01."""
//| ELECTRICAL_MEASUREMENT: int
//| """Cluster 0x0B04."""
//| TOUCHLINK_COMMISSIONING: int
//| """Cluster 0x1000."""

static const mp_rom_map_elem_t zigbee_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_zigbee) },
    { MP_ROM_QSTR(MP_QSTR_Stack), MP_ROM_PTR(&zigbee_stack_type) },
    { MP_ROM_QSTR(MP_QSTR_Endpoint), MP_ROM_PTR(&zigbee_endpoint_type) },

    { MP_ROM_QSTR(MP_QSTR_COORDINATOR), MP_ROM_INT(EZB_NWK_DEVICE_TYPE_COORDINATOR) },
    { MP_ROM_QSTR(MP_QSTR_ROUTER), MP_ROM_INT(EZB_NWK_DEVICE_TYPE_ROUTER) },
    { MP_ROM_QSTR(MP_QSTR_END_DEVICE), MP_ROM_INT(EZB_NWK_DEVICE_TYPE_END_DEVICE) },

    // Every ZHA device type the library can build, by its standard device id.
    { MP_ROM_QSTR(MP_QSTR_ON_OFF_SWITCH), MP_ROM_INT(EZB_ZHA_ON_OFF_SWITCH_DEVICE_ID) },
    { MP_ROM_QSTR(MP_QSTR_CONFIGURATION_TOOL), MP_ROM_INT(EZB_ZHA_CONFIGURATION_TOOL_DEVICE_ID) },
    { MP_ROM_QSTR(MP_QSTR_MAINS_POWER_OUTLET), MP_ROM_INT(EZB_ZHA_MAINS_POWER_OUTLET_DEVICE_ID) },
    { MP_ROM_QSTR(MP_QSTR_DOOR_LOCK), MP_ROM_INT(EZB_ZHA_DOOR_LOCK_DEVICE_ID) },
    { MP_ROM_QSTR(MP_QSTR_DOOR_LOCK_CONTROLLER), MP_ROM_INT(EZB_ZHA_DOOR_LOCK_CONTROLLER_DEVICE_ID) },
    { MP_ROM_QSTR(MP_QSTR_ON_OFF_LIGHT), MP_ROM_INT(EZB_ZHA_ON_OFF_LIGHT_DEVICE_ID) },
    { MP_ROM_QSTR(MP_QSTR_DIMMABLE_LIGHT), MP_ROM_INT(EZB_ZHA_DIMMABLE_LIGHT_DEVICE_ID) },
    { MP_ROM_QSTR(MP_QSTR_COLOR_DIMMABLE_LIGHT), MP_ROM_INT(EZB_ZHA_COLOR_DIMMABLE_LIGHT_DEVICE_ID) },
    { MP_ROM_QSTR(MP_QSTR_DIMMER_SWITCH), MP_ROM_INT(EZB_ZHA_DIMMER_SWITCH_DEVICE_ID) },
    { MP_ROM_QSTR(MP_QSTR_COLOR_DIMMER_SWITCH), MP_ROM_INT(EZB_ZHA_COLOR_DIMMER_SWITCH_DEVICE_ID) },
    { MP_ROM_QSTR(MP_QSTR_LIGHT_SENSOR), MP_ROM_INT(EZB_ZHA_LIGHT_SENSOR_DEVICE_ID) },
    { MP_ROM_QSTR(MP_QSTR_SHADE), MP_ROM_INT(EZB_ZHA_SHADE_DEVICE_ID) },
    { MP_ROM_QSTR(MP_QSTR_SHADE_CONTROLLER), MP_ROM_INT(EZB_ZHA_SHADE_CONTROLLER_DEVICE_ID) },
    { MP_ROM_QSTR(MP_QSTR_WINDOW_COVERING), MP_ROM_INT(EZB_ZHA_WINDOW_COVERING_DEVICE_ID) },
    { MP_ROM_QSTR(MP_QSTR_WINDOW_COVERING_CONTROLLER),
      MP_ROM_INT(EZB_ZHA_WINDOW_COVERING_CONTROLLER_DEVICE_ID) },
    { MP_ROM_QSTR(MP_QSTR_THERMOSTAT), MP_ROM_INT(EZB_ZHA_THERMOSTAT_DEVICE_ID) },
    { MP_ROM_QSTR(MP_QSTR_TEMPERATURE_SENSOR), MP_ROM_INT(EZB_ZHA_TEMPERATURE_SENSOR_DEVICE_ID) },
    { MP_ROM_QSTR(MP_QSTR_CUSTOM_GATEWAY), MP_ROM_INT(EZB_ZHA_CUSTOM_GATEWAY_DEVICE_ID) },

    { MP_ROM_QSTR(MP_QSTR_SERVER), MP_ROM_INT(EZB_ZCL_CLUSTER_SERVER) },
    { MP_ROM_QSTR(MP_QSTR_CLIENT), MP_ROM_INT(EZB_ZCL_CLUSTER_CLIENT) },

    // The clusters the library carries descriptors for. An endpoint only has
    // the ones its device type brings, which Endpoint.clusters() reports.
    { MP_ROM_QSTR(MP_QSTR_BASIC), MP_ROM_INT(EZB_ZCL_CLUSTER_ID_BASIC) },
    { MP_ROM_QSTR(MP_QSTR_POWER_CONFIG), MP_ROM_INT(EZB_ZCL_CLUSTER_ID_POWER_CONFIG) },
    { MP_ROM_QSTR(MP_QSTR_DEVICE_TEMP_CONFIG), MP_ROM_INT(EZB_ZCL_CLUSTER_ID_DEVICE_TEMP_CONFIG) },
    { MP_ROM_QSTR(MP_QSTR_IDENTIFY), MP_ROM_INT(EZB_ZCL_CLUSTER_ID_IDENTIFY) },
    { MP_ROM_QSTR(MP_QSTR_GROUPS), MP_ROM_INT(EZB_ZCL_CLUSTER_ID_GROUPS) },
    { MP_ROM_QSTR(MP_QSTR_SCENES), MP_ROM_INT(EZB_ZCL_CLUSTER_ID_SCENES) },
    { MP_ROM_QSTR(MP_QSTR_ON_OFF), MP_ROM_INT(EZB_ZCL_CLUSTER_ID_ON_OFF) },
    { MP_ROM_QSTR(MP_QSTR_ON_OFF_SWITCH_CONFIG), MP_ROM_INT(EZB_ZCL_CLUSTER_ID_ON_OFF_SWITCH_CONFIG) },
    { MP_ROM_QSTR(MP_QSTR_LEVEL), MP_ROM_INT(EZB_ZCL_CLUSTER_ID_LEVEL) },
    { MP_ROM_QSTR(MP_QSTR_ALARMS), MP_ROM_INT(EZB_ZCL_CLUSTER_ID_ALARMS) },
    { MP_ROM_QSTR(MP_QSTR_TIME), MP_ROM_INT(EZB_ZCL_CLUSTER_ID_TIME) },
    { MP_ROM_QSTR(MP_QSTR_ANALOG_INPUT), MP_ROM_INT(EZB_ZCL_CLUSTER_ID_ANALOG_INPUT) },
    { MP_ROM_QSTR(MP_QSTR_ANALOG_OUTPUT), MP_ROM_INT(EZB_ZCL_CLUSTER_ID_ANALOG_OUTPUT) },
    { MP_ROM_QSTR(MP_QSTR_ANALOG_VALUE), MP_ROM_INT(EZB_ZCL_CLUSTER_ID_ANALOG_VALUE) },
    { MP_ROM_QSTR(MP_QSTR_BINARY_INPUT), MP_ROM_INT(EZB_ZCL_CLUSTER_ID_BINARY_INPUT) },
    { MP_ROM_QSTR(MP_QSTR_BINARY_OUTPUT), MP_ROM_INT(EZB_ZCL_CLUSTER_ID_BINARY_OUTPUT) },
    { MP_ROM_QSTR(MP_QSTR_BINARY_VALUE), MP_ROM_INT(EZB_ZCL_CLUSTER_ID_BINARY_VALUE) },
    { MP_ROM_QSTR(MP_QSTR_MULTISTATE_INPUT), MP_ROM_INT(EZB_ZCL_CLUSTER_ID_MULTISTATE_INPUT) },
    { MP_ROM_QSTR(MP_QSTR_MULTISTATE_OUTPUT), MP_ROM_INT(EZB_ZCL_CLUSTER_ID_MULTISTATE_OUTPUT) },
    { MP_ROM_QSTR(MP_QSTR_MULTISTATE_VALUE), MP_ROM_INT(EZB_ZCL_CLUSTER_ID_MULTISTATE_VALUE) },
    { MP_ROM_QSTR(MP_QSTR_COMMISSIONING), MP_ROM_INT(EZB_ZCL_CLUSTER_ID_COMMISSIONING) },
    { MP_ROM_QSTR(MP_QSTR_OTA_UPGRADE), MP_ROM_INT(EZB_ZCL_CLUSTER_ID_OTA_UPGRADE) },
    { MP_ROM_QSTR(MP_QSTR_POLL_CONTROL), MP_ROM_INT(EZB_ZCL_CLUSTER_ID_POLL_CONTROL) },
    { MP_ROM_QSTR(MP_QSTR_GREEN_POWER), MP_ROM_INT(EZB_ZCL_CLUSTER_ID_GREEN_POWER) },
    { MP_ROM_QSTR(MP_QSTR_SHADE_CONFIG), MP_ROM_INT(EZB_ZCL_CLUSTER_ID_SHADE_CONFIG) },
    { MP_ROM_QSTR(MP_QSTR_DOOR_LOCK_CLUSTER), MP_ROM_INT(EZB_ZCL_CLUSTER_ID_DOOR_LOCK) },
    { MP_ROM_QSTR(MP_QSTR_WINDOW_COVERING_CLUSTER), MP_ROM_INT(EZB_ZCL_CLUSTER_ID_WINDOW_COVERING) },
    { MP_ROM_QSTR(MP_QSTR_THERMOSTAT_CLUSTER), MP_ROM_INT(EZB_ZCL_CLUSTER_ID_THERMOSTAT) },
    { MP_ROM_QSTR(MP_QSTR_FAN_CONTROL), MP_ROM_INT(EZB_ZCL_CLUSTER_ID_FAN_CONTROL) },
    { MP_ROM_QSTR(MP_QSTR_DEHUMIDIFICATION_CONTROL),
      MP_ROM_INT(EZB_ZCL_CLUSTER_ID_DEHUMIDIFICATION_CONTROL) },
    { MP_ROM_QSTR(MP_QSTR_THERMOSTAT_UI_CONFIG), MP_ROM_INT(EZB_ZCL_CLUSTER_ID_THERMOSTAT_UI_CONFIG) },
    { MP_ROM_QSTR(MP_QSTR_COLOR_CONTROL), MP_ROM_INT(EZB_ZCL_CLUSTER_ID_COLOR_CONTROL) },
    { MP_ROM_QSTR(MP_QSTR_ILLUMINANCE_MEASUREMENT),
      MP_ROM_INT(EZB_ZCL_CLUSTER_ID_ILLUMINANCE_MEASUREMENT) },
    { MP_ROM_QSTR(MP_QSTR_TEMPERATURE_MEASUREMENT),
      MP_ROM_INT(EZB_ZCL_CLUSTER_ID_TEMPERATURE_MEASUREMENT) },
    { MP_ROM_QSTR(MP_QSTR_PRESSURE_MEASUREMENT), MP_ROM_INT(EZB_ZCL_CLUSTER_ID_PRESSURE_MEASUREMENT) },
    { MP_ROM_QSTR(MP_QSTR_FLOW_MEASUREMENT), MP_ROM_INT(EZB_ZCL_CLUSTER_ID_FLOW_MEASUREMENT) },
    { MP_ROM_QSTR(MP_QSTR_HUMIDITY_MEASUREMENT),
      MP_ROM_INT(EZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT) },
    { MP_ROM_QSTR(MP_QSTR_OCCUPANCY_SENSING), MP_ROM_INT(EZB_ZCL_CLUSTER_ID_OCCUPANCY_SENSING) },
    { MP_ROM_QSTR(MP_QSTR_PH_MEASUREMENT), MP_ROM_INT(EZB_ZCL_CLUSTER_ID_PH_MEASUREMENT) },
    { MP_ROM_QSTR(MP_QSTR_EC_MEASUREMENT), MP_ROM_INT(EZB_ZCL_CLUSTER_ID_EC_MEASUREMENT) },
    { MP_ROM_QSTR(MP_QSTR_WIND_SPEED_MEASUREMENT),
      MP_ROM_INT(EZB_ZCL_CLUSTER_ID_WIND_SPEED_MEASUREMENT) },
    { MP_ROM_QSTR(MP_QSTR_CARBON_DIOXIDE_MEASUREMENT),
      MP_ROM_INT(EZB_ZCL_CLUSTER_ID_CARBON_DIOXIDE_MEASUREMENT) },
    { MP_ROM_QSTR(MP_QSTR_PM2_5_MEASUREMENT), MP_ROM_INT(EZB_ZCL_CLUSTER_ID_PM2_5_MEASUREMENT) },
    { MP_ROM_QSTR(MP_QSTR_IAS_ZONE), MP_ROM_INT(EZB_ZCL_CLUSTER_ID_IAS_ZONE) },
    { MP_ROM_QSTR(MP_QSTR_IAS_ACE), MP_ROM_INT(EZB_ZCL_CLUSTER_ID_IAS_ACE) },
    { MP_ROM_QSTR(MP_QSTR_IAS_WD), MP_ROM_INT(EZB_ZCL_CLUSTER_ID_IAS_WD) },
    { MP_ROM_QSTR(MP_QSTR_PRICE), MP_ROM_INT(EZB_ZCL_CLUSTER_ID_PRICE) },
    { MP_ROM_QSTR(MP_QSTR_METERING), MP_ROM_INT(EZB_ZCL_CLUSTER_ID_METERING) },
    { MP_ROM_QSTR(MP_QSTR_METER_IDENTIFICATION),
      MP_ROM_INT(EZB_ZCL_CLUSTER_ID_METER_IDENTIFICATION) },
    { MP_ROM_QSTR(MP_QSTR_ELECTRICAL_MEASUREMENT),
      MP_ROM_INT(EZB_ZCL_CLUSTER_ID_ELECTRICAL_MEASUREMENT) },
    { MP_ROM_QSTR(MP_QSTR_TOUCHLINK_COMMISSIONING),
      MP_ROM_INT(EZB_ZCL_CLUSTER_ID_TOUCHLINK_COMMISSIONING) },
};
static MP_DEFINE_CONST_DICT(zigbee_module_globals, zigbee_module_globals_table);

const mp_obj_module_t zigbee_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&zigbee_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_zigbee, zigbee_module);
