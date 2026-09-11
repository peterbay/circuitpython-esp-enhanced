// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-License-Identifier: MIT

// CIRCUITPY-CHANGE: see shared-module/usb_net/__init__.h.

#include <string.h>

#include "py/objstr.h"
#include "py/runtime.h"

#include "shared-bindings/ipaddress/__init__.h"
#include "shared-bindings/ipaddress/IPv4Address.h"
#include "shared-bindings/usb_net/__init__.h"
#include "shared-module/usb_net/__init__.h"

//| """USB network interface
//|
//| The `usb_net` module presents the board to the host as an Ethernet
//| adapter over USB. The board answers on an address of its own, runs a DHCP
//| server that gives the host an address on the same link, and runs a DNS
//| server that answers for the board's name, so a browser on the host reaches
//| a server on the board by name, with no network in between.
//|
//| The protocol is CDC-NCM, which Windows 11, Linux and macOS use without a
//| driver. A build with ``CIRCUITPY_USB_NET_RNDIS = 1`` uses RNDIS instead;
//| Windows does not start that one.
//|
//| The interface costs two IN endpoints and one OUT endpoint, which is more
//| than a board with five IN endpoints has spare once the serial console, the
//| drive and HID have taken theirs. Turn off what you do not need in
//| ``boot.py``::
//|
//|     import storage, usb_hid, usb_net
//|     storage.disable_usb_drive()
//|     usb_hid.disable()
//|     usb_net.enable(hostname="cardputer.home.arpa")
//|
//| If too many interfaces are enabled, CircuitPython goes into safe mode
//| after ``boot.py`` and says so.
//|
//| A server on the board listens on the board's address, through the same
//| `socketpool` that wifi uses::
//|
//|     import socketpool, wifi
//|     from adafruit_httpserver import Server, Response
//|
//|     server = Server(socketpool.SocketPool(wifi.radio))
//|
//|     @server.route("/")
//|     def index(request):
//|         return Response(request, "Hello over USB")
//|
//|     server.serve_forever("192.168.7.1", 80)
//|
//| ``examples/usbnet_demo.py`` next to this module is a complete page.
//|
//| The name the host shows for the device as a whole is its USB product
//| name, which `supervisor.set_usb_identification` sets.
//| """

static uint32_t ipv4_to_uint32(const uint8_t address[4]) {
    return ((uint32_t)address[0] << 24) | ((uint32_t)address[1] << 16) |
           ((uint32_t)address[2] << 8) | address[3];
}

static void ipv4_from_uint32(uint32_t value, uint8_t address[4]) {
    for (size_t i = 0; i < 4; i++) {
        address[i] = (value >> (24 - 8 * i)) & 0xff;
    }
}

// On the link, and neither the link's own address nor its broadcast address.
static bool ipv4_is_host_on_link(uint32_t address, uint32_t link, uint32_t netmask) {
    uint32_t host_part = address & ~netmask;
    return (address & netmask) == (link & netmask) && host_part != 0 && host_part != ~netmask;
}

static void get_ipv4_address(mp_obj_t obj, qstr arg_name, uint8_t address[4]) {
    if (mp_obj_is_str(obj)) {
        GET_STR_DATA_LEN(obj, str_data, str_len);
        uint32_t value;
        if (!ipaddress_parse_ipv4address((const char *)str_data, str_len, &value)) {
            mp_arg_error_invalid(arg_name);
        }
        // The parser puts the first octet in the low byte.
        for (size_t i = 0; i < 4; i++) {
            address[i] = (value >> (8 * i)) & 0xff;
        }
    } else if (mp_obj_is_type(obj, &ipaddress_ipv4address_type)) {
        mp_buffer_info_t bufinfo;
        mp_get_buffer_raise(common_hal_ipaddress_ipv4address_get_packed(MP_OBJ_TO_PTR(obj)),
            &bufinfo, MP_BUFFER_READ);
        mp_arg_validate_length(bufinfo.len, 4, arg_name);
        memcpy(address, bufinfo.buf, 4);
    } else {
        mp_raise_TypeError_varg(MP_ERROR_TEXT("%q must be of type %q or %q, not %q"),
            arg_name, MP_QSTR_str, MP_QSTR_IPv4Address, mp_obj_get_type(obj)->name);
    }
}

static void get_mac_address(mp_obj_t obj, qstr arg_name, uint8_t mac[6]) {
    static const uint8_t zero[6] = { 0 };
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(obj, &bufinfo, MP_BUFFER_READ);
    mp_arg_validate_length(bufinfo.len, 6, arg_name);
    const uint8_t *given = bufinfo.buf;
    // A group address, bit 0 of the first byte, cannot be an interface's own.
    if ((given[0] & 0x01) != 0 || memcmp(given, zero, sizeof(zero)) == 0) {
        mp_arg_error_invalid(arg_name);
    }
    memcpy(mac, given, 6);
}

// Letters, digits, hyphens and dots, with no empty label, since it is typed
// into a browser and looked up in DNS.
static void get_hostname(mp_obj_t obj, char hostname[USB_NET_HOSTNAME_MAX_LEN + 1]) {
    mp_arg_validate_type_string(obj, MP_QSTR_hostname);
    GET_STR_DATA_LEN(obj, str_data, str_len);
    mp_arg_validate_length_range(str_len, 1, USB_NET_HOSTNAME_MAX_LEN, MP_QSTR_hostname);
    char previous = '.';
    for (size_t i = 0; i < str_len; i++) {
        char c = (char)str_data[i];
        bool valid = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || (c == '.' && previous != '.');
        if (!valid) {
            mp_arg_error_invalid(MP_QSTR_hostname);
        }
        previous = c;
    }
    if (previous == '.') {
        mp_arg_error_invalid(MP_QSTR_hostname);
    }
    memcpy(hostname, str_data, str_len);
    hostname[str_len] = '\0';
}

//| def enable(
//|     *,
//|     ipv4_address: Union[str, ipaddress.IPv4Address] = "192.168.7.1",
//|     netmask: Union[str, ipaddress.IPv4Address] = "255.255.255.0",
//|     host_ipv4_address: Optional[Union[str, ipaddress.IPv4Address]] = None,
//|     gateway: Optional[Union[str, ipaddress.IPv4Address]] = None,
//|     mac_address: Optional[ReadableBuffer] = None,
//|     host_mac_address: Optional[ReadableBuffer] = None,
//|     hostname: Optional[str] = None,
//| ) -> None:
//|     """Present a network interface to the host.
//|     Can only be called in ``boot.py``, before USB is connected.
//|
//|     :param ipv4_address: the board's address on the link
//|     :param netmask: the link's netmask
//|     :param host_ipv4_address: the address the DHCP server gives the host.
//|       It must be on the link and not the board's own. Defaults to the
//|       board's address plus one.
//|     :param gateway: the router the DHCP server offers the host. By default
//|       it offers none, and the host keeps sending everything that is not
//|       for the board where it did before.
//|     :param mac_address: the board's six-byte Ethernet address
//|     :param host_mac_address: the address the host gives the interface it
//|       creates. Both default to addresses derived from the processor's
//|       unique id, and the two must differ.
//|     :param hostname: the name the board answers for in DNS, such as
//|       ``"cardputer.home.arpa"``. The DHCP server always offers the board
//|       as the host's DNS server; the board answers for this name alone and
//|       refuses every other query, which sends the host to its other
//|       servers at once.
//|     """
//|     ...
//|
//|
static mp_obj_t usb_net_enable(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_ipv4_address, ARG_netmask, ARG_host_ipv4_address, ARG_gateway,
           ARG_mac_address, ARG_host_mac_address, ARG_hostname };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_ipv4_address, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = mp_const_none} },
        { MP_QSTR_netmask, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = mp_const_none} },
        { MP_QSTR_host_ipv4_address, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = mp_const_none} },
        { MP_QSTR_gateway, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = mp_const_none} },
        { MP_QSTR_mac_address, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = mp_const_none} },
        { MP_QSTR_host_mac_address, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = mp_const_none} },
        { MP_QSTR_hostname, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = mp_const_none} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    usb_net_config_t config;
    usb_net_default_config(&config);

    if (args[ARG_ipv4_address].u_obj != mp_const_none) {
        get_ipv4_address(args[ARG_ipv4_address].u_obj, MP_QSTR_ipv4_address, config.ipv4_address);
    }
    if (args[ARG_netmask].u_obj != mp_const_none) {
        get_ipv4_address(args[ARG_netmask].u_obj, MP_QSTR_netmask, config.netmask);
    }
    const uint32_t address = ipv4_to_uint32(config.ipv4_address);
    const uint32_t netmask = ipv4_to_uint32(config.netmask);
    // Ones then zeros, leaving room for the board and the host at least.
    const uint32_t host_bits = ~netmask;
    if ((host_bits & (host_bits + 1)) != 0 || host_bits < 3) {
        mp_arg_error_invalid(MP_QSTR_netmask);
    }
    if (!ipv4_is_host_on_link(address, address, netmask)) {
        mp_arg_error_invalid(MP_QSTR_ipv4_address);
    }

    if (args[ARG_host_ipv4_address].u_obj != mp_const_none) {
        get_ipv4_address(args[ARG_host_ipv4_address].u_obj, MP_QSTR_host_ipv4_address,
            config.host_ipv4_address);
    } else {
        ipv4_from_uint32(address + 1, config.host_ipv4_address);
    }
    const uint32_t host_address = ipv4_to_uint32(config.host_ipv4_address);
    if (host_address == address || !ipv4_is_host_on_link(host_address, address, netmask)) {
        mp_arg_error_invalid(MP_QSTR_host_ipv4_address);
    }

    if (args[ARG_gateway].u_obj != mp_const_none) {
        get_ipv4_address(args[ARG_gateway].u_obj, MP_QSTR_gateway, config.gateway);
        if (!ipv4_is_host_on_link(ipv4_to_uint32(config.gateway), address, netmask)) {
            mp_arg_error_invalid(MP_QSTR_gateway);
        }
    }

    if (args[ARG_mac_address].u_obj != mp_const_none) {
        get_mac_address(args[ARG_mac_address].u_obj, MP_QSTR_mac_address, config.mac_address);
    }
    if (args[ARG_host_mac_address].u_obj != mp_const_none) {
        get_mac_address(args[ARG_host_mac_address].u_obj, MP_QSTR_host_mac_address,
            config.host_mac_address);
    }
    if (memcmp(config.mac_address, config.host_mac_address, sizeof(config.mac_address)) == 0) {
        mp_arg_error_invalid(MP_QSTR_host_mac_address);
    }

    if (args[ARG_hostname].u_obj != mp_const_none) {
        get_hostname(args[ARG_hostname].u_obj, config.hostname);
    }

    if (!common_hal_usb_net_enable(&config)) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("Cannot change USB devices now"));
    }
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_KW(usb_net_enable_obj, 0, usb_net_enable);

//| def disable() -> None:
//|     """Do not present a network interface to the host.
//|     Can only be called in ``boot.py``, before USB is connected."""
//|     ...
//|
//|
static mp_obj_t usb_net_disable(void) {
    if (!common_hal_usb_net_disable()) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("Cannot change USB devices now"));
    }
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(usb_net_disable_obj, usb_net_disable);

//| def is_enabled() -> bool:
//|     """True when the network interface will be presented to the host."""
//|     ...
//|
//|
static mp_obj_t usb_net_is_enabled(void) {
    return mp_obj_new_bool(usb_net_enabled());
}
MP_DEFINE_CONST_FUN_OBJ_0(usb_net_is_enabled_obj, usb_net_is_enabled);

static const mp_rom_map_elem_t usb_net_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_usb_net) },
    { MP_ROM_QSTR(MP_QSTR_enable), MP_ROM_PTR(&usb_net_enable_obj) },
    { MP_ROM_QSTR(MP_QSTR_disable), MP_ROM_PTR(&usb_net_disable_obj) },
    { MP_ROM_QSTR(MP_QSTR_is_enabled), MP_ROM_PTR(&usb_net_is_enabled_obj) },
};

static MP_DEFINE_CONST_DICT(usb_net_module_globals, usb_net_module_globals_table);

const mp_obj_module_t usb_net_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&usb_net_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_usb_net, usb_net_module);
