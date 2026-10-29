// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Petr Vavrin
//
// SPDX-License-Identifier: MIT

// espidf.BLEMesh; see extended.h.

#include <string.h>

#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/obj.h"
#include "py/objproperty.h"
#include "py/runtime.h"

#include "bindings/espidf/__init__.h"
#include "bindings/espidf/extended.h"
#include "shared-bindings/_bleio/Adapter.h"
#include "shared-bindings/_bleio/__init__.h"
#include "shared-bindings/util.h"
#include "shared/runtime/context_manager_helpers.h"
#include "supervisor/shared/tick.h"

// The headers below exist only when the bt component carries ESP-BLE-MESH.
#if CIRCUITPY_ESPIDF_BLE_MESH

#include "esp_ble_mesh_common_api.h"
#include "esp_ble_mesh_config_model_api.h"
#include "esp_ble_mesh_defs.h"
#include "esp_ble_mesh_local_data_operation_api.h"
#include "esp_ble_mesh_networking_api.h"
#include "esp_ble_mesh_provisioning_api.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// Bluetooth Mesh: nodes relay each other's messages over advertising, so a
// message reaches devices out of direct range. A device joins only once a
// provisioner has given it the network key and an address (provisioning),
// and its models take messages only under an application key the
// provisioner has added and bound to them (configuration).
//
// One composition serves both roles: a Configuration Server, a Configuration
// Client (used by the provisioner) and one vendor model carrying opaque
// bytes, which the provisioner binds on every node it adds. Nothing is
// stored in flash: a node is provisioned again after every reset.

#define CID_ESP (0x02E5)
#define VND_MODEL_ID (0x0001)
#define VND_OP_DATA ESP_BLE_MESH_MODEL_OP_3(0x00, CID_ESP)
#define PROVISIONER_ADDR (0x0001)
#define FIRST_NODE_ADDR (0x0002)
#define APP_IDX (0x0000)
// The largest access message less its 4-byte MIC and the 3-byte opcode.
#define MESH_MAX_DATA (ESP_BLE_MESH_SDU_MAX_LEN - 4 - 3)
#define MESH_QUEUE_DEPTH (16)
#define CONFIG_RETRIES (3)

// Completions of the stack's asynchronous calls, one bit each.
enum {
    DONE_NODE_PROV_ENABLE = 1 << 0,
    DONE_PROV_ENABLE = 1 << 1,
    DONE_UUID_MATCH = 1 << 2,
    DONE_LOCAL_APP_KEY = 1 << 3,
    DONE_LOCAL_BIND = 1 << 4,
};

typedef enum {
    ADD_IDLE, ADD_WAITING, ADD_PROVISIONING, ADD_APP_KEY, ADD_BIND, ADD_DONE, ADD_FAILED
} add_state_t;

typedef struct {
    uint16_t src;
    uint16_t dst;
    int8_t rssi;
    uint8_t ttl;
    uint16_t len;
    uint8_t data[];
} mesh_msg_t;

typedef struct {
    mp_obj_base_t base;
    bool running;
    bool provisioner;
} espidf_ble_mesh_obj_t;

static espidf_ble_mesh_obj_t *_mesh;
static QueueHandle_t _queue;
static bool _callbacks_registered;

// Shared with the stack's task.
static volatile uint32_t _done;
static volatile int _done_err;
static volatile bool _bound;
static volatile uint16_t _address;
static volatile add_state_t _add_state;
static volatile uint16_t _added_addr;
static volatile int _add_err;
static volatile uint32_t _dropped;
static int _retries;
static uint8_t _app_key[16];

static uint8_t _uuid[16];

static esp_ble_mesh_cfg_srv_t _config_server = {
    .net_transmit = ESP_BLE_MESH_TRANSMIT(2, 20),
    #if CONFIG_BLE_MESH_RELAY
    .relay = ESP_BLE_MESH_RELAY_ENABLED,
    #else
    .relay = ESP_BLE_MESH_RELAY_NOT_SUPPORTED,
    #endif
    .relay_retransmit = ESP_BLE_MESH_TRANSMIT(2, 20),
    .beacon = ESP_BLE_MESH_BEACON_ENABLED,
    .gatt_proxy = ESP_BLE_MESH_GATT_PROXY_NOT_SUPPORTED,
    .friend_state = ESP_BLE_MESH_FRIEND_NOT_SUPPORTED,
    .default_ttl = 7,
};

static esp_ble_mesh_client_t _config_client;

static esp_ble_mesh_model_t _root_models[] = {
    ESP_BLE_MESH_MODEL_CFG_SRV(&_config_server),
    ESP_BLE_MESH_MODEL_CFG_CLI(&_config_client),
};

static esp_ble_mesh_model_op_t _vnd_ops[] = {
    ESP_BLE_MESH_MODEL_OP(VND_OP_DATA, 0),
    ESP_BLE_MESH_MODEL_OP_END,
};

static esp_ble_mesh_model_t _vnd_models[] = {
    ESP_BLE_MESH_VENDOR_MODEL(CID_ESP, VND_MODEL_ID, _vnd_ops, NULL, NULL),
};

static esp_ble_mesh_elem_t _elements[] = {
    ESP_BLE_MESH_ELEMENT(0, _root_models, _vnd_models),
};

static esp_ble_mesh_comp_t _composition = {
    .cid = CID_ESP,
    .element_count = MP_ARRAY_SIZE(_elements),
    .elements = _elements,
};

static esp_ble_mesh_prov_t _provision = {
    .uuid = _uuid,
    .prov_uuid = _uuid,
    .prov_unicast_addr = PROVISIONER_ADDR,
    .prov_start_address = FIRST_NODE_ADDR,
};

static void set_done(uint32_t bit, int err) {
    if (err != 0) {
        _done_err = err;
    }
    _done |= bit;
}

static void config_common(esp_ble_mesh_client_common_param_t *common, uint16_t addr, uint32_t opcode) {
    memset(common, 0, sizeof(*common));
    common->opcode = opcode;
    common->model = _config_client.model;
    common->ctx.net_idx = ESP_BLE_MESH_NET_PRIMARY;
    common->ctx.app_idx = APP_IDX;
    common->ctx.addr = addr;
    common->ctx.send_ttl = 3;
}

// The two configuration steps after provisioning: give the node the
// application key, then bind it to the node's vendor model.
static void send_app_key(uint16_t addr) {
    esp_ble_mesh_client_common_param_t common;
    config_common(&common, addr, ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD);
    esp_ble_mesh_cfg_client_set_state_t set = { 0 };
    set.app_key_add.net_idx = ESP_BLE_MESH_NET_PRIMARY;
    set.app_key_add.app_idx = APP_IDX;
    memcpy(set.app_key_add.app_key, _app_key, 16);
    if (esp_ble_mesh_config_client_set_state(&common, &set) != ESP_OK) {
        _add_state = ADD_FAILED;
    }
}

static void send_bind(uint16_t addr) {
    esp_ble_mesh_client_common_param_t common;
    config_common(&common, addr, ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND);
    esp_ble_mesh_cfg_client_set_state_t set = { 0 };
    set.model_app_bind.element_addr = addr;
    set.model_app_bind.model_app_idx = APP_IDX;
    set.model_app_bind.model_id = VND_MODEL_ID;
    set.model_app_bind.company_id = CID_ESP;
    if (esp_ble_mesh_config_client_set_state(&common, &set) != ESP_OK) {
        _add_state = ADD_FAILED;
    }
}

static void prov_cb(esp_ble_mesh_prov_cb_event_t event, esp_ble_mesh_prov_cb_param_t *param) {
    switch (event) {
        case ESP_BLE_MESH_NODE_PROV_ENABLE_COMP_EVT:
            set_done(DONE_NODE_PROV_ENABLE, param->node_prov_enable_comp.err_code);
            break;
        case ESP_BLE_MESH_NODE_PROV_COMPLETE_EVT:
            _address = param->node_prov_complete.addr;
            break;
        case ESP_BLE_MESH_PROVISIONER_PROV_ENABLE_COMP_EVT:
            set_done(DONE_PROV_ENABLE, param->provisioner_prov_enable_comp.err_code);
            break;
        case ESP_BLE_MESH_PROVISIONER_SET_DEV_UUID_MATCH_COMP_EVT:
            set_done(DONE_UUID_MATCH, param->provisioner_set_dev_uuid_match_comp.err_code);
            break;
        case ESP_BLE_MESH_PROVISIONER_ADD_LOCAL_APP_KEY_COMP_EVT:
            set_done(DONE_LOCAL_APP_KEY, param->provisioner_add_app_key_comp.err_code);
            break;
        case ESP_BLE_MESH_PROVISIONER_BIND_APP_KEY_TO_MODEL_COMP_EVT:
            set_done(DONE_LOCAL_BIND, param->provisioner_bind_app_key_to_model_comp.err_code);
            break;
        case ESP_BLE_MESH_PROVISIONER_RECV_UNPROV_ADV_PKT_EVT: {
            if (_add_state != ADD_WAITING) {
                break;
            }
            const struct ble_mesh_provisioner_recv_unprov_adv_pkt_param *p = &param->provisioner_recv_unprov_adv_pkt;
            esp_ble_mesh_unprov_dev_add_t dev = {
                .addr_type = p->addr_type,
                .oob_info = p->oob_info,
                .bearer = p->bearer,
            };
            memcpy(dev.addr, p->addr, sizeof(dev.addr));
            memcpy(dev.uuid, p->dev_uuid, 16);
            if (esp_ble_mesh_provisioner_add_unprov_dev(&dev,
                ADD_DEV_RM_AFTER_PROV_FLAG | ADD_DEV_START_PROV_NOW_FLAG | ADD_DEV_FLUSHABLE_DEV_FLAG) == ESP_OK) {
                _add_state = ADD_PROVISIONING;
            }
            break;
        }
        case ESP_BLE_MESH_PROVISIONER_PROV_LINK_CLOSE_EVT:
            // Closed before completing: take the next beacon of a matching device.
            if (_add_state == ADD_PROVISIONING) {
                _add_state = ADD_WAITING;
            }
            break;
        case ESP_BLE_MESH_PROVISIONER_PROV_COMPLETE_EVT:
            if (_add_state == ADD_PROVISIONING) {
                _added_addr = param->provisioner_prov_complete.unicast_addr;
                _add_state = ADD_APP_KEY;
                _retries = 0;
                send_app_key(_added_addr);
            }
            break;
        default:
            break;
    }
}

static void config_client_cb(esp_ble_mesh_cfg_client_cb_event_t event, esp_ble_mesh_cfg_client_cb_param_t *param) {
    if (param->params->ctx.addr != _added_addr || (_add_state != ADD_APP_KEY && _add_state != ADD_BIND)) {
        return;
    }
    uint32_t opcode = param->params->opcode;
    switch (event) {
        case ESP_BLE_MESH_CFG_CLIENT_SET_STATE_EVT:
            if (param->error_code != 0) {
                _add_err = param->error_code;
                _add_state = ADD_FAILED;
            } else if (opcode == ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD) {
                if (param->status_cb.appkey_status.status != 0) {
                    _add_err = param->status_cb.appkey_status.status;
                    _add_state = ADD_FAILED;
                } else {
                    _add_state = ADD_BIND;
                    _retries = 0;
                    send_bind(_added_addr);
                }
            } else if (opcode == ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND) {
                if (param->status_cb.model_app_status.status != 0) {
                    _add_err = param->status_cb.model_app_status.status;
                    _add_state = ADD_FAILED;
                } else {
                    _add_state = ADD_DONE;
                }
            }
            break;
        case ESP_BLE_MESH_CFG_CLIENT_TIMEOUT_EVT:
            if (++_retries > CONFIG_RETRIES) {
                _add_err = -MP_ETIMEDOUT;
                _add_state = ADD_FAILED;
            } else if (opcode == ESP_BLE_MESH_MODEL_OP_APP_KEY_ADD) {
                send_app_key(_added_addr);
            } else if (opcode == ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND) {
                send_bind(_added_addr);
            }
            break;
        default:
            break;
    }
}

static void config_server_cb(esp_ble_mesh_cfg_server_cb_event_t event, esp_ble_mesh_cfg_server_cb_param_t *param) {
    if (event == ESP_BLE_MESH_CFG_SERVER_STATE_CHANGE_EVT &&
        param->ctx.recv_op == ESP_BLE_MESH_MODEL_OP_MODEL_APP_BIND &&
        param->value.state_change.mod_app_bind.company_id == CID_ESP &&
        param->value.state_change.mod_app_bind.model_id == VND_MODEL_ID) {
        _bound = true;
    }
}

static void model_cb(esp_ble_mesh_model_cb_event_t event, esp_ble_mesh_model_cb_param_t *param) {
    if (event != ESP_BLE_MESH_MODEL_OPERATION_EVT || param->model_operation.opcode != VND_OP_DATA) {
        return;
    }
    const esp_ble_mesh_msg_ctx_t *ctx = param->model_operation.ctx;
    uint16_t len = param->model_operation.length;
    mesh_msg_t *msg = malloc(sizeof(mesh_msg_t) + len);
    if (msg == NULL) {
        _dropped++;
        return;
    }
    msg->src = ctx->addr;
    msg->dst = ctx->recv_dst;
    msg->rssi = ctx->recv_rssi;
    msg->ttl = ctx->recv_ttl;
    msg->len = len;
    memcpy(msg->data, param->model_operation.msg, len);
    if (_queue == NULL || xQueueSend(_queue, &msg, 0) != pdTRUE) {
        free(msg);
        _dropped++;
    }
}

static void drain_queue(void) {
    mesh_msg_t *msg;
    while (_queue != NULL && xQueueReceive(_queue, &msg, 0) == pdTRUE) {
        free(msg);
    }
}

// Waits for the stack to report a call done, and raises what it reported.
static void wait_done(uint32_t bit, qstr what) {
    uint64_t end = supervisor_ticks_ms64() + 5000;
    while (!(_done & bit)) {
        RUN_BACKGROUND_TASKS;
        if (mp_hal_is_interrupted()) {
            return;
        }
        if (supervisor_ticks_ms64() > end) {
            mp_raise_msg_varg(&mp_type_RuntimeError, MP_ERROR_TEXT("%q: no answer from the mesh stack"), what);
        }
    }
    _done &= ~bit;
    if (_done_err != 0) {
        int err = _done_err;
        _done_err = 0;
        mp_raise_msg_varg(&mp_type_RuntimeError, MP_ERROR_TEXT("%q failed: %d"), what, err);
    }
}

static void mesh_stop(espidf_ble_mesh_obj_t *self) {
    if (!self->running) {
        return;
    }
    esp_ble_mesh_deinit_param_t param = { .erase_flash = false };
    esp_ble_mesh_deinit(&param);
    drain_queue();
    self->running = false;
    if (_mesh == self) {
        _mesh = NULL;
    }
}

static espidf_ble_mesh_obj_t *get_running(mp_obj_t self_in) {
    espidf_ble_mesh_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->running) {
        raise_deinited_error();
    }
    return self;
}

//| class BLEMesh:
//|     """Bluetooth Mesh (ESP-BLE-MESH) between boards running this firmware:
//|     one provisioner, which adds nodes to its network, and the nodes. Every
//|     device carries one vendor model that sends and receives opaque bytes.
//|     Advertising bearer only. Nothing is stored: a node has to be added
//|     again after every reset, and the provisioner makes a new network each
//|     time. Starts BLE if `_bleio.adapter` has not; do not scan with
//|     `_bleio` meanwhile, the mesh scans all the time. ESP32-C5."""
//|
//|     def __init__(self, *, provisioner: bool = False, uuid: Optional[ReadableBuffer] = None, app_key: Optional[ReadableBuffer] = None) -> None:
//|         """Start as a node waiting to be provisioned, or as the provisioner
//|         (address 1, its nodes from address 2 on).
//|
//|         :param ReadableBuffer uuid: up to 16 bytes identifying this device to
//|           a provisioner; by default ``b"CPy"`` and the Bluetooth address.
//|         :param ReadableBuffer app_key: the provisioner's 16-byte application
//|           key; random by default."""
//|         ...
//|
static mp_obj_t espidf_ble_mesh_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_provisioner, ARG_uuid, ARG_app_key };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_provisioner, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = false } },
        { MP_QSTR_uuid, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = mp_const_none } },
        { MP_QSTR_app_key, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = mp_const_none } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    bool provisioner = args[ARG_provisioner].u_bool;
    if (_mesh != NULL) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("BLEMesh in use"));
    }

    memset(_uuid, 0, sizeof(_uuid));
    if (args[ARG_uuid].u_obj != mp_const_none) {
        mp_buffer_info_t uuid;
        mp_get_buffer_raise(args[ARG_uuid].u_obj, &uuid, MP_BUFFER_READ);
        mp_arg_validate_length_max(uuid.len, 16, MP_QSTR_uuid);
        memcpy(_uuid, uuid.buf, uuid.len);
    } else {
        memcpy(_uuid, "CPy", 3);
        esp_read_mac(_uuid + 3, ESP_MAC_BT);
    }
    if (args[ARG_app_key].u_obj != mp_const_none) {
        mp_buffer_info_t key;
        mp_get_buffer_raise(args[ARG_app_key].u_obj, &key, MP_BUFFER_READ);
        mp_arg_validate_length(key.len, 16, MP_QSTR_app_key);
        memcpy(_app_key, key.buf, 16);
    } else {
        esp_fill_random(_app_key, sizeof(_app_key));
    }

    if (!common_hal_bleio_adapter_get_enabled(&common_hal_bleio_adapter_obj)) {
        common_hal_bleio_adapter_set_enabled(&common_hal_bleio_adapter_obj, true);
    }
    if (_queue == NULL) {
        _queue = xQueueCreate(MESH_QUEUE_DEPTH, sizeof(mesh_msg_t *));
        if (_queue == NULL) {
            mp_raise_msg(&mp_type_MemoryError, NULL);
        }
    }
    if (!_callbacks_registered) {
        esp_ble_mesh_register_prov_callback(prov_cb);
        esp_ble_mesh_register_config_client_callback(config_client_cb);
        esp_ble_mesh_register_config_server_callback(config_server_cb);
        esp_ble_mesh_register_custom_model_callback(model_cb);
        _callbacks_registered = true;
    }
    _done = 0;
    _done_err = 0;
    _bound = false;
    _address = 0;
    _add_state = ADD_IDLE;
    _dropped = 0;

    espidf_ble_mesh_obj_t *self = mp_obj_malloc_with_finaliser(espidf_ble_mesh_obj_t, &espidf_ble_mesh_type);
    self->running = false;
    self->provisioner = provisioner;
    CHECK_ESP_RESULT(esp_ble_mesh_init(&_provision, &_composition));
    self->running = true;
    _mesh = self;

    // Undone by deinit if any step raises.
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        if (provisioner) {
            CHECK_ESP_RESULT(esp_ble_mesh_provisioner_prov_enable(ESP_BLE_MESH_PROV_ADV));
            wait_done(DONE_PROV_ENABLE, MP_QSTR_provisioner);
            CHECK_ESP_RESULT(esp_ble_mesh_provisioner_add_local_app_key(_app_key, ESP_BLE_MESH_NET_PRIMARY, APP_IDX));
            wait_done(DONE_LOCAL_APP_KEY, MP_QSTR_app_key);
            CHECK_ESP_RESULT(esp_ble_mesh_provisioner_bind_app_key_to_local_model(PROVISIONER_ADDR, APP_IDX, VND_MODEL_ID, CID_ESP));
            wait_done(DONE_LOCAL_BIND, MP_QSTR_app_key);
            _address = PROVISIONER_ADDR;
            _bound = true;
        } else {
            CHECK_ESP_RESULT(esp_ble_mesh_node_prov_enable(ESP_BLE_MESH_PROV_ADV));
            wait_done(DONE_NODE_PROV_ENABLE, MP_QSTR_BLEMesh);
        }
        nlr_pop();
    } else {
        mesh_stop(self);
        nlr_jump(nlr.ret_val);
    }
    return MP_OBJ_FROM_PTR(self);
}

//|     def add_node(self, uuid_prefix: ReadableBuffer = b"CPy", *, timeout: float = 30) -> int:
//|         """Provisioner only: wait for an unprovisioned device whose uuid
//|         starts with ``uuid_prefix``, add it to the network, give it the
//|         application key and bind it to its vendor model.
//|
//|         :return: the node's address.
//|         :raises OSError: ETIMEDOUT if no device was added in time."""
//|         ...
//|
static mp_obj_t espidf_ble_mesh_add_node(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_self, ARG_uuid_prefix, ARG_timeout };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_self, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_uuid_prefix, MP_ARG_OBJ, { .u_obj = MP_OBJ_NULL } },
        { MP_QSTR_timeout, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = MP_OBJ_NEW_SMALL_INT(30) } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    espidf_ble_mesh_obj_t *self = get_running(args[ARG_self].u_obj);
    if (!self->provisioner) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("Not the provisioner"));
    }
    uint8_t prefix[16];
    size_t prefix_len = 3;
    memcpy(prefix, "CPy", 3);
    if (args[ARG_uuid_prefix].u_obj != MP_OBJ_NULL) {
        mp_buffer_info_t buf;
        mp_get_buffer_raise(args[ARG_uuid_prefix].u_obj, &buf, MP_BUFFER_READ);
        prefix_len = mp_arg_validate_length_range(buf.len, 1, 16, MP_QSTR_uuid_prefix);
        memcpy(prefix, buf.buf, prefix_len);
    }
    mp_float_t timeout = mp_obj_get_float(args[ARG_timeout].u_obj);

    CHECK_ESP_RESULT(esp_ble_mesh_provisioner_set_dev_uuid_match(prefix, prefix_len, 0, false));
    wait_done(DONE_UUID_MATCH, MP_QSTR_uuid_prefix);
    _add_err = 0;
    _add_state = ADD_WAITING;
    uint64_t end = supervisor_ticks_ms64() + (uint64_t)(timeout * 1000);
    while (_add_state != ADD_DONE && _add_state != ADD_FAILED) {
        RUN_BACKGROUND_TASKS;
        if (mp_hal_is_interrupted()) {
            _add_state = ADD_IDLE;
            return mp_const_none;
        }
        if (supervisor_ticks_ms64() > end) {
            _add_state = ADD_IDLE;
            mp_raise_OSError(MP_ETIMEDOUT);
        }
    }
    add_state_t state = _add_state;
    _add_state = ADD_IDLE;
    if (state == ADD_FAILED) {
        if (_add_err == -MP_ETIMEDOUT) {
            mp_raise_OSError(MP_ETIMEDOUT);
        }
        mp_raise_msg_varg(&mp_type_RuntimeError, MP_ERROR_TEXT("Node configuration failed: %d"), _add_err);
    }
    return MP_OBJ_NEW_SMALL_INT(_added_addr);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(espidf_ble_mesh_add_node_obj, 1, espidf_ble_mesh_add_node);

//|     def send(self, dest: int, data: ReadableBuffer, *, ttl: int = 3) -> None:
//|         """Send ``data`` (up to 377 bytes; more than 8 go in segments) to the
//|         node at ``dest``, or to every node with 0xFFFF. Not acknowledged:
//|         a node relays it ``ttl`` hops at most."""
//|         ...
//|
static mp_obj_t espidf_ble_mesh_send(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_self, ARG_dest, ARG_data, ARG_ttl };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_self, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_dest, MP_ARG_REQUIRED | MP_ARG_INT },
        { MP_QSTR_data, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_ttl, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 3 } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    get_running(args[ARG_self].u_obj);
    if (!_bound) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("Not part of a mesh yet"));
    }
    mp_buffer_info_t data;
    mp_get_buffer_raise(args[ARG_data].u_obj, &data, MP_BUFFER_READ);
    mp_arg_validate_length_max(data.len, MESH_MAX_DATA, MP_QSTR_data);
    esp_ble_mesh_msg_ctx_t ctx = {
        .net_idx = ESP_BLE_MESH_NET_PRIMARY,
        .app_idx = APP_IDX,
        .addr = mp_arg_validate_int_range(args[ARG_dest].u_int, 1, 0xFFFF, MP_QSTR_dest),
        .send_ttl = mp_arg_validate_int_range(args[ARG_ttl].u_int, 0, 127, MP_QSTR_ttl),
    };
    CHECK_ESP_RESULT(esp_ble_mesh_server_model_send_msg(&_vnd_models[0], &ctx, VND_OP_DATA, data.len, data.buf));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(espidf_ble_mesh_send_obj, 3, espidf_ble_mesh_send);

//|     def read(self) -> Optional[Tuple[int, int, int, int, bytes]]:
//|         """The oldest message not yet read, or None: ``(src, dest, rssi,
//|         ttl, data)``. ``rssi`` is of the last hop. Sixteen are kept; later
//|         ones are counted in `dropped`."""
//|         ...
//|
static mp_obj_t espidf_ble_mesh_read(mp_obj_t self_in) {
    get_running(self_in);
    mesh_msg_t *msg;
    if (xQueueReceive(_queue, &msg, 0) != pdTRUE) {
        return mp_const_none;
    }
    mp_obj_t items[5] = {
        MP_OBJ_NEW_SMALL_INT(msg->src),
        MP_OBJ_NEW_SMALL_INT(msg->dst),
        MP_OBJ_NEW_SMALL_INT(msg->rssi),
        MP_OBJ_NEW_SMALL_INT(msg->ttl),
        mp_const_none,
    };
    // Freed even if making the bytes object raises.
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        items[4] = mp_obj_new_bytes(msg->data, msg->len);
        nlr_pop();
    } else {
        free(msg);
        nlr_jump(nlr.ret_val);
    }
    free(msg);
    return mp_obj_new_tuple(5, items);
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_ble_mesh_read_obj, espidf_ble_mesh_read);

//|     ready: bool
//|     """True once this device can send and receive: for a node, provisioned
//|     and its vendor model bound to the application key."""
//|
//|     address: Optional[int]
//|     """This device's unicast address, or None before it is provisioned."""
//|
//|     uuid: bytes
//|     """The 16-byte uuid this device shows to a provisioner."""
//|
//|     dropped: int
//|     """Messages lost because `read` did not keep up."""
//|
static mp_obj_t espidf_ble_mesh_get_ready(mp_obj_t self_in) {
    get_running(self_in);
    return mp_obj_new_bool(_bound);
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_ble_mesh_get_ready_obj, espidf_ble_mesh_get_ready);
MP_PROPERTY_GETTER(espidf_ble_mesh_ready_obj, (mp_obj_t)&espidf_ble_mesh_get_ready_obj);

static mp_obj_t espidf_ble_mesh_get_address(mp_obj_t self_in) {
    get_running(self_in);
    return _address == 0 ? mp_const_none : MP_OBJ_NEW_SMALL_INT(_address);
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_ble_mesh_get_address_obj, espidf_ble_mesh_get_address);
MP_PROPERTY_GETTER(espidf_ble_mesh_address_obj, (mp_obj_t)&espidf_ble_mesh_get_address_obj);

static mp_obj_t espidf_ble_mesh_get_uuid(mp_obj_t self_in) {
    return mp_obj_new_bytes(_uuid, sizeof(_uuid));
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_ble_mesh_get_uuid_obj, espidf_ble_mesh_get_uuid);
MP_PROPERTY_GETTER(espidf_ble_mesh_uuid_obj, (mp_obj_t)&espidf_ble_mesh_get_uuid_obj);

static mp_obj_t espidf_ble_mesh_get_dropped(mp_obj_t self_in) {
    return mp_obj_new_int_from_uint(_dropped);
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_ble_mesh_get_dropped_obj, espidf_ble_mesh_get_dropped);
MP_PROPERTY_GETTER(espidf_ble_mesh_dropped_obj, (mp_obj_t)&espidf_ble_mesh_get_dropped_obj);

//|     def deinit(self) -> None:
//|         """Stop the mesh stack. The network is forgotten."""
//|         ...
//|
static mp_obj_t espidf_ble_mesh_deinit(mp_obj_t self_in) {
    mesh_stop(MP_OBJ_TO_PTR(self_in));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_ble_mesh_deinit_obj, espidf_ble_mesh_deinit);

static mp_obj_t espidf_ble_mesh___exit__(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    return espidf_ble_mesh_deinit(args[0]);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(espidf_ble_mesh___exit___obj, 4, 4, espidf_ble_mesh___exit__);

static const mp_rom_map_elem_t espidf_ble_mesh_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_add_node), MP_ROM_PTR(&espidf_ble_mesh_add_node_obj) },
    { MP_ROM_QSTR(MP_QSTR_send), MP_ROM_PTR(&espidf_ble_mesh_send_obj) },
    { MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&espidf_ble_mesh_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_ready), MP_ROM_PTR(&espidf_ble_mesh_ready_obj) },
    { MP_ROM_QSTR(MP_QSTR_address), MP_ROM_PTR(&espidf_ble_mesh_address_obj) },
    { MP_ROM_QSTR(MP_QSTR_uuid), MP_ROM_PTR(&espidf_ble_mesh_uuid_obj) },
    { MP_ROM_QSTR(MP_QSTR_dropped), MP_ROM_PTR(&espidf_ble_mesh_dropped_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&espidf_ble_mesh_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&espidf_ble_mesh_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&espidf_ble_mesh___exit___obj) },
};
static MP_DEFINE_CONST_DICT(espidf_ble_mesh_locals_dict, espidf_ble_mesh_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    espidf_ble_mesh_type,
    MP_QSTR_BLEMesh,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, espidf_ble_mesh_make_new,
    locals_dict, &espidf_ble_mesh_locals_dict
    );

void espidf_ble_mesh_reset(void) {
    if (_mesh != NULL) {
        mesh_stop(_mesh);
    }
}

#endif
