// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-License-Identifier: MIT

#include <string.h>

#include "py/objproperty.h"
#include "py/runtime.h"
#include "shared-bindings/util.h"
#include "shared/runtime/context_manager_helpers.h"

#include "bindings/zigbee/Endpoint.h"
#include "bindings/zigbee/Stack.h"

#include "esp_zigbee.h"
#include "ezbee/af.h"
#include "ezbee/aps.h"
#include "ezbee/app_signals.h"
#include "ezbee/bdb.h"
#include "ezbee/core.h"
#include "ezbee/nwk.h"
#include "ezbee/secur.h"
#include "ezbee/zcl/cluster/basic_desc.h"
#include "ezbee/zcl/cluster/identify_desc.h"
#include "ezbee/zcl/zcl_common.h"
#include "ezbee/zcl/zcl_core.h"
#include "ezbee/zcl/zcl_desc.h"
#include "ezbee/zcl/cluster/custom.h"
#include "ezbee/zdo/zdo_dev_srv_disc.h"
#include "ezbee/zdo/zdo_nwk_mgmt.h"
#include "ezbee/zcl/zcl_general_cmd.h"
#include "ezbee/zcl/zcl_type.h"
#include "ezbee/zha.h"

#include "py/mperrno.h"
#include "supervisor/shared/tick.h"

#include "esp_heap_caps.h"
#include "esp_ieee802154.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Public ESP-IDF, but its header is not on this port's include path.
extern esp_err_t esp_vfs_eventfd_unregister(void);

#define ZIGBEE_EVENT_QUEUE (16)
// Endpoints a single device may carry. The specification allows 240; this is
// what one script would plausibly build, and each costs a descriptor.
#define ZIGBEE_MAX_ENDPOINTS (8)
#define ZIGBEE_ATTR_QUEUE (16)
// Enough for every fixed-width ZCL type and for a short string. Anything longer
// is stored truncated, with the true length kept, rather than dropped.
#define ZIGBEE_ATTR_VALUE_MAX (32)

// What produced an attribute record.
#define ZIGBEE_ATTR_READ_RESPONSE (0)
#define ZIGBEE_ATTR_REPORT (1)
#define ZIGBEE_ATTR_WRITTEN (2)
// A device's answer to a command sent with Stack.command(), and to a write.
// Without these a command is sent into silence: nothing else in this module
// reports whether the far end accepted it.
#define ZIGBEE_ATTR_COMMAND_RESPONSE (3)
#define ZIGBEE_ATTR_WRITE_RESPONSE (4)

// Discovery results, kept alongside the attribute records because they arrive
// the same way: on the stack's task, in buffers the library frees straight
// after the callback returns.
#define ZIGBEE_DESC_QUEUE (8)
#define ZIGBEE_DESC_CLUSTERS_MAX (16)

typedef struct {
    uint16_t type;
    uint16_t address;
    uint8_t status;
    uint8_t action;
} zigbee_event_t;

typedef struct {
    uint16_t address;
    uint16_t cluster;
    uint16_t attribute;
    uint8_t endpoint;
    uint8_t type;
    uint8_t status;
    uint8_t kind;
    uint8_t length;
    int8_t rssi;
    uint8_t value[ZIGBEE_ATTR_VALUE_MAX];
} zigbee_attr_t;

typedef struct {
    uint16_t address;
    uint16_t profile;
    uint16_t device_type;
    uint8_t endpoint;
    uint8_t status;
    uint8_t input_count;
    uint8_t output_count;
    uint16_t clusters[ZIGBEE_DESC_CLUSTERS_MAX];
} zigbee_desc_t;

typedef struct {
    mp_obj_base_t base;
    bool started;
    uint32_t channels;
    // Written by the stack's task, read by the VM. Single producer, single
    // consumer, and both indices are updated after the slot they refer to, so
    // no lock is needed for what this stores.
    volatile uint8_t head;
    volatile uint8_t tail;
    volatile uint16_t lost;
    // Getting an address is not the same as being let in. The Trust Center
    // decides afterwards, and a device can hold an address for a while before
    // being told to leave -- so "joined" alone reads as success too early.
    volatile bool authorized;
    // Network state as of the last signal, refreshed on the stack's own task.
    // The library requires its lock around every call made from outside a
    // callback, and taking it for a property read is not an option either: a
    // script polling these a few times a second then holds the mutex the stack
    // itself needs, and measured that way a device stops associating at all.
    // Reading them here, where the lock is already held, avoids both.
    volatile uint16_t short_address;
    volatile uint16_t pan_id;
    volatile uint8_t channel;
    volatile uint8_t role;
    volatile bool factory_new;
    volatile bool running;
    uint8_t extended_pan_id[8];
    zigbee_event_t events[ZIGBEE_EVENT_QUEUE];
    volatile uint8_t attr_head;
    volatile uint8_t attr_tail;
    volatile uint16_t attr_lost;
    zigbee_attr_t attrs[ZIGBEE_ATTR_QUEUE];
    volatile uint8_t desc_head;
    volatile uint8_t desc_tail;
    zigbee_desc_t descs[ZIGBEE_DESC_QUEUE];
} zigbee_stack_obj_t;

// One radio, one network dataset, one stack: the library keeps all of its state
// in statics, so a second instance would not be a second anything.
static zigbee_stack_obj_t *_stack = NULL;
static TaskHandle_t _pump_task = NULL;

bool zigbee_holds_radio(void) {
    return _stack != NULL;
}

// Runs on the stack's own task. Records what happened and gets out of the way:
// building Python objects here would allocate on a heap the VM owns.
static void zigbee_pump(void *arg);

// What the task needs to bring the stack up, and how it went.
static struct {
    uint8_t role;
    uint32_t channels;
    bool require_link_key;
    volatile int8_t result;     // 0 waiting, 1 up, -1 failed
    // Endpoints have to be built between init and start, which both happen on
    // the stack's task, so the objects the constructor was given are parked
    // here rather than reached through the Python object.
    zigbee_endpoint_obj_t *endpoints[ZIGBEE_MAX_ENDPOINTS];
    uint8_t endpoint_count;
    volatile esp_err_t error;
    // How much internal RAM was left once the stack was up. The reference
    // examples have a whole chip to themselves; here Wi-Fi, BLE and
    // CircuitPython are running first, and a stack that cannot allocate
    // fails quietly rather than saying so.
} _setup;

static bool signal_handler(const ezb_app_signal_t *signal) {
    zigbee_stack_obj_t *self = _stack;
    if (self == NULL) {
        return false;
    }
    ezb_app_signal_type_t kind = ezb_app_signal_get_type(signal);

    // Drive commissioning from here rather than from Python. The stack expects
    // the application to answer these three signals, and starting the stack
    // with autostart instead means the first-start signal never arrives at all
    // -- so nothing ever forms or joins. This runs on the stack's own task, so
    // it must not take the lock: it is already inside it.
    {
        const uint8_t *bdb = ezb_app_signal_get_params(signal);
        uint8_t status = bdb ? *bdb : 0xFF;
        switch (kind) {
            case EZB_ZDO_SIGNAL_SKIP_STARTUP:
                ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_INITIALIZATION);
                break;
            case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
            case EZB_BDB_SIGNAL_DEVICE_REBOOT:
                if (status != EZB_BDB_STATUS_SUCCESS) {
                    break;
                }
                if (ezb_bdb_is_factory_new()) {
                    // Nothing stored: a coordinator makes a network, anything
                    // else goes looking for one.
                    ezb_bdb_start_top_level_commissioning(
                        _setup.role == EZB_NWK_DEVICE_TYPE_COORDINATOR
                            ? EZB_BDB_MODE_NETWORK_FORMATION
                            : EZB_BDB_MODE_NETWORK_STEERING);
                } else if (_setup.role != EZB_NWK_DEVICE_TYPE_END_DEVICE) {
                    // Back on a network it already had: open it, rather than
                    // run steering. Steering on a device that is already on a
                    // network takes a different path through the commissioning
                    // state machine, and a coordinator that went that way keeps
                    // its network open but never finishes admitting anyone --
                    // every joining device is answered with "ignore".
                    ezb_bdb_open_network(180);
                }
                break;
            case EZB_BDB_SIGNAL_FORMATION:
                // Opening a network is steering run on a device already on one.
                if (status == EZB_BDB_STATUS_SUCCESS) {
                    ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
                }
                break;
            default:
                break;
        }
    }

    if (kind == EZB_ZDO_SIGNAL_DEVICE_AUTHORIZED) {
        self->authorized = true;
    } else if (kind == EZB_ZDO_SIGNAL_LEAVE) {
        self->authorized = false;
    }

    // Every signal is a state change worth recording, and this is the one place
    // the stack can be asked safely.
    self->short_address = ezb_get_short_address();
    self->pan_id = ezb_get_panid();
    self->channel = ezb_get_current_channel();
    self->role = (uint8_t)ezb_nwk_get_device_type();
    self->factory_new = ezb_bdb_is_factory_new();
    self->running = ezb_dev_is_started();
    ezb_extpanid_t epid;
    ezb_get_extended_panid(&epid);
    memcpy(self->extended_pan_id, &epid, sizeof(self->extended_pan_id));

    uint8_t next = (self->head + 1) % ZIGBEE_EVENT_QUEUE;
    if (next == self->tail) {
        self->lost++;
    } else {
        ezb_app_signal_type_t type = ezb_app_signal_get_type(signal);
        const void *params = ezb_app_signal_get_params(signal);
        zigbee_event_t *event = &self->events[self->head];
        event->type = (uint16_t)type;
        event->address = 0xFFFF;
        event->action = 0;
        // Each signal carries its own structure. Reading the first byte and
        // calling it a status only works for the BDB ones; for the rest it is
        // the first byte of an address, which reads as a plausible-looking
        // number that means nothing.
        switch (type) {
            case EZB_ZDO_SIGNAL_LEAVE:
                event->status = params
                    ? ((const ezb_zdo_signal_leave_params_t *)params)->leave_type : 0;
                break;
            case EZB_ZDO_SIGNAL_DEVICE_ANNCE:
                event->status = 0;
                if (params) {
                    event->address =
                        ((const ezb_zdo_signal_device_annce_params_t *)params)->short_addr;
                }
                break;
            case EZB_ZDO_SIGNAL_DEVICE_UPDATE:
                if (params) {
                    const ezb_zdo_signal_device_update_params_t *p = params;
                    event->status = p->status;
                    event->address = p->short_addr;
                    // What the Trust Center decided to do about this device,
                    // which is the only thing that says why one gets dropped.
                    event->action = p->tc_action;
                } else {
                    event->status = 0;
                }
                break;
            case EZB_ZDO_SIGNAL_DEVICE_AUTHORIZED:
                if (params) {
                    const ezb_zdo_signal_device_authorized_params_t *p = params;
                    event->status = p->status;
                    event->address = p->short_addr;
                    event->action = p->type;
                } else {
                    event->status = 0;
                }
                break;
            default:
                // The BDB signals and ZDO error, whose parameters start with a
                // one-byte status.
                event->status = params ? *(const uint8_t *)params : 0;
                break;
        }
        self->head = next;
    }
    // true: the signal is handled here. The header is explicit -- "true if the
    // application signal is handled" -- and every example the library ships
    // returns it. Returning false leaves the stack running its own default
    // handling on top of this handler's, so two commissioning drivers act on
    // the same signal; a coordinator built that way forms a network, opens it,
    // and then answers every join with "ignore".
    return true;
}

// Accept ZCL commands rather than leaving them unanswered. Nothing is done
// with them yet; what matters is that the endpoint responds at all.
// Discovery answers. Both run on the stack's task and both are handed buffers
// the library frees as soon as they return, so everything wanted is copied out
// before then. The endpoint list arrives first; each endpoint is then asked for
// its descriptor, which is what says what the device actually is.
static void simple_desc_result(const ezb_zdo_simple_desc_req_result_t *result, void *user_ctx) {
    (void)user_ctx;
    zigbee_stack_obj_t *self = _stack;
    if (self == NULL || result == NULL || result->rsp == NULL) {
        return;
    }
    uint8_t next = (self->desc_head + 1) % ZIGBEE_DESC_QUEUE;
    if (next == self->desc_tail) {
        return;
    }
    const ezb_af_simple_desc_t *desc = &result->rsp->desc;
    zigbee_desc_t *record = &self->descs[self->desc_head];
    record->address = result->rsp->nwk_addr_of_interest;
    record->status = result->rsp->status;
    record->endpoint = desc->ep_id;
    record->profile = desc->app_profile_id;
    record->device_type = desc->app_device_id;
    record->input_count = desc->app_input_cluster_count;
    record->output_count = desc->app_output_cluster_count;
    uint16_t total = (uint16_t)desc->app_input_cluster_count + desc->app_output_cluster_count;
    if (total > ZIGBEE_DESC_CLUSTERS_MAX) {
        total = ZIGBEE_DESC_CLUSTERS_MAX;
        // The counts are left as reported, so a truncated list is visible
        // rather than looking like the whole of a shorter one.
    }
    for (uint16_t i = 0; i < total && desc->app_cluster_list != NULL; i++) {
        record->clusters[i] = desc->app_cluster_list[i];
    }
    self->desc_head = next;
}

static void active_ep_result(const ezb_zdo_active_ep_req_result_t *result, void *user_ctx) {
    (void)user_ctx;
    if (result == NULL || result->rsp == NULL || result->rsp->active_ep_list == NULL) {
        return;
    }
    // Asking for each endpoint's descriptor from inside this callback is what
    // the library's own gateway example does: it is already on the stack's task
    // and holding the lock.
    for (uint8_t i = 0; i < result->rsp->active_ep_count; i++) {
        ezb_zdo_simple_desc_req_t req = {
            .dst_nwk_addr = result->rsp->nwk_addr_of_interest,
            .field = {
                .nwk_addr_of_interest = result->rsp->nwk_addr_of_interest,
                .endpoint = result->rsp->active_ep_list[i],
            },
            .cb = simple_desc_result,
        };
        ezb_zdo_simple_desc_req(&req);
    }
}

// One record per attribute, copied out of the message before the callback
// returns: the library owns those buffers and frees them straight afterwards.
static void record_attr(uint16_t address, uint8_t endpoint, int8_t rssi,
    uint16_t cluster, uint8_t kind, uint16_t attr_id, uint8_t type, uint8_t status,
    const void *value) {
    zigbee_stack_obj_t *self = _stack;
    if (self == NULL) {
        return;
    }
    uint8_t next = (self->attr_head + 1) % ZIGBEE_ATTR_QUEUE;
    if (next == self->attr_tail) {
        self->attr_lost++;
        return;
    }
    zigbee_attr_t *record = &self->attrs[self->attr_head];
    record->address = address;
    record->endpoint = endpoint;
    record->rssi = rssi;
    record->cluster = cluster;
    record->kind = kind;
    record->attribute = attr_id;
    record->type = type;
    record->status = status;
    record->length = 0;
    if (value != NULL) {
        // The stored width comes from the type. A string carries its own length
        // in the first byte; everything else is fixed.
        uint16_t size = ezb_zcl_get_attr_value_size(type, value);
        if (size > ZIGBEE_ATTR_VALUE_MAX) {
            size = ZIGBEE_ATTR_VALUE_MAX;
        }
        memcpy(record->value, value, size);
        record->length = (uint8_t)size;
    }
    self->attr_head = next;
}

// Attribute values arrive here: answers to Stack.read(), unsolicited reports
// from a device that was told to send them, and writes another device made to
// this one. Everything else the stack asks about is left at its default, which
// is what its own examples do.
static void zcl_action_handler(ezb_zcl_core_action_callback_id_t callback_id,
    void *message) {
    switch (callback_id) {
        case EZB_ZCL_CORE_READ_ATTR_RSP_CB_ID: {
            const ezb_zcl_cmd_read_attr_rsp_message_t *msg = message;
            const ezb_zcl_cmd_hdr_t *hdr = msg->in.header;
            for (const ezb_zcl_read_attr_rsp_variable_t *v = msg->in.variables;
                 v != NULL; v = v->next) {
                record_attr(hdr ? hdr->src_addr.u.short_addr : 0xFFFF,
                    hdr ? hdr->src_ep : 0, hdr ? hdr->rssi : 0,
                    msg->info.cluster_id, ZIGBEE_ATTR_READ_RESPONSE,
                    v->attr_id, v->attr_type, v->status, v->attr_value);
            }
            break;
        }
        case EZB_ZCL_CORE_REPORT_ATTR_CB_ID: {
            const ezb_zcl_cmd_report_attr_message_t *msg = message;
            const ezb_zcl_cmd_hdr_t *hdr = msg->in.header;
            for (const ezb_zcl_report_attr_variable_t *v = msg->in.variables;
                 v != NULL; v = v->next) {
                record_attr(hdr ? hdr->src_addr.u.short_addr : 0xFFFF,
                    hdr ? hdr->src_ep : 0, hdr ? hdr->rssi : 0,
                    msg->info.cluster_id, ZIGBEE_ATTR_REPORT,
                    v->attr_id, v->attr_type, 0, v->attr_value);
            }
            break;
        }
        case EZB_ZCL_CORE_DEFAULT_RSP_CB_ID: {
            // What a device says about a command it was sent. The command id
            // travels where an attribute id normally would, because that is the
            // only thing this answer identifies.
            const ezb_zcl_cmd_default_rsp_message_t *msg = message;
            const ezb_zcl_cmd_hdr_t *hdr = msg->in.header;
            record_attr(hdr ? hdr->src_addr.u.short_addr : 0xFFFF,
                hdr ? hdr->src_ep : 0, hdr ? hdr->rssi : 0,
                msg->info.cluster_id, ZIGBEE_ATTR_COMMAND_RESPONSE,
                msg->in.rsp_to_cmd, 0, msg->in.status_code, NULL);
            break;
        }
        case EZB_ZCL_CORE_WRITE_ATTR_RSP_CB_ID: {
            const ezb_zcl_cmd_write_attr_rsp_message_t *msg = message;
            const ezb_zcl_cmd_hdr_t *hdr = msg->in.header;
            for (const ezb_zcl_write_attr_rsp_variable_t *v = msg->in.variables;
                 v != NULL; v = v->next) {
                record_attr(hdr ? hdr->src_addr.u.short_addr : 0xFFFF,
                    hdr ? hdr->src_ep : 0, hdr ? hdr->rssi : 0,
                    msg->info.cluster_id, ZIGBEE_ATTR_WRITE_RESPONSE,
                    v->attr_id, 0, v->status, NULL);
            }
            break;
        }
        case EZB_ZCL_CORE_SET_ATTR_VALUE_CB_ID: {
            const ezb_zcl_set_attr_value_message_t *msg = message;
            record_attr(0xFFFF, msg->info.dst_ep, 0, msg->info.cluster_id,
                ZIGBEE_ATTR_WRITTEN, msg->in.attribute.id, msg->in.attribute.data.type,
                0, msg->in.attribute.data.value);
            break;
        }
        default:
            break;
    }
}

// The stack is brought up inside its own task, not from the VM's, and this is
// not a preference: the library's own examples do all of init, configuration,
// endpoint registration and start in the task that then runs the main loop.
// Doing the setup from one task and the loop from another leaves the Trust
// Center answering every join with silence -- a device gets an address, never
// gets the network key, and leaves, with nothing but a device-update action of
// "ignore" to say so.
//
// esp_zigbee_launch_mainloop() does not return while the stack is up.
static void zigbee_pump(void *arg) {
    (void)arg;

    esp_zigbee_config_t config = {
        .device_config = {
            .device_type = (ezb_nwk_device_type_t)_setup.role,
            .install_code_policy = false,
        },
        .platform_config = {
            .storage_partition_name = "nvs",
            .radio_config = { .radio_mode = ESP_ZIGBEE_RADIO_MODE_NATIVE },
        },
    };
    // The two role configurations share a union, so only the one that matches
    // the role may be written. Filling the coordinator's member for an end
    // device puts its child count where the ageing timeout belongs and leaves
    // the keep-alive as whatever was on the stack.
    if (_setup.role == EZB_NWK_DEVICE_TYPE_END_DEVICE) {
        config.device_config.zed_config.ed_timeout = EZB_NWK_ED_TIMEOUT_64MIN;
        config.device_config.zed_config.keep_alive = 3000;
    } else {
        config.device_config.zczr_config.max_children = 10;
    }

    // The eventfd VFS has to be handed back before the stack will initialise,
    // and nothing says so: it returns ESP_ERR_INVALID_STATE with no log line.
    // The platform layer tolerates finding the VFS registered but then opens a
    // descriptor through it, and one registered by somebody else -- here,
    // CircuitPython -- does not have the room the stack asked for, so the open
    // fails and takes the whole init with it.
    //
    // An esp_zigbee_deinit() used to sit in front of this, from when the cause
    // was still unknown. Deinitialising a stack that was never initialised is
    // not free.
    esp_err_t err = esp_zigbee_init(&config);
    if (err == ESP_OK) {

        // Exactly what the library's own coordinator does, and nothing else.
        // Three security settings used to be forced here -- install codes off,
        // the Trust Center link key exchange off, rejoins with the well-known
        // key on -- each added as a guess at why joins were being ignored. None
        // of them helped and the reference sets none of them; leaving the
        // defaults alone is the only version that matches something known to
        // work. require_link_key is applied further down, and only when asked
        // for.
        ezb_aps_secur_enable_distributed_security(false);
        ezb_bdb_set_primary_channel_set(_setup.channels);
        // The whole band as the fallback, which is what the library's examples
        // use. An empty secondary set leaves commissioning with nowhere to go
        // when the primary one turns up nothing.
        ezb_bdb_set_secondary_channel_set(0x07FFF800);
        ezb_app_signal_add_handler(signal_handler);
        if (_setup.require_link_key) {
            ezb_secur_set_tclk_exchange_required(true);
        }

        // Every Zigbee device needs at least one endpoint, and Zigbee 3.0
        // requires Basic and Identify on it.
        // Endpoints are built here because this is the only window in which
        // they may be registered: after esp_zigbee_init() and before
        // esp_zigbee_start(). A script that passed none gets a Configuration
        // Tool, which is the ZHA device type for something that manages a
        // network rather than being an appliance.
        ezb_af_device_desc_t device = ezb_af_create_device_desc();
        if (_setup.endpoint_count == 0) {
            ezb_zha_configuration_tool_config_t tool_cfg = EZB_ZHA_CONFIGURATION_TOOL_CONFIG();
            ezb_af_ep_desc_t endpoint = ezb_zha_create_configuration_tool(1, &tool_cfg);
            ezb_zcl_cluster_desc_t basic =
                ezb_af_endpoint_get_cluster_desc(endpoint, EZB_ZCL_CLUSTER_ID_BASIC,
                    EZB_ZCL_CLUSTER_SERVER);
            // Length-prefixed Zigbee strings, not C strings.
            ezb_zcl_basic_cluster_desc_add_attr(basic,
                EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, (void *)"\x0d" "CircuitPython");
            ezb_zcl_basic_cluster_desc_add_attr(basic,
                EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, (void *)"\x08" "ESP32-C5");
            ezb_af_device_add_endpoint_desc(device, endpoint);
        } else {
            for (uint8_t i = 0; i < _setup.endpoint_count; i++) {
                ezb_af_ep_desc_t endpoint = zigbee_endpoint_build(_setup.endpoints[i]);
                if (endpoint == NULL) {
                    err = ESP_ERR_INVALID_ARG;
                    break;
                }
                ezb_zcl_cluster_desc_t basic =
                    ezb_af_endpoint_get_cluster_desc(endpoint, EZB_ZCL_CLUSTER_ID_BASIC,
                        EZB_ZCL_CLUSTER_SERVER);
                if (basic != NULL) {
                    ezb_zcl_basic_cluster_desc_add_attr(basic,
                        EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, (void *)"\x0d" "CircuitPython");
                    ezb_zcl_basic_cluster_desc_add_attr(basic,
                        EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, (void *)"\x08" "ESP32-C5");
                }
                ezb_af_device_add_endpoint_desc(device, endpoint);
                _setup.endpoints[i]->registered = true;
            }
        }
        ezb_af_device_desc_register(device);
        // Registered even though nothing is done with the commands yet: the
        // library's examples all register one, and an endpoint whose commands
        // nobody accepts is part of what a joining device is judged on.
        ezb_zcl_core_action_handler_register(zcl_action_handler);

        // false, not true: commissioning is driven from the signal handler,
        // which is the sequence the library expects. With autostart the stack
        // runs its own and never reports the first-start signal at all.
        err = esp_zigbee_start(false);

    }

    _setup.error = err;
    _setup.result = (err == ESP_OK) ? 1 : -1;

    if (err == ESP_OK) {
        // Returns when esp_zigbee_stop() is called from elsewhere.
        esp_zigbee_launch_mainloop();
        esp_zigbee_deinit();
    }
    _pump_task = NULL;
    vTaskDelete(NULL);
}

static void check_ezb_err(ezb_err_t err, const char *what) {
    if (err != EZB_ERR_NONE) {
        mp_raise_msg_varg(&mp_type_RuntimeError, MP_ERROR_TEXT("%s failed (%d)"),
            what, (int)err);
    }
}

static void check_esp_err_named(esp_err_t err, const char *what) {
    if (err != ESP_OK) {
        // The numeric code as well as the name: the stack returns values that
        // are not in esp_err_to_name's table, and "UNKNOWN ERROR" on its own
        // says nothing about which call failed or why.
        mp_raise_msg_varg(&mp_type_RuntimeError, MP_ERROR_TEXT("%s failed: %s (0x%x)"),
            what, esp_err_to_name(err), (unsigned int)err);
    }
}

static zigbee_stack_obj_t *native_stack(mp_obj_t self_in) {
    zigbee_stack_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->started) {
        raise_deinited_error();
    }
    return self;
}

// For anything that reads or drives the stack itself. Until start() has run
// there is no stack to ask -- the library keeps its state in statics that
// nothing has filled in yet, and calling into it faults the board rather than
// returning an error.
static zigbee_stack_obj_t *running_stack(mp_obj_t self_in) {
    zigbee_stack_obj_t *self = native_stack(self_in);
    if (_setup.result != 1) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("call start() first"));
    }
    return self;
}

// Everything that drives the stack from the VM's task takes this lock, as the
// library's own examples do. Plain reads deliberately do not: a script polling
// joined/channel/address a few times a second would hold the mutex the stack
// itself needs, and measured with the reads locked a device stops associating
// altogether. Without it a device joins and is dropped again
// moments later, over and over, because two tasks are walking the same
// commissioning state machine.
void zigbee_lock(void) {
    if (!esp_zigbee_lock_acquire(pdMS_TO_TICKS(1000))) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("the Zigbee stack is busy"));
    }
}

void zigbee_unlock(void) {
    esp_zigbee_lock_release();
}

static mp_obj_t zigbee_stack_make_new(const mp_obj_type_t *type, size_t n_args,
    size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_role, ARG_channels, ARG_endpoints, ARG_require_link_key };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_role, MP_ARG_KW_ONLY | MP_ARG_INT,
          { .u_int = EZB_NWK_DEVICE_TYPE_COORDINATOR } },
        { MP_QSTR_channels, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 0x07FFF800 } },
        { MP_QSTR_endpoints, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = mp_const_none } },
        { MP_QSTR_require_link_key, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = false } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args),
        allowed_args, args);

    if (_stack != NULL) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("Zigbee stack already in use"));
    }
    mp_int_t role = mp_arg_validate_int_range(args[ARG_role].u_int,
        EZB_NWK_DEVICE_TYPE_COORDINATOR, EZB_NWK_DEVICE_TYPE_END_DEVICE, MP_QSTR_role);
    uint32_t channels = (uint32_t)args[ARG_channels].u_int & 0x07FFF800;
    if (channels == 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("channels must name at least one of 11 to 26"));
    }

    zigbee_stack_obj_t *self = mp_obj_malloc(zigbee_stack_obj_t, &zigbee_stack_type);
    self->started = true;
    self->channels = channels;
    self->head = 0;
    self->tail = 0;
    self->lost = 0;
    self->attr_head = 0;
    self->attr_tail = 0;
    self->attr_lost = 0;
    self->desc_head = 0;
    self->desc_tail = 0;
    self->authorized = false;
    self->short_address = 0xFFFF;
    self->pan_id = 0xFFFF;
    self->channel = 0;
    self->role = (uint8_t)role;
    self->factory_new = true;
    self->running = false;
    memset(self->extended_pan_id, 0, sizeof(self->extended_pan_id));
    // Published before the stack can signal anything, because the handler reads it.
    _stack = self;

    _setup.endpoint_count = 0;
    if (args[ARG_endpoints].u_obj != mp_const_none) {
        size_t count;
        mp_obj_t *items;
        mp_obj_get_array(args[ARG_endpoints].u_obj, &count, &items);
        if (count > ZIGBEE_MAX_ENDPOINTS) {
            mp_raise_ValueError(MP_ERROR_TEXT("too many endpoints"));
        }
        for (size_t i = 0; i < count; i++) {
            zigbee_endpoint_obj_t *endpoint =
                mp_arg_validate_type(items[i], &zigbee_endpoint_type, MP_QSTR_endpoints);
            for (size_t j = 0; j < i; j++) {
                if (_setup.endpoints[j]->endpoint_id == endpoint->endpoint_id) {
                    mp_raise_ValueError(MP_ERROR_TEXT("two endpoints with the same endpoint_id"));
                }
            }
            endpoint->registered = false;
            _setup.endpoints[i] = endpoint;
        }
        _setup.endpoint_count = (uint8_t)count;
    }

    _setup.role = (uint8_t)role;
    _setup.channels = channels;
    _setup.require_link_key = args[ARG_require_link_key].u_bool;
    _setup.result = 0;
    _setup.error = ESP_OK;

    return MP_OBJ_FROM_PTR(self);
}

//|     def deinit(self) -> None:
//|         """Stop the stack and give the radio back."""
//|         ...
static mp_obj_t zigbee_stack_deinit(mp_obj_t self_in) {
    zigbee_stack_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->started) {
        self->started = false;
        _stack = NULL;
        ezb_app_signal_remove_handler(signal_handler);
        if (_pump_task != NULL) {
            // esp_zigbee_stop() makes the main loop return, and the task then
            // deinitialises and deletes itself. Wait for it rather than delete
            // it here: a task killed inside the stack takes the stack's lock
            // and radio state with it.
            esp_zigbee_stop();
            uint64_t deadline = supervisor_ticks_ms64() + 2000;
            while (_pump_task != NULL && supervisor_ticks_ms64() < deadline) {
                RUN_BACKGROUND_TASKS;
            }
            if (_pump_task != NULL) {
                vTaskDelete(_pump_task);
                _pump_task = NULL;
                esp_zigbee_deinit();
            }
        }
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(zigbee_stack_deinit_obj, zigbee_stack_deinit);

void zigbee_reset(void) {
    if (_stack != NULL) {
        zigbee_stack_deinit(MP_OBJ_FROM_PTR(_stack));
    }
}

static mp_obj_t zigbee_stack___exit__(size_t n_args, const mp_obj_t *args) {
    return zigbee_stack_deinit(args[0]);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(zigbee_stack___exit___obj, 4, 4,
    zigbee_stack___exit__);

//|     def start(self) -> None:
//|         """Bring the stack up and begin commissioning.
//|
//|         A coordinator forms a network, or resumes the one it already has. A
//|         router or end device looks for one to join, which only works while
//|         some coordinator has its network open -- see `open_network`.
//|
//|         This blocks only until the stack is running; forming or joining
//|         happens afterwards and is reported through `event`."""
//|         ...
static mp_obj_t zigbee_stack_start(mp_obj_t self_in) {
    native_stack(self_in);
    if (_pump_task != NULL) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("already started"));
    }
    // The whole of init happens on the task, not here. See zigbee_pump().
    // 8 kB rather than the 4 kB the library's examples use: all of init runs
    // here, including endpoint registration, and the stack's own commissioning
    // callbacks run on top of it.
    if (xTaskCreate(zigbee_pump, "zigbee", 8192, NULL, 5, &_pump_task) != pdPASS) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("could not start the Zigbee task"));
    }
    uint64_t deadline = supervisor_ticks_ms64() + 5000;
    while (_setup.result == 0) {
        RUN_BACKGROUND_TASKS;
        if (supervisor_ticks_ms64() > deadline) {
            mp_raise_OSError(MP_ETIMEDOUT);
        }
    }
    if (_setup.result < 0) {
        check_esp_err_named(_setup.error, "esp_zigbee_init");
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(zigbee_stack_start_obj, zigbee_stack_start);

//|     lost_events: int
//|     """Events dropped because nothing read them in time."""
static mp_obj_t zigbee_stack_get_lost_events(mp_obj_t self_in) {
    zigbee_stack_obj_t *self = running_stack(self_in);
    return MP_OBJ_NEW_SMALL_INT(self->lost);
}
static MP_DEFINE_CONST_FUN_OBJ_1(zigbee_stack_get_lost_events_obj,
    zigbee_stack_get_lost_events);
MP_PROPERTY_GETTER(zigbee_stack_lost_events_obj,
    (mp_obj_t)&zigbee_stack_get_lost_events_obj);

//|     role: int
//|     """`COORDINATOR`, `ROUTER` or `END_DEVICE`.
//|
//|     Can be set, but only while the device is not on a network."""
static mp_obj_t zigbee_stack_get_role(mp_obj_t self_in) {
    zigbee_stack_obj_t *self = running_stack(self_in);
    return MP_OBJ_NEW_SMALL_INT(self->role);
}
static MP_DEFINE_CONST_FUN_OBJ_1(zigbee_stack_get_role_obj, zigbee_stack_get_role);

static mp_obj_t zigbee_stack_set_role(mp_obj_t self_in, mp_obj_t value) {
    running_stack(self_in);
    mp_int_t role = mp_arg_validate_int_range(mp_obj_get_int(value),
        EZB_NWK_DEVICE_TYPE_COORDINATOR, EZB_NWK_DEVICE_TYPE_END_DEVICE, MP_QSTR_role);
    zigbee_lock();
    ezb_err_t err = ezb_nwk_set_device_type((ezb_nwk_device_type_t)role);
    zigbee_unlock();
    check_ezb_err(err, "ezb_nwk_set_device_type");
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(zigbee_stack_set_role_obj, zigbee_stack_set_role);
MP_PROPERTY_GETSET(zigbee_stack_role_obj,
    (mp_obj_t)&zigbee_stack_get_role_obj,
    (mp_obj_t)&zigbee_stack_set_role_obj);

//|     started: bool
//|     """True once the stack is running, whether or not it is on a network."""
static mp_obj_t zigbee_stack_get_started(mp_obj_t self_in) {
    zigbee_stack_obj_t *self = running_stack(self_in);
    return mp_obj_new_bool(self->running);
}
static MP_DEFINE_CONST_FUN_OBJ_1(zigbee_stack_get_started_obj, zigbee_stack_get_started);
MP_PROPERTY_GETTER(zigbee_stack_started_obj, (mp_obj_t)&zigbee_stack_get_started_obj);

//|     joined: bool
//|     """True once this device has a network address, which means it is on a
//|     network. 0xFFFF means it is not."""
static mp_obj_t zigbee_stack_get_joined(mp_obj_t self_in) {
    zigbee_stack_obj_t *self = running_stack(self_in);
    return mp_obj_new_bool(self->short_address != 0xFFFF);
}
static MP_DEFINE_CONST_FUN_OBJ_1(zigbee_stack_get_joined_obj, zigbee_stack_get_joined);
MP_PROPERTY_GETTER(zigbee_stack_joined_obj, (mp_obj_t)&zigbee_stack_get_joined_obj);

//|     short_address: int
//|     """This device's 16-bit network address, 0xFFFF when it has none. A
//|     coordinator that has formed a network is always 0x0000."""
static mp_obj_t zigbee_stack_get_short_address(mp_obj_t self_in) {
    zigbee_stack_obj_t *self = running_stack(self_in);
    return MP_OBJ_NEW_SMALL_INT(self->short_address);
}
static MP_DEFINE_CONST_FUN_OBJ_1(zigbee_stack_get_short_address_obj,
    zigbee_stack_get_short_address);
MP_PROPERTY_GETTER(zigbee_stack_short_address_obj,
    (mp_obj_t)&zigbee_stack_get_short_address_obj);

//|     channel: int
//|     """The channel in use, 11 to 26."""
static mp_obj_t zigbee_stack_get_channel(mp_obj_t self_in) {
    zigbee_stack_obj_t *self = running_stack(self_in);
    return MP_OBJ_NEW_SMALL_INT(self->channel);
}
static MP_DEFINE_CONST_FUN_OBJ_1(zigbee_stack_get_channel_obj, zigbee_stack_get_channel);
MP_PROPERTY_GETTER(zigbee_stack_channel_obj, (mp_obj_t)&zigbee_stack_get_channel_obj);

//|     pan_id: int
//|     """The network this device belongs to, 0xFFFF when it is on none."""
static mp_obj_t zigbee_stack_get_pan_id(mp_obj_t self_in) {
    zigbee_stack_obj_t *self = running_stack(self_in);
    return MP_OBJ_NEW_SMALL_INT(self->pan_id);
}
static MP_DEFINE_CONST_FUN_OBJ_1(zigbee_stack_get_pan_id_obj, zigbee_stack_get_pan_id);
MP_PROPERTY_GETTER(zigbee_stack_pan_id_obj, (mp_obj_t)&zigbee_stack_get_pan_id_obj);

//|     extended_pan_id: bytes
//|     """The network's 8-byte identifier, which unlike `pan_id` does not change
//|     when the network moves channel."""
static mp_obj_t zigbee_stack_get_extended_pan_id(mp_obj_t self_in) {
    zigbee_stack_obj_t *self = running_stack(self_in);
    return mp_obj_new_bytes(self->extended_pan_id, sizeof(self->extended_pan_id));
}
static MP_DEFINE_CONST_FUN_OBJ_1(zigbee_stack_get_extended_pan_id_obj,
    zigbee_stack_get_extended_pan_id);
MP_PROPERTY_GETTER(zigbee_stack_extended_pan_id_obj,
    (mp_obj_t)&zigbee_stack_get_extended_pan_id_obj);

//|     extended_address: bytes
//|     """This device's own 8-byte address."""
static mp_obj_t zigbee_stack_get_extended_address(mp_obj_t self_in) {
    running_stack(self_in);
    ezb_extaddr_t addr;
    zigbee_lock();
    ezb_get_extended_address(&addr);
    zigbee_unlock();
    return mp_obj_new_bytes((const uint8_t *)&addr, sizeof(addr));
}
static MP_DEFINE_CONST_FUN_OBJ_1(zigbee_stack_get_extended_address_obj,
    zigbee_stack_get_extended_address);
MP_PROPERTY_GETTER(zigbee_stack_extended_address_obj,
    (mp_obj_t)&zigbee_stack_get_extended_address_obj);

//|     tx_power: int
//|     """Transmit power in dBm."""
static mp_obj_t zigbee_stack_get_tx_power(mp_obj_t self_in) {
    running_stack(self_in);
    int8_t power = 0;
    ezb_get_tx_power(&power);
    return MP_OBJ_NEW_SMALL_INT(power);
}
static MP_DEFINE_CONST_FUN_OBJ_1(zigbee_stack_get_tx_power_obj, zigbee_stack_get_tx_power);

static mp_obj_t zigbee_stack_set_tx_power(mp_obj_t self_in, mp_obj_t value) {
    running_stack(self_in);
    zigbee_lock();
    ezb_set_tx_power((int8_t)mp_obj_get_int(value));
    zigbee_unlock();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(zigbee_stack_set_tx_power_obj, zigbee_stack_set_tx_power);
MP_PROPERTY_GETSET(zigbee_stack_tx_power_obj,
    (mp_obj_t)&zigbee_stack_get_tx_power_obj,
    (mp_obj_t)&zigbee_stack_set_tx_power_obj);

//|     def open_network(self, seconds: int = 180) -> None:
//|         """Let devices join, for this many seconds.
//|
//|         Only a coordinator or router can do this, and only while it is on a
//|         network. Commissioning opens the network by itself when it forms or
//|         resumes one; this is for opening it again once that window has
//|         closed."""
//|         ...
static mp_obj_t zigbee_stack_open_network(size_t n_args, const mp_obj_t *args) {
    running_stack(args[0]);
    mp_int_t seconds = (n_args > 1) ? mp_arg_validate_int_range(
        mp_obj_get_int(args[1]), 1, 254, MP_QSTR_seconds) : 180;
    zigbee_lock();
    ezb_err_t err = ezb_bdb_open_network((uint8_t)seconds);
    zigbee_unlock();
    check_ezb_err(err, "ezb_bdb_open_network");
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(zigbee_stack_open_network_obj, 1, 2,
    zigbee_stack_open_network);

//|     def close_network(self) -> None:
//|         """Stop accepting new devices."""
//|         ...
static mp_obj_t zigbee_stack_close_network(mp_obj_t self_in) {
    running_stack(self_in);
    zigbee_lock();
    ezb_err_t err = ezb_bdb_close_network();
    zigbee_unlock();
    check_ezb_err(err, "ezb_bdb_close_network");
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(zigbee_stack_close_network_obj, zigbee_stack_close_network);

//|     def factory_reset(self) -> None:
//|         """Forget the network and everything about it.
//|
//|         The stored dataset, the keys and any devices this one knew about all
//|         go. The library restarts the device to apply it, so nothing after
//|         this call runs."""
//|         ...
static mp_obj_t zigbee_stack_factory_reset(mp_obj_t self_in) {
    running_stack(self_in);
    zigbee_lock();
    ezb_bdb_reset_via_local_action();
    zigbee_unlock();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(zigbee_stack_factory_reset_obj, zigbee_stack_factory_reset);

//|     def steer(self) -> None:
//|         """Look for a network to join.
//|
//|         Commissioning does this by itself at start-up. This is for trying
//|         again after that attempt found nothing."""
//|         ...
static mp_obj_t zigbee_stack_steer(mp_obj_t self_in) {
    running_stack(self_in);
    zigbee_lock();
    ezb_err_t err = ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
    zigbee_unlock();
    check_ezb_err(err, "ezb_bdb_start_top_level_commissioning");
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(zigbee_stack_steer_obj, zigbee_stack_steer);

//|     def form(self) -> None:
//|         """Make a network. Coordinators only.
//|
//|         Commissioning does this by itself at start-up. This is for trying
//|         again after that attempt failed."""
//|         ...
static mp_obj_t zigbee_stack_form(mp_obj_t self_in) {
    running_stack(self_in);
    zigbee_lock();
    ezb_err_t err = ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_FORMATION);
    zigbee_unlock();
    check_ezb_err(err, "ezb_bdb_start_top_level_commissioning");
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(zigbee_stack_form_obj, zigbee_stack_form);

//|     factory_new: bool
//|     """True when there is no stored network, so the next start forms or
//|     joins one rather than resuming."""
static mp_obj_t zigbee_stack_get_factory_new(mp_obj_t self_in) {
    zigbee_stack_obj_t *self = running_stack(self_in);
    return mp_obj_new_bool(self->factory_new);
}
static MP_DEFINE_CONST_FUN_OBJ_1(zigbee_stack_get_factory_new_obj,
    zigbee_stack_get_factory_new);
MP_PROPERTY_GETTER(zigbee_stack_factory_new_obj,
    (mp_obj_t)&zigbee_stack_get_factory_new_obj);

//|     authorized: bool
//|     """True once the Trust Center has admitted a device.
//|
//|     Read this on the coordinator. The stack raises the signal behind it on
//|     the Trust Center, which is the side that does the admitting -- a device
//|     being admitted is never told in those words, and this stays False there.
//|     What a joining device can see is `joined`, and that it is not asked to
//|     leave again.
//|
//|     On the coordinator it is not the same as `joined`: a device is given an
//|     address first and only then given the network key, and one that is never
//|     given the key holds an address for a while before giving up."""
static mp_obj_t zigbee_stack_get_authorized(mp_obj_t self_in) {
    zigbee_stack_obj_t *self = running_stack(self_in);
    return mp_obj_new_bool(self->authorized);
}
static MP_DEFINE_CONST_FUN_OBJ_1(zigbee_stack_get_authorized_obj,
    zigbee_stack_get_authorized);
MP_PROPERTY_GETTER(zigbee_stack_authorized_obj,
    (mp_obj_t)&zigbee_stack_get_authorized_obj);

//|     distributed_security: bool
//|     """True when the network has no Trust Center and every router shares the
//|     key, rather than one coordinator deciding who gets in.
//|
//|     A centralized network is the usual kind and the one a coordinator
//|     forms."""
static mp_obj_t zigbee_stack_get_distributed_security(mp_obj_t self_in) {
    running_stack(self_in);
    zigbee_lock();
    bool distributed = ezb_aps_secur_is_distributed_security();
    zigbee_unlock();
    return mp_obj_new_bool(distributed);
}
static MP_DEFINE_CONST_FUN_OBJ_1(zigbee_stack_get_distributed_security_obj,
    zigbee_stack_get_distributed_security);

static mp_obj_t zigbee_stack_set_distributed_security(mp_obj_t self_in, mp_obj_t value) {
    running_stack(self_in);
    zigbee_lock();
    ezb_aps_secur_enable_distributed_security(mp_obj_is_true(value));
    zigbee_unlock();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(zigbee_stack_set_distributed_security_obj,
    zigbee_stack_set_distributed_security);
MP_PROPERTY_GETSET(zigbee_stack_distributed_security_obj,
    (mp_obj_t)&zigbee_stack_get_distributed_security_obj,
    (mp_obj_t)&zigbee_stack_set_distributed_security_obj);

//|     def event(self) -> dict | None:
//|         """Take the oldest event, or None when nothing is waiting.
//|
//|         The keys are ``name`` (what the stack calls the signal), ``type``,
//|         ``status``, ``action`` and ``address``. ``action`` carries the Trust
//|         Center's decision on a device update: 0 accepted, 1 denied, 2
//|         ignored. ``address`` is 0xFFFF when the signal is not about another
//|         device."""
//|         ...
static mp_obj_t zigbee_stack_event(mp_obj_t self_in) {
    zigbee_stack_obj_t *self = running_stack(self_in);
    if (self->tail == self->head) {
        return mp_const_none;
    }
    zigbee_event_t event = self->events[self->tail];
    self->tail = (self->tail + 1) % ZIGBEE_EVENT_QUEUE;

    const char *name = ezb_app_signal_to_string((ezb_app_signal_type_t)event.type);
    mp_obj_t dict = mp_obj_new_dict(5);
    mp_obj_dict_store(dict, MP_ROM_QSTR(MP_QSTR_name),
        mp_obj_new_str(name, strlen(name)));
    mp_obj_dict_store(dict, MP_ROM_QSTR(MP_QSTR_type), MP_OBJ_NEW_SMALL_INT(event.type));
    mp_obj_dict_store(dict, MP_ROM_QSTR(MP_QSTR_status), MP_OBJ_NEW_SMALL_INT(event.status));
    mp_obj_dict_store(dict, MP_ROM_QSTR(MP_QSTR_action), MP_OBJ_NEW_SMALL_INT(event.action));
    mp_obj_dict_store(dict, MP_ROM_QSTR(MP_QSTR_address), MP_OBJ_NEW_SMALL_INT(event.address));
    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_1(zigbee_stack_event_obj, zigbee_stack_event);

//|     def read(self, address: int, endpoint: int, cluster: int,
//|              attributes: int | tuple[int, ...], *, source_endpoint: int = 1,
//|              role: int = 0) -> None:
//|         """Ask another device for one or more of its attributes.
//|
//|         Nothing is returned here: the request is queued and the answers
//|         arrive later, one record per attribute, through `report`. A call that
//|         returns has been accepted for sending, which is not the same as
//|         having been transmitted or delivered. A device that is asleep or out
//|         of range simply never answers.
//|
//|         :param int address: the device's 16-bit network address.
//|         :param int endpoint: which of its endpoints to ask.
//|         :param int cluster: which cluster on that endpoint.
//|         :param attributes: one attribute id, or several.
//|         :param int source_endpoint: which of this device's endpoints the
//|           request comes from. A read is originated from the client side of a
//|           cluster, so that endpoint needs a client-role descriptor for the
//|           cluster being read -- `Endpoint.clusters` with `CLIENT` lists
//|           exactly what it may ask about. Without one the call raises
//|           straight away, with "not found", and nothing is transmitted.
//|           `CUSTOM_GATEWAY` is exempt from the check and may read anything,
//|           which is what makes it the type for a coordinator.
//|         :param int role: `SERVER` to read a server cluster, which is the
//|           usual direction."""
//|         ...
static mp_obj_t zigbee_stack_read(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_address, ARG_endpoint, ARG_cluster, ARG_attributes, ARG_source_endpoint, ARG_role };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_address, MP_ARG_REQUIRED | MP_ARG_INT },
        { MP_QSTR_endpoint, MP_ARG_REQUIRED | MP_ARG_INT },
        { MP_QSTR_cluster, MP_ARG_REQUIRED | MP_ARG_INT },
        { MP_QSTR_attributes, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_source_endpoint, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 1 } },
        { MP_QSTR_role, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = EZB_ZCL_CLUSTER_SERVER } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args),
        allowed_args, args);
    running_stack(pos_args[0]);

    // One id or a sequence of them, held on the stack for the duration of the
    // call: the library copies the payload before returning.
    uint16_t ids[16];
    size_t count;
    if (mp_obj_is_int(args[ARG_attributes].u_obj)) {
        ids[0] = (uint16_t)mp_obj_get_int(args[ARG_attributes].u_obj);
        count = 1;
    } else {
        mp_obj_t *items;
        mp_obj_get_array(args[ARG_attributes].u_obj, &count, &items);
        if (count == 0 || count > MP_ARRAY_SIZE(ids)) {
            mp_raise_ValueError(MP_ERROR_TEXT("attributes must name 1 to 16 attributes"));
        }
        for (size_t i = 0; i < count; i++) {
            ids[i] = (uint16_t)mp_obj_get_int(items[i]);
        }
    }

    ezb_zcl_read_attr_cmd_t cmd = {
        .cmd_ctrl = {
            .dst_addr = {
                .addr_mode = EZB_ADDR_MODE_SHORT,
                .u = { .short_addr = (uint16_t)args[ARG_address].u_int },
            },
            .dst_ep = (uint8_t)args[ARG_endpoint].u_int,
            .src_ep = (uint8_t)args[ARG_source_endpoint].u_int,
            .cluster_id = (uint16_t)args[ARG_cluster].u_int,
            .manuf_code = EZB_ZCL_STD_MANUF_CODE,
            // direction 0 is towards a server cluster, which is where values
            // live; 1 asks a client.
            .fc = { .direction = (args[ARG_role].u_int == EZB_ZCL_CLUSTER_CLIENT) ? 1 : 0 },
        },
        .payload = { .attr_number = (uint8_t)count, .attr_field = ids },
    };

    zigbee_lock();
    ezb_err_t err = ezb_zcl_read_attr_cmd_req(&cmd);
    zigbee_unlock();
    check_ezb_err(err, "ezb_zcl_read_attr_cmd_req");
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(zigbee_stack_read_obj, 4, zigbee_stack_read);

//|     def write(self, address: int, endpoint: int, cluster: int, attribute: int,
//|               attribute_type: int, value: bool | int | float | ReadableBuffer, *,
//|               source_endpoint: int = 1, role: int = 0) -> None:
//|         """Write an attribute on another device.
//|
//|         The same rule as `read` applies to the source endpoint: the clusters
//|         it may write are its client-role ones, and `CUSTOM_GATEWAY` is exempt.
//|
//|         ``attribute_type`` is the ZCL type of the attribute, and the value is
//|         laid out for it here, so writing the wrong type is refused by the
//|         device rather than silently mangled. The device's answer arrives
//|         through `report` as a record of kind 4.
//|
//|         Most measured values are read-only from the network and will be
//|         refused: this is for settings, not for readings."""
//|         ...
static mp_obj_t zigbee_stack_write(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_address, ARG_endpoint, ARG_cluster, ARG_attribute, ARG_attribute_type,
           ARG_value, ARG_source_endpoint, ARG_role };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_address, MP_ARG_REQUIRED | MP_ARG_INT },
        { MP_QSTR_endpoint, MP_ARG_REQUIRED | MP_ARG_INT },
        { MP_QSTR_cluster, MP_ARG_REQUIRED | MP_ARG_INT },
        { MP_QSTR_attribute, MP_ARG_REQUIRED | MP_ARG_INT },
        { MP_QSTR_attribute_type, MP_ARG_REQUIRED | MP_ARG_INT },
        { MP_QSTR_value, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_source_endpoint, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 1 } },
        { MP_QSTR_role, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = EZB_ZCL_CLUSTER_SERVER } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args),
        allowed_args, args);
    running_stack(pos_args[0]);

    // Laid out here rather than by the caller, so the value is written in the
    // width and byte order the type says and a mistake is a Python error rather
    // than a device refusing something that looks right.
    uint8_t raw[256];
    uint16_t size = zigbee_value_from_python((uint8_t)args[ARG_attribute_type].u_int,
        args[ARG_value].u_obj, raw, sizeof(raw));

    ezb_zcl_attribute_t field = {
        .id = (uint16_t)args[ARG_attribute].u_int,
        .data = {
            .type = (uint8_t)args[ARG_attribute_type].u_int,
            .size = size,
            .value = raw,
        },
    };
    ezb_zcl_write_attr_cmd_t cmd = {
        .cmd_ctrl = {
            .dst_addr = {
                .addr_mode = EZB_ADDR_MODE_SHORT,
                .u = { .short_addr = (uint16_t)args[ARG_address].u_int },
            },
            .dst_ep = (uint8_t)args[ARG_endpoint].u_int,
            .src_ep = (uint8_t)args[ARG_source_endpoint].u_int,
            .cluster_id = (uint16_t)args[ARG_cluster].u_int,
            .manuf_code = EZB_ZCL_STD_MANUF_CODE,
            .fc = { .direction = (args[ARG_role].u_int == EZB_ZCL_CLUSTER_CLIENT) ? 1 : 0 },
        },
        .payload = { .attr_number = 1, .attr_field = &field },
    };

    zigbee_lock();
    ezb_err_t err = ezb_zcl_write_attr_cmd_req(&cmd);
    zigbee_unlock();
    check_ezb_err(err, "ezb_zcl_write_attr_cmd_req");
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(zigbee_stack_write_obj, 7, zigbee_stack_write);

//|     def configure_report(self, address: int, endpoint: int, cluster: int,
//|                          attribute: int, attribute_type: int, *,
//|                          minimum: int = 1, maximum: int = 60, change: int = 1,
//|                          source_endpoint: int = 1) -> None:
//|         """Ask another device to report an attribute by itself.
//|
//|         The device then sends a report when the value has moved by ``change``,
//|         but no more often than ``minimum`` seconds and at least every
//|         ``maximum`` seconds. Reports arrive through `report`, and this is the
//|         way to watch a sensor without polling it.
//|
//|         ``attribute_type`` is the ZCL type of the attribute, which the device
//|         will only accept if it matches."""
//|         ...
static mp_obj_t zigbee_stack_configure_report(size_t n_args, const mp_obj_t *pos_args,
    mp_map_t *kw_args) {
    enum { ARG_address, ARG_endpoint, ARG_cluster, ARG_attribute, ARG_attribute_type,
           ARG_minimum, ARG_maximum, ARG_change, ARG_source_endpoint };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_address, MP_ARG_REQUIRED | MP_ARG_INT },
        { MP_QSTR_endpoint, MP_ARG_REQUIRED | MP_ARG_INT },
        { MP_QSTR_cluster, MP_ARG_REQUIRED | MP_ARG_INT },
        { MP_QSTR_attribute, MP_ARG_REQUIRED | MP_ARG_INT },
        { MP_QSTR_attribute_type, MP_ARG_REQUIRED | MP_ARG_INT },
        { MP_QSTR_minimum, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 1 } },
        { MP_QSTR_maximum, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 60 } },
        { MP_QSTR_change, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 1 } },
        { MP_QSTR_source_endpoint, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 1 } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args),
        allowed_args, args);
    running_stack(pos_args[0]);

    // The reportable change is a union as wide as the widest attribute, and
    // the record is only asking the other device to send: the client side of
    // the union is the one that carries intervals.
    ezb_zcl_config_report_record_t record = {
        .direction = EZB_ZCL_REPORTING_SEND,
        .attr_id = (uint16_t)args[ARG_attribute].u_int,
        .client = {
            .attr_type = (uint8_t)args[ARG_attribute_type].u_int,
            .min_interval = (uint16_t)args[ARG_minimum].u_int,
            .max_interval = (uint16_t)args[ARG_maximum].u_int,
        },
    };
    // Written after the fact because the union member depends on the type, and
    // a change smaller than the attribute's width would be read past its end.
    uint32_t change = (uint32_t)args[ARG_change].u_int;
    memcpy(record.client.reportable_change.data, &change, sizeof(change));
    ezb_zcl_config_report_cmd_t cmd = {
        .cmd_ctrl = {
            .dst_addr = {
                .addr_mode = EZB_ADDR_MODE_SHORT,
                .u = { .short_addr = (uint16_t)args[ARG_address].u_int },
            },
            .dst_ep = (uint8_t)args[ARG_endpoint].u_int,
            .src_ep = (uint8_t)args[ARG_source_endpoint].u_int,
            .cluster_id = (uint16_t)args[ARG_cluster].u_int,
            .manuf_code = EZB_ZCL_STD_MANUF_CODE,
        },
        .payload = { .record_number = 1, .record_field = &record },
    };

    zigbee_lock();
    ezb_err_t err = ezb_zcl_config_report_cmd_req(&cmd);
    zigbee_unlock();
    check_ezb_err(err, "ezb_zcl_config_report_cmd_req");
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(zigbee_stack_configure_report_obj, 6,
    zigbee_stack_configure_report);

//|     def report(self) -> dict | None:
//|         """Take the oldest attribute value that arrived, or None.
//|
//|         The keys are ``address`` (which device it came from, 0xFFFF when it
//|         was this one being written to), ``endpoint``, ``cluster``,
//|         ``attribute``, ``type`` (the ZCL type), ``value``, ``status`` (0 when
//|         the read succeeded), ``rssi`` and ``kind``.
//|
//|         ``kind`` says what the record is:
//|
//|         * 0 -- an answer to `read`. ``value`` is the attribute.
//|         * 1 -- a report a device sent by itself.
//|         * 2 -- an attribute another device wrote here.
//|         * 3 -- a device's answer to a `command`. ``attribute`` holds the
//|           command id it is answering and ``status`` says whether that command
//|           was accepted; ``value`` is None. Only arrives for commands sent
//|           with ``response=True``, which is the default.
//|         * 4 -- a device's answer to a write. ``attribute`` is 0xFFFF when
//|           every attribute was written, otherwise it names one that failed.
//|
//|         Nothing here means a request was delivered, and None only means the
//|         queue is empty at this moment -- an answer may still be on its way,
//|         or may never come. See `lost_reports` for answers that arrived and
//|         were dropped."""
//|         ...
static mp_obj_t zigbee_stack_report(mp_obj_t self_in) {
    zigbee_stack_obj_t *self = running_stack(self_in);
    if (self->attr_tail == self->attr_head) {
        return mp_const_none;
    }
    zigbee_attr_t record = self->attrs[self->attr_tail];
    self->attr_tail = (self->attr_tail + 1) % ZIGBEE_ATTR_QUEUE;

    mp_obj_t dict = mp_obj_new_dict(9);
    mp_obj_dict_store(dict, MP_ROM_QSTR(MP_QSTR_address), MP_OBJ_NEW_SMALL_INT(record.address));
    mp_obj_dict_store(dict, MP_ROM_QSTR(MP_QSTR_endpoint), MP_OBJ_NEW_SMALL_INT(record.endpoint));
    mp_obj_dict_store(dict, MP_ROM_QSTR(MP_QSTR_cluster), MP_OBJ_NEW_SMALL_INT(record.cluster));
    mp_obj_dict_store(dict, MP_ROM_QSTR(MP_QSTR_attribute), MP_OBJ_NEW_SMALL_INT(record.attribute));
    mp_obj_dict_store(dict, MP_ROM_QSTR(MP_QSTR_type), MP_OBJ_NEW_SMALL_INT(record.type));
    mp_obj_dict_store(dict, MP_ROM_QSTR(MP_QSTR_status), MP_OBJ_NEW_SMALL_INT(record.status));
    mp_obj_dict_store(dict, MP_ROM_QSTR(MP_QSTR_rssi), MP_OBJ_NEW_SMALL_INT(record.rssi));
    mp_obj_dict_store(dict, MP_ROM_QSTR(MP_QSTR_kind), MP_OBJ_NEW_SMALL_INT(record.kind));
    mp_obj_dict_store(dict, MP_ROM_QSTR(MP_QSTR_value),
        zigbee_value_to_python(record.type, record.value, record.length));
    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_1(zigbee_stack_report_obj, zigbee_stack_report);

//|     lost_reports: int
//|     """Attribute values dropped because nothing read them in time."""
static mp_obj_t zigbee_stack_get_lost_reports(mp_obj_t self_in) {
    zigbee_stack_obj_t *self = running_stack(self_in);
    return MP_OBJ_NEW_SMALL_INT(self->attr_lost);
}
static MP_DEFINE_CONST_FUN_OBJ_1(zigbee_stack_get_lost_reports_obj,
    zigbee_stack_get_lost_reports);
MP_PROPERTY_GETTER(zigbee_stack_lost_reports_obj,
    (mp_obj_t)&zigbee_stack_get_lost_reports_obj);

//|     def discover(self, address: int) -> None:
//|         """Ask a device what it is.
//|
//|         Sends a ZDO active-endpoints request, and then a simple-descriptor
//|         request for each endpoint that comes back. The descriptors arrive
//|         through `descriptor`, one per endpoint.
//|
//|         Nothing is returned here, and a device that is asleep or gone simply
//|         never answers."""
//|         ...
static mp_obj_t zigbee_stack_discover(mp_obj_t self_in, mp_obj_t address_in) {
    running_stack(self_in);
    ezb_zdo_active_ep_req_t req = {
        .dst_nwk_addr = (uint16_t)mp_obj_get_int(address_in),
        .field = { .nwk_addr_of_interest = (uint16_t)mp_obj_get_int(address_in) },
        .cb = active_ep_result,
    };
    zigbee_lock();
    ezb_err_t err = ezb_zdo_active_ep_req(&req);
    zigbee_unlock();
    check_ezb_err(err, "ezb_zdo_active_ep_req");
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(zigbee_stack_discover_obj, zigbee_stack_discover);

//|     def descriptor(self) -> dict | None:
//|         """Take the oldest descriptor that arrived from `discover`, or None.
//|
//|         The keys are ``address``, ``endpoint``, ``profile``, ``device_type``
//|         (the same ZHA constants `Endpoint` is built with), ``input_clusters``
//|         and ``output_clusters``.
//|
//|         Input clusters are the ones the device answers reads on; output
//|         clusters are the ones it sends commands to."""
//|         ...
static mp_obj_t zigbee_stack_descriptor(mp_obj_t self_in) {
    zigbee_stack_obj_t *self = running_stack(self_in);
    if (self->desc_tail == self->desc_head) {
        return mp_const_none;
    }
    zigbee_desc_t record = self->descs[self->desc_tail];
    self->desc_tail = (self->desc_tail + 1) % ZIGBEE_DESC_QUEUE;

    size_t stored = record.input_count + record.output_count;
    if (stored > ZIGBEE_DESC_CLUSTERS_MAX) {
        stored = ZIGBEE_DESC_CLUSTERS_MAX;
    }
    size_t inputs = record.input_count < stored ? record.input_count : stored;
    size_t outputs = stored - inputs;

    mp_obj_t input_list[ZIGBEE_DESC_CLUSTERS_MAX];
    mp_obj_t output_list[ZIGBEE_DESC_CLUSTERS_MAX];
    for (size_t i = 0; i < inputs; i++) {
        input_list[i] = MP_OBJ_NEW_SMALL_INT(record.clusters[i]);
    }
    for (size_t i = 0; i < outputs; i++) {
        output_list[i] = MP_OBJ_NEW_SMALL_INT(record.clusters[inputs + i]);
    }

    mp_obj_t dict = mp_obj_new_dict(6);
    mp_obj_dict_store(dict, MP_ROM_QSTR(MP_QSTR_address), MP_OBJ_NEW_SMALL_INT(record.address));
    mp_obj_dict_store(dict, MP_ROM_QSTR(MP_QSTR_endpoint), MP_OBJ_NEW_SMALL_INT(record.endpoint));
    mp_obj_dict_store(dict, MP_ROM_QSTR(MP_QSTR_profile), MP_OBJ_NEW_SMALL_INT(record.profile));
    mp_obj_dict_store(dict, MP_ROM_QSTR(MP_QSTR_device_type),
        MP_OBJ_NEW_SMALL_INT(record.device_type));
    mp_obj_dict_store(dict, MP_ROM_QSTR(MP_QSTR_input_clusters),
        mp_obj_new_tuple(inputs, input_list));
    mp_obj_dict_store(dict, MP_ROM_QSTR(MP_QSTR_output_clusters),
        mp_obj_new_tuple(outputs, output_list));
    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_1(zigbee_stack_descriptor_obj, zigbee_stack_descriptor);

//|     def command(self, address: int, endpoint: int, cluster: int, command: int,
//|                 data: ReadableBuffer = b"", *, source_endpoint: int = 1,
//|                 to_client: bool = False, response: bool = True) -> None:
//|         """Send a ZCL cluster command to another device.
//|
//|         This is every cluster's command set, not a chosen few: the command id
//|         and its payload are the specification's, sent as given. Turning an
//|         outlet on is cluster 0x0006, command 0x01, no payload; off is
//|         command 0x00.
//|
//|         :param int command: the command id within the cluster.
//|         :param ReadableBuffer data: the command's payload, already laid out
//|           the way the cluster defines it.
//|         :param bool to_client: False sends to the device's server cluster,
//|           which is the usual direction.
//|         :param bool response: whether to ask for a default response.
//|
//|         This path -- the library's generic command request -- was measured
//|         not to check the sending endpoint's own clusters the way `read` does,
//|         for every cluster tried. The library's per-cluster command functions
//|         are documented as checking; this one is what the module uses, so a
//|         command is not refused locally for being sent from the wrong
//|         endpoint. Whether the far end accepts it is a separate question,
//|         answered by its default response: with ``response`` left true that
//|         arrives through `report` as a record of kind 3.
//|
//|         A call that returns has been accepted for sending. It is not proof
//|         that anything was transmitted, still less delivered."""
//|         ...
static mp_obj_t zigbee_stack_command(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_address, ARG_endpoint, ARG_cluster, ARG_command, ARG_data,
           ARG_source_endpoint, ARG_to_client, ARG_response };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_address, MP_ARG_REQUIRED | MP_ARG_INT },
        { MP_QSTR_endpoint, MP_ARG_REQUIRED | MP_ARG_INT },
        { MP_QSTR_cluster, MP_ARG_REQUIRED | MP_ARG_INT },
        { MP_QSTR_command, MP_ARG_REQUIRED | MP_ARG_INT },
        { MP_QSTR_data, MP_ARG_OBJ, { .u_obj = mp_const_none } },
        { MP_QSTR_source_endpoint, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 1 } },
        { MP_QSTR_to_client, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = false } },
        { MP_QSTR_response, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = true } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args),
        allowed_args, args);
    running_stack(pos_args[0]);

    mp_buffer_info_t payload = { .buf = NULL, .len = 0 };
    if (args[ARG_data].u_obj != mp_const_none) {
        mp_get_buffer_raise(args[ARG_data].u_obj, &payload, MP_BUFFER_READ);
    }

    ezb_zcl_custom_cmd_t cmd = {
        .cmd_ctrl = {
            .dst_addr = {
                .addr_mode = EZB_ADDR_MODE_SHORT,
                .u = { .short_addr = (uint16_t)args[ARG_address].u_int },
            },
            .dst_ep = (uint8_t)args[ARG_endpoint].u_int,
            .src_ep = (uint8_t)args[ARG_source_endpoint].u_int,
            .cluster_id = (uint16_t)args[ARG_cluster].u_int,
            .manuf_code = EZB_ZCL_STD_MANUF_CODE,
            .fc = {
                .direction = args[ARG_to_client].u_bool ? 1 : 0,
                .dis_default_rsp = args[ARG_response].u_bool ? 0 : 1,
            },
        },
        .cmd_id = (uint8_t)args[ARG_command].u_int,
        .data_length = (uint16_t)payload.len,
        .data = payload.buf,
    };

    zigbee_lock();
    ezb_err_t err = ezb_zcl_custom_cmd_req(&cmd);
    zigbee_unlock();
    check_ezb_err(err, "ezb_zcl_custom_cmd_req");
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(zigbee_stack_command_obj, 5, zigbee_stack_command);

//|     def neighbors(self) -> tuple[dict, ...]:
//|         """Every device the stack currently knows about.
//|
//|         This is the stack's own neighbour table, read directly. It is built
//|         from traffic rather than stored: after a reset it starts empty and
//|         fills over the next few seconds as routers send their link status and
//|         end devices poll their parent. Measured, a router that was already on
//|         the network appears within about four seconds. What does survive a
//|         reset is the network itself and the security state of the devices on
//|         it, which is why they rejoin without being let in again.
//|
//|         Each entry has ``address``, ``extended_address``, ``device_type``
//|         (`COORDINATOR`, `ROUTER` or `END_DEVICE`), ``relationship`` (0
//|         parent, 1 child, 2 sibling, 3 neither), ``rx_on_when_idle``,
//|         ``depth``, ``lqi``, ``rssi`` and ``age``.
//|
//|         A coordinator's own children are the entries whose relationship is
//|         1. Note that only devices that stayed are listed; one that joined and
//|         then left is not."""
//|         ...
static mp_obj_t zigbee_stack_neighbors(mp_obj_t self_in) {
    running_stack(self_in);

    // The table is bounded by the stack's own configuration, and this walks it
    // under the lock in one go rather than handing out an iterator that would
    // have to stay valid across VM calls.
    mp_obj_t found[32];
    size_t count = 0;
    ezb_nwk_info_iterator_t iterator = EZB_NWK_INFO_ITERATOR_INIT;
    ezb_nwk_neighbor_info_t info;

    while (count < MP_ARRAY_SIZE(found)) {
        zigbee_lock();
        ezb_err_t err = ezb_nwk_get_next_neighbor(&iterator, &info);
        zigbee_unlock();
        if (err != EZB_ERR_NONE) {
            break;
        }
        mp_obj_t entry = mp_obj_new_dict(9);
        mp_obj_dict_store(entry, MP_ROM_QSTR(MP_QSTR_address),
            MP_OBJ_NEW_SMALL_INT(info.short_addr));
        mp_obj_dict_store(entry, MP_ROM_QSTR(MP_QSTR_extended_address),
            mp_obj_new_bytes((const uint8_t *)&info.ieee_addr, sizeof(info.ieee_addr)));
        mp_obj_dict_store(entry, MP_ROM_QSTR(MP_QSTR_device_type),
            MP_OBJ_NEW_SMALL_INT(info.device_type));
        mp_obj_dict_store(entry, MP_ROM_QSTR(MP_QSTR_relationship),
            MP_OBJ_NEW_SMALL_INT(info.relationship));
        mp_obj_dict_store(entry, MP_ROM_QSTR(MP_QSTR_rx_on_when_idle),
            mp_obj_new_bool(info.rx_on_when_idle));
        mp_obj_dict_store(entry, MP_ROM_QSTR(MP_QSTR_depth), MP_OBJ_NEW_SMALL_INT(info.depth));
        mp_obj_dict_store(entry, MP_ROM_QSTR(MP_QSTR_lqi), MP_OBJ_NEW_SMALL_INT(info.lqi));
        mp_obj_dict_store(entry, MP_ROM_QSTR(MP_QSTR_rssi), MP_OBJ_NEW_SMALL_INT(info.rssi));
        mp_obj_dict_store(entry, MP_ROM_QSTR(MP_QSTR_age), MP_OBJ_NEW_SMALL_INT(info.age));
        found[count++] = entry;
    }
    return mp_obj_new_tuple(count, found);
}
static MP_DEFINE_CONST_FUN_OBJ_1(zigbee_stack_neighbors_obj, zigbee_stack_neighbors);

//|     network_key: bytes
//|     """The 16-byte network key, or None before there is a network.
//|
//|     Every device on the network holds this key -- it is what the Trust Center
//|     hands out when it admits one, and what secures every frame afterwards.
//|     Reading it here is how to give a sniffer something to decrypt with; see
//|     the `ieee802154` examples. Anyone holding it can read and forge traffic
//|     on this network, so it is worth the same care as the network itself.
//|
//|     A factory reset makes a new one."""
static mp_obj_t zigbee_stack_get_network_key(mp_obj_t self_in) {
    running_stack(self_in);
    uint8_t key[16];
    zigbee_lock();
    ezb_err_t err = ezb_secur_get_network_key(key);
    zigbee_unlock();
    if (err != EZB_ERR_NONE) {
        return mp_const_none;
    }
    return mp_obj_new_bytes(key, sizeof(key));
}
static MP_DEFINE_CONST_FUN_OBJ_1(zigbee_stack_get_network_key_obj,
    zigbee_stack_get_network_key);
MP_PROPERTY_GETTER(zigbee_stack_network_key_obj,
    (mp_obj_t)&zigbee_stack_get_network_key_obj);

static const mp_rom_map_elem_t zigbee_stack_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&zigbee_stack_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&zigbee_stack___exit___obj) },

    { MP_ROM_QSTR(MP_QSTR_start), MP_ROM_PTR(&zigbee_stack_start_obj) },
    { MP_ROM_QSTR(MP_QSTR_open_network), MP_ROM_PTR(&zigbee_stack_open_network_obj) },
    { MP_ROM_QSTR(MP_QSTR_close_network), MP_ROM_PTR(&zigbee_stack_close_network_obj) },
    { MP_ROM_QSTR(MP_QSTR_factory_reset), MP_ROM_PTR(&zigbee_stack_factory_reset_obj) },
    { MP_ROM_QSTR(MP_QSTR_factory_new), MP_ROM_PTR(&zigbee_stack_factory_new_obj) },
    { MP_ROM_QSTR(MP_QSTR_authorized), MP_ROM_PTR(&zigbee_stack_authorized_obj) },
    { MP_ROM_QSTR(MP_QSTR_distributed_security),
      MP_ROM_PTR(&zigbee_stack_distributed_security_obj) },
    { MP_ROM_QSTR(MP_QSTR_steer), MP_ROM_PTR(&zigbee_stack_steer_obj) },
    { MP_ROM_QSTR(MP_QSTR_form), MP_ROM_PTR(&zigbee_stack_form_obj) },
    { MP_ROM_QSTR(MP_QSTR_event), MP_ROM_PTR(&zigbee_stack_event_obj) },
    { MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&zigbee_stack_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&zigbee_stack_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_configure_report), MP_ROM_PTR(&zigbee_stack_configure_report_obj) },
    { MP_ROM_QSTR(MP_QSTR_report), MP_ROM_PTR(&zigbee_stack_report_obj) },
    { MP_ROM_QSTR(MP_QSTR_discover), MP_ROM_PTR(&zigbee_stack_discover_obj) },
    { MP_ROM_QSTR(MP_QSTR_descriptor), MP_ROM_PTR(&zigbee_stack_descriptor_obj) },
    { MP_ROM_QSTR(MP_QSTR_neighbors), MP_ROM_PTR(&zigbee_stack_neighbors_obj) },
    { MP_ROM_QSTR(MP_QSTR_command), MP_ROM_PTR(&zigbee_stack_command_obj) },
    { MP_ROM_QSTR(MP_QSTR_lost_reports), MP_ROM_PTR(&zigbee_stack_lost_reports_obj) },

    { MP_ROM_QSTR(MP_QSTR_role), MP_ROM_PTR(&zigbee_stack_role_obj) },
    { MP_ROM_QSTR(MP_QSTR_started), MP_ROM_PTR(&zigbee_stack_started_obj) },
    { MP_ROM_QSTR(MP_QSTR_joined), MP_ROM_PTR(&zigbee_stack_joined_obj) },
    { MP_ROM_QSTR(MP_QSTR_short_address), MP_ROM_PTR(&zigbee_stack_short_address_obj) },
    { MP_ROM_QSTR(MP_QSTR_channel), MP_ROM_PTR(&zigbee_stack_channel_obj) },
    { MP_ROM_QSTR(MP_QSTR_pan_id), MP_ROM_PTR(&zigbee_stack_pan_id_obj) },
    { MP_ROM_QSTR(MP_QSTR_extended_pan_id), MP_ROM_PTR(&zigbee_stack_extended_pan_id_obj) },
    { MP_ROM_QSTR(MP_QSTR_extended_address), MP_ROM_PTR(&zigbee_stack_extended_address_obj) },
    { MP_ROM_QSTR(MP_QSTR_network_key), MP_ROM_PTR(&zigbee_stack_network_key_obj) },
    { MP_ROM_QSTR(MP_QSTR_tx_power), MP_ROM_PTR(&zigbee_stack_tx_power_obj) },
    { MP_ROM_QSTR(MP_QSTR_lost_events), MP_ROM_PTR(&zigbee_stack_lost_events_obj) },
};
static MP_DEFINE_CONST_DICT(zigbee_stack_locals_dict, zigbee_stack_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    zigbee_stack_type,
    MP_QSTR_Stack,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, zigbee_stack_make_new,
    locals_dict, &zigbee_stack_locals_dict
    );
