// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-License-Identifier: MIT

#include <string.h>

#include "py/objproperty.h"
#include "py/runtime.h"

#include "bindings/zigbee/Endpoint.h"
#include "bindings/zigbee/Stack.h"

#include "ezbee/zcl/zcl_common.h"
#include "ezbee/zcl/zcl_desc.h"
#include "ezbee/zcl/zcl_type.h"
#include "ezbee/zha.h"

// Every ZHA device type the library can build, by its standard device id. The
// ids are the ones that travel in a simple descriptor, so a coordinator reading
// a remote device gets the same numbers back that it would use to build one.
//
// Each case takes the library's own default configuration for that type. The
// defaults are what its examples use, and an endpoint assembled by hand from
// individual clusters is not the same thing: the helper also sets the profile,
// the device version and the client-role clusters that make a device usable
// from the other side.
#define BUILD_ZHA(lower, UPPER) \
    case EZB_ZHA_##UPPER##_DEVICE_ID: { \
        ezb_zha_##lower##_config_t cfg = EZB_ZHA_##UPPER##_CONFIG(); \
        return ezb_zha_create_##lower(self->endpoint_id, &cfg); \
    }

ezb_af_ep_desc_t zigbee_endpoint_build(zigbee_endpoint_obj_t *self) {
    switch (self->device_type) {
        BUILD_ZHA(on_off_switch, ON_OFF_SWITCH)
        BUILD_ZHA(configuration_tool, CONFIGURATION_TOOL)
        BUILD_ZHA(mains_power_outlet, MAINS_POWER_OUTLET)
        BUILD_ZHA(door_lock, DOOR_LOCK)
        BUILD_ZHA(door_lock_controller, DOOR_LOCK_CONTROLLER)
        BUILD_ZHA(on_off_light, ON_OFF_LIGHT)
        BUILD_ZHA(dimmable_light, DIMMABLE_LIGHT)
        BUILD_ZHA(color_dimmable_light, COLOR_DIMMABLE_LIGHT)
        BUILD_ZHA(dimmer_switch, DIMMER_SWITCH)
        BUILD_ZHA(color_dimmer_switch, COLOR_DIMMER_SWITCH)
        BUILD_ZHA(light_sensor, LIGHT_SENSOR)
        BUILD_ZHA(shade, SHADE)
        BUILD_ZHA(shade_controller, SHADE_CONTROLLER)
        BUILD_ZHA(window_covering, WINDOW_COVERING)
        BUILD_ZHA(window_covering_controller, WINDOW_COVERING_CONTROLLER)
        BUILD_ZHA(thermostat, THERMOSTAT)
        BUILD_ZHA(temperature_sensor, TEMPERATURE_SENSOR)
        BUILD_ZHA(custom_gateway, CUSTOM_GATEWAY)
        default:
            return NULL;
    }
}

// Whether a device type is one this can build. Checked in the constructor so
// that a wrong constant is a ValueError at the point of the mistake rather than
// a stack that fails to start much later.
static bool known_device_type(mp_int_t type) {
    switch (type) {
        case EZB_ZHA_ON_OFF_SWITCH_DEVICE_ID:
        case EZB_ZHA_CONFIGURATION_TOOL_DEVICE_ID:
        case EZB_ZHA_MAINS_POWER_OUTLET_DEVICE_ID:
        case EZB_ZHA_DOOR_LOCK_DEVICE_ID:
        case EZB_ZHA_DOOR_LOCK_CONTROLLER_DEVICE_ID:
        case EZB_ZHA_ON_OFF_LIGHT_DEVICE_ID:
        case EZB_ZHA_DIMMABLE_LIGHT_DEVICE_ID:
        case EZB_ZHA_COLOR_DIMMABLE_LIGHT_DEVICE_ID:
        case EZB_ZHA_DIMMER_SWITCH_DEVICE_ID:
        case EZB_ZHA_COLOR_DIMMER_SWITCH_DEVICE_ID:
        case EZB_ZHA_LIGHT_SENSOR_DEVICE_ID:
        case EZB_ZHA_SHADE_DEVICE_ID:
        case EZB_ZHA_SHADE_CONTROLLER_DEVICE_ID:
        case EZB_ZHA_WINDOW_COVERING_DEVICE_ID:
        case EZB_ZHA_WINDOW_COVERING_CONTROLLER_DEVICE_ID:
        case EZB_ZHA_THERMOSTAT_DEVICE_ID:
        case EZB_ZHA_TEMPERATURE_SENSOR_DEVICE_ID:
        case EZB_ZHA_CUSTOM_GATEWAY_DEVICE_ID:
            return true;
        default:
            return false;
    }
}

//| class Endpoint:
//|     """One endpoint on this device: a ZHA device type and the clusters that
//|     come with it.
//|
//|     Endpoints have to exist before the stack starts, so build them first and
//|     hand them to `Stack`::
//|
//|         sensor = zigbee.Endpoint(1, zigbee.TEMPERATURE_SENSOR)
//|         stack = zigbee.Stack(role=zigbee.END_DEVICE, endpoints=(sensor,))
//|         stack.start()
//|         sensor[zigbee.TEMPERATURE_MEASUREMENT, 0x0000] = 2350  # 23.50 degC
//|
//|     The clusters an endpoint carries are decided by its device type. They are
//|     the library's own defaults for that type, which is what makes a device
//|     recognisable to a coordinator that did not build it."""
//|
//|     def __init__(self, endpoint_id: int, device_type: int) -> None:
//|         """Describe an endpoint. Nothing is created until the stack starts.
//|
//|         :param int endpoint_id: 1 to 240. Each endpoint on a device needs its
//|           own, and 0 belongs to the stack.
//|         :param int device_type: one of the module's device type constants,
//|           for instance `ON_OFF_LIGHT` or `THERMOSTAT`."""
//|         ...
static mp_obj_t zigbee_endpoint_make_new(const mp_obj_type_t *type, size_t n_args,
    size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_endpoint_id, ARG_device_type };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_endpoint_id, MP_ARG_REQUIRED | MP_ARG_INT },
        { MP_QSTR_device_type, MP_ARG_REQUIRED | MP_ARG_INT },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args),
        allowed_args, args);

    // 0 is the ZDO's own endpoint and 241-254 are reserved for Green Power and
    // the like, so neither is available to an application.
    mp_int_t endpoint_id = mp_arg_validate_int_range(args[ARG_endpoint_id].u_int,
        1, 240, MP_QSTR_endpoint_id);
    if (!known_device_type(args[ARG_device_type].u_int)) {
        mp_raise_ValueError(MP_ERROR_TEXT("unknown device_type"));
    }

    zigbee_endpoint_obj_t *self = mp_obj_malloc(zigbee_endpoint_obj_t, &zigbee_endpoint_type);
    self->endpoint_id = (uint8_t)endpoint_id;
    self->device_type = (uint16_t)args[ARG_device_type].u_int;
    self->registered = false;
    return MP_OBJ_FROM_PTR(self);
}

static zigbee_endpoint_obj_t *registered_endpoint(mp_obj_t self_in) {
    zigbee_endpoint_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->registered) {
        // The descriptors are built by the stack's task during start(). Before
        // that there is no attribute anywhere to read or write.
        mp_raise_RuntimeError(MP_ERROR_TEXT("endpoint not registered; start the stack first"));
    }
    return self;
}

// ZCL attribute values are stored raw, in whatever width the type says, so a
// value cannot be read or written without consulting the type first.
mp_obj_t zigbee_value_to_python(uint8_t type, const void *raw, uint16_t size) {
    switch (type) {
        case EZB_ZCL_ATTR_TYPE_BOOL:
            return mp_obj_new_bool(*(const uint8_t *)raw != 0);
        case EZB_ZCL_ATTR_TYPE_INT8:
            return MP_OBJ_NEW_SMALL_INT(*(const int8_t *)raw);
        case EZB_ZCL_ATTR_TYPE_INT16:
            return MP_OBJ_NEW_SMALL_INT(*(const int16_t *)raw);
        case EZB_ZCL_ATTR_TYPE_INT32:
            return mp_obj_new_int(*(const int32_t *)raw);
        case EZB_ZCL_ATTR_TYPE_UINT8:
        case EZB_ZCL_ATTR_TYPE_ENUM8:
        case EZB_ZCL_ATTR_TYPE_MAP8:
        case EZB_ZCL_ATTR_TYPE_DATA8:
            return MP_OBJ_NEW_SMALL_INT(*(const uint8_t *)raw);
        case EZB_ZCL_ATTR_TYPE_UINT16:
        case EZB_ZCL_ATTR_TYPE_ENUM16:
        case EZB_ZCL_ATTR_TYPE_MAP16:
        case EZB_ZCL_ATTR_TYPE_DATA16:
        case EZB_ZCL_ATTR_TYPE_CLUSTER_ID:
        case EZB_ZCL_ATTR_TYPE_ATTRIBUTE_ID:
            return MP_OBJ_NEW_SMALL_INT(*(const uint16_t *)raw);
        case EZB_ZCL_ATTR_TYPE_UINT32:
        case EZB_ZCL_ATTR_TYPE_MAP32:
        case EZB_ZCL_ATTR_TYPE_DATA32:
        case EZB_ZCL_ATTR_TYPE_UTC:
            return mp_obj_new_int_from_uint(*(const uint32_t *)raw);
        case EZB_ZCL_ATTR_TYPE_SINGLE:
            return mp_obj_new_float(*(const float *)raw);
        case EZB_ZCL_ATTR_TYPE_STRING:
        case EZB_ZCL_ATTR_TYPE_OCTSTR: {
            // Length-prefixed, and 0xFF in the length byte means "not set".
            const uint8_t *bytes = raw;
            if (bytes[0] == 0xFF) {
                return mp_const_none;
            }
            if (type == EZB_ZCL_ATTR_TYPE_STRING) {
                return mp_obj_new_str((const char *)bytes + 1, bytes[0]);
            }
            return mp_obj_new_bytes(bytes + 1, bytes[0]);
        }
        default:
            // Anything this does not know how to type is handed back whole, so
            // an unusual attribute is still reachable rather than an error.
            return mp_obj_new_bytes(raw, size);
    }
}

uint16_t zigbee_value_from_python(uint8_t type, mp_obj_t value,
    uint8_t *raw, uint16_t capacity) {
    switch (type) {
        case EZB_ZCL_ATTR_TYPE_BOOL:
            raw[0] = mp_obj_is_true(value) ? 1 : 0;
            return 1;
        case EZB_ZCL_ATTR_TYPE_INT8:
        case EZB_ZCL_ATTR_TYPE_UINT8:
        case EZB_ZCL_ATTR_TYPE_ENUM8:
        case EZB_ZCL_ATTR_TYPE_MAP8:
        case EZB_ZCL_ATTR_TYPE_DATA8:
            raw[0] = (uint8_t)mp_obj_get_int(value);
            return 1;
        case EZB_ZCL_ATTR_TYPE_INT16:
        case EZB_ZCL_ATTR_TYPE_UINT16:
        case EZB_ZCL_ATTR_TYPE_ENUM16:
        case EZB_ZCL_ATTR_TYPE_MAP16:
        case EZB_ZCL_ATTR_TYPE_DATA16:
        case EZB_ZCL_ATTR_TYPE_CLUSTER_ID:
        case EZB_ZCL_ATTR_TYPE_ATTRIBUTE_ID: {
            uint16_t v = (uint16_t)mp_obj_get_int(value);
            memcpy(raw, &v, sizeof(v));
            return sizeof(v);
        }
        case EZB_ZCL_ATTR_TYPE_INT32:
        case EZB_ZCL_ATTR_TYPE_UINT32:
        case EZB_ZCL_ATTR_TYPE_MAP32:
        case EZB_ZCL_ATTR_TYPE_DATA32:
        case EZB_ZCL_ATTR_TYPE_UTC: {
            uint32_t v = (uint32_t)mp_obj_get_int(value);
            memcpy(raw, &v, sizeof(v));
            return sizeof(v);
        }
        case EZB_ZCL_ATTR_TYPE_SINGLE: {
            float v = mp_obj_get_float(value);
            memcpy(raw, &v, sizeof(v));
            return sizeof(v);
        }
        case EZB_ZCL_ATTR_TYPE_STRING:
        case EZB_ZCL_ATTR_TYPE_OCTSTR: {
            mp_buffer_info_t buf;
            mp_get_buffer_raise(value, &buf, MP_BUFFER_READ);
            // The stored form is one length byte and then the bytes.
            if (buf.len + 1 > capacity || buf.len > 0xFE) {
                mp_raise_ValueError(MP_ERROR_TEXT("value too long for this attribute"));
            }
            raw[0] = (uint8_t)buf.len;
            memcpy(raw + 1, buf.buf, buf.len);
            return (uint16_t)(buf.len + 1);
        }
        default: {
            mp_buffer_info_t buf;
            // A type this does not lay out itself is taken as raw bytes.
            mp_get_buffer_raise(value, &buf, MP_BUFFER_READ);
            if (buf.len > capacity) {
                mp_raise_ValueError(MP_ERROR_TEXT("value is too large for this attribute"));
            }
            memcpy(raw, buf.buf, buf.len);
            return (uint16_t)buf.len;
        }
    }
}

static ezb_zcl_attr_desc_t find_attr(zigbee_endpoint_obj_t *self, mp_obj_t key,
    uint16_t *cluster_out, uint16_t *attr_out, uint8_t *role_out) {
    // The key is (cluster, attribute) or (cluster, attribute, role). Server is
    // the role that holds values; client-role clusters are the ones this device
    // sends commands to.
    mp_obj_t *items;
    size_t len;
    mp_obj_get_array(key, &len, &items);
    if (len < 2 || len > 3) {
        mp_raise_TypeError(MP_ERROR_TEXT("index must be (cluster, attribute[, role])"));
    }
    uint16_t cluster = (uint16_t)mp_obj_get_int(items[0]);
    uint16_t attribute = (uint16_t)mp_obj_get_int(items[1]);
    uint8_t role = (len == 3) ? (uint8_t)mp_obj_get_int(items[2]) : EZB_ZCL_CLUSTER_SERVER;

    zigbee_lock();
    ezb_zcl_attr_desc_t desc = ezb_zcl_get_attr_desc(self->endpoint_id, cluster, role,
        attribute, EZB_ZCL_STD_MANUF_CODE);
    zigbee_unlock();
    if (desc == EZB_INVALID_ZCL_ATTR_DESC) {
        mp_raise_ValueError(MP_ERROR_TEXT("no such attribute on this endpoint"));
    }
    *cluster_out = cluster;
    *attr_out = attribute;
    *role_out = role;
    return desc;
}

//|     def __getitem__(self, key: tuple) -> bool | int | float | str | bytes | None:
//|         """Read one of this endpoint's own attributes.
//|
//|         The key is ``(cluster, attribute)``, or ``(cluster, attribute, role)``
//|         to reach a client-role cluster. The value comes back typed from the
//|         attribute's own ZCL type, and is None for a string that has never
//|         been set.
//|
//|         This is local: it reads what this device holds, and sends nothing.
//|         To read another device, see `Stack.read`."""
//|         ...
//|     def __setitem__(self, key: tuple, value: bool | int | float | str | bytes) -> None:
//|         """Write one of this endpoint's own attributes.
//|
//|         Anything that has asked to be told about the attribute -- a bound
//|         coordinator, a configured report -- is told, so this is how a sensor
//|         publishes a reading."""
//|         ...
static mp_obj_t zigbee_endpoint_subscr(mp_obj_t self_in, mp_obj_t key, mp_obj_t value) {
    zigbee_endpoint_obj_t *self = registered_endpoint(self_in);
    uint16_t cluster, attribute;
    uint8_t role;
    ezb_zcl_attr_desc_t desc = find_attr(self, key, &cluster, &attribute, &role);

    zigbee_lock();
    ezb_zcl_attr_type_t type = ezb_zcl_attr_desc_get_type(desc);
    uint16_t size = ezb_zcl_attr_desc_get_value_size(desc);
    zigbee_unlock();

    // Wide enough for every fixed-width ZCL type, and for the length byte plus
    // the bytes of a string as long as the cluster made room for.
    uint8_t raw[256];
    if (size > sizeof(raw)) {
        mp_raise_ValueError(MP_ERROR_TEXT("attribute is too large to read"));
    }

    if (value == MP_OBJ_SENTINEL) {
        zigbee_lock();
        ezb_err_t err = ezb_zcl_attr_desc_get_value(desc, raw);
        zigbee_unlock();
        if (err != EZB_ERR_NONE) {
            mp_raise_RuntimeError(MP_ERROR_TEXT("could not read the attribute"));
        }
        return zigbee_value_to_python(type, raw, size);
    }
    if (value == MP_OBJ_NULL) {
        // del
        return MP_OBJ_NULL;
    }

    memset(raw, 0, size);
    uint16_t written = zigbee_value_from_python(type, value, raw, size);
    if (written > size) {
        mp_raise_ValueError(MP_ERROR_TEXT("value is the wrong size for this attribute"));
    }
    // ezb_zcl_set_attr_value() rather than a raw write through the descriptor:
    // it is the path the library's own examples use, and the one that tells
    // anything watching -- a configured report, a bound coordinator -- that the
    // value changed. A raw write updates the byte and nobody hears about it.
    //
    // check_access is false because a sensor writes its own measured value,
    // and measured values are read-only as seen from the network. The check is
    // there to stop a remote device writing them, not this one.
    zigbee_lock();
    ezb_zcl_status_t status = ezb_zcl_set_attr_value(self->endpoint_id, cluster, role,
        attribute, EZB_ZCL_STD_MANUF_CODE, raw, false);
    zigbee_unlock();
    if (status != EZB_ZCL_STATUS_SUCCESS) {
        mp_raise_msg_varg(&mp_type_RuntimeError,
            MP_ERROR_TEXT("could not write the attribute (ZCL status %d)"), (int)status);
    }
    return mp_const_none;
}

//|     def clusters(self, role: int = SERVER) -> tuple[int, ...]:
//|         """Which clusters this endpoint carries in that role."""
//|         ...
static mp_obj_t zigbee_endpoint_clusters(size_t n_args, const mp_obj_t *args) {
    zigbee_endpoint_obj_t *self = registered_endpoint(args[0]);
    uint8_t role = (n_args > 1) ? (uint8_t)mp_obj_get_int(args[1]) : EZB_ZCL_CLUSTER_SERVER;

    // Asked one cluster id at a time rather than enumerated: the library gives
    // no way to walk an endpoint's clusters, only to look one up.
    mp_obj_t found[64];
    size_t count = 0;
    for (uint32_t id = 0; id <= 0x1000 && count < MP_ARRAY_SIZE(found); id++) {
        zigbee_lock();
        ezb_zcl_cluster_desc_t desc = ezb_zcl_get_cluster_desc(self->endpoint_id, (uint16_t)id, role);
        zigbee_unlock();
        if (desc != EZB_INVALID_ZCL_CLUSTER_DESC) {
            found[count++] = MP_OBJ_NEW_SMALL_INT(id);
        }
    }
    return mp_obj_new_tuple(count, found);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(zigbee_endpoint_clusters_obj, 1, 2,
    zigbee_endpoint_clusters);

//|     endpoint_id: int
//|     """The number this endpoint answers on."""
static mp_obj_t zigbee_endpoint_get_endpoint_id(mp_obj_t self_in) {
    zigbee_endpoint_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return MP_OBJ_NEW_SMALL_INT(self->endpoint_id);
}
static MP_DEFINE_CONST_FUN_OBJ_1(zigbee_endpoint_get_endpoint_id_obj,
    zigbee_endpoint_get_endpoint_id);
MP_PROPERTY_GETTER(zigbee_endpoint_endpoint_id_obj,
    (mp_obj_t)&zigbee_endpoint_get_endpoint_id_obj);

//|     device_type: int
//|     """The ZHA device type this endpoint was built as."""
static mp_obj_t zigbee_endpoint_get_device_type(mp_obj_t self_in) {
    zigbee_endpoint_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return MP_OBJ_NEW_SMALL_INT(self->device_type);
}
static MP_DEFINE_CONST_FUN_OBJ_1(zigbee_endpoint_get_device_type_obj,
    zigbee_endpoint_get_device_type);
MP_PROPERTY_GETTER(zigbee_endpoint_device_type_obj,
    (mp_obj_t)&zigbee_endpoint_get_device_type_obj);

//|     registered: bool
//|     """True once the stack has built this endpoint, which happens in
//|     `Stack.start`."""
static mp_obj_t zigbee_endpoint_get_registered(mp_obj_t self_in) {
    zigbee_endpoint_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_bool(self->registered);
}
static MP_DEFINE_CONST_FUN_OBJ_1(zigbee_endpoint_get_registered_obj,
    zigbee_endpoint_get_registered);
MP_PROPERTY_GETTER(zigbee_endpoint_registered_obj,
    (mp_obj_t)&zigbee_endpoint_get_registered_obj);

static const mp_rom_map_elem_t zigbee_endpoint_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_clusters), MP_ROM_PTR(&zigbee_endpoint_clusters_obj) },
    { MP_ROM_QSTR(MP_QSTR_endpoint_id), MP_ROM_PTR(&zigbee_endpoint_endpoint_id_obj) },
    { MP_ROM_QSTR(MP_QSTR_device_type), MP_ROM_PTR(&zigbee_endpoint_device_type_obj) },
    { MP_ROM_QSTR(MP_QSTR_registered), MP_ROM_PTR(&zigbee_endpoint_registered_obj) },
};
static MP_DEFINE_CONST_DICT(zigbee_endpoint_locals_dict, zigbee_endpoint_locals_dict_table);

static void zigbee_endpoint_print(const mp_print_t *print, mp_obj_t self_in,
    mp_print_kind_t kind) {
    (void)kind;
    zigbee_endpoint_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_printf(print, "<Endpoint %u, device type 0x%04X%s>",
        self->endpoint_id, self->device_type, self->registered ? "" : ", not registered");
}

MP_DEFINE_CONST_OBJ_TYPE(
    zigbee_endpoint_type,
    MP_QSTR_Endpoint,
    MP_TYPE_FLAG_NONE,
    make_new, zigbee_endpoint_make_new,
    print, zigbee_endpoint_print,
    subscr, zigbee_endpoint_subscr,
    locals_dict, &zigbee_endpoint_locals_dict
    );
