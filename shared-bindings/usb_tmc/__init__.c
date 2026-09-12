// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-License-Identifier: MIT

// CIRCUITPY-CHANGE: see shared-module/usb_tmc/__init__.h.

#include "py/mphal.h"
#include "py/objstr.h"
#include "py/runtime.h"

#include "shared-bindings/usb_tmc/__init__.h"
#include "shared-module/usb_tmc/__init__.h"
#include "supervisor/shared/tick.h"

//| """USB Test and Measurement Class instrument
//|
//| The `usb_tmc` module presents the board to the host as a USBTMC (USB488)
//| instrument, the class scopes and meters use, so VISA on the host talks to
//| it as it would to any SCPI instrument. Commands the host sends arrive as
//| whole messages; the board answers with whole messages.
//|
//| The interface takes one IN and one OUT endpoint. Enable it in ``boot.py``::
//|
//|     import usb_tmc
//|     usb_tmc.enable()
//|
//| and answer the host in ``code.py``::
//|
//|     import usb_tmc
//|     while True:
//|         command = usb_tmc.read()
//|         if command.strip().upper() == b"*IDN?":
//|             usb_tmc.write(b"CircuitPython,Cardputer,0,1.0\\n")
//|
//| On the host, with pyvisa and pyvisa-py::
//|
//|     import pyvisa
//|     inst = pyvisa.ResourceManager("@py").open_resource("USB0::0x303A::0x81DA::84AC345B9D03::INSTR")
//|     print(inst.query("*IDN?"))
//|
//| The host waits for a response only as long as its own timeout, two seconds
//| by default, so answer a query promptly. Windows has no driver of its own
//| for the class: bind WinUSB to the interface, with Zadig for instance, or
//| use NI-VISA.
//| """

//| def enable() -> None:
//|     """Present the instrument to the host.
//|     Can only be called in ``boot.py``, before USB is connected."""
//|     ...
//|
//|
static mp_obj_t usb_tmc_enable(void) {
    if (!common_hal_usb_tmc_enable()) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("Cannot change USB devices now"));
    }
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(usb_tmc_enable_obj, usb_tmc_enable);

//| def disable() -> None:
//|     """Do not present the instrument to the host.
//|     Can only be called in ``boot.py``, before USB is connected."""
//|     ...
//|
//|
static mp_obj_t usb_tmc_disable(void) {
    if (!common_hal_usb_tmc_disable()) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("Cannot change USB devices now"));
    }
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(usb_tmc_disable_obj, usb_tmc_disable);

//| def is_enabled() -> bool:
//|     """True when the instrument will be presented to the host."""
//|     ...
//|
//|
static mp_obj_t usb_tmc_is_enabled(void) {
    return mp_obj_new_bool(usb_tmc_enabled());
}
MP_DEFINE_CONST_FUN_OBJ_0(usb_tmc_is_enabled_obj, usb_tmc_is_enabled);

// Timeouts follow usb_cdc.Serial: None waits forever, 0 does not wait.
static bool wait_for(bool (*ready)(void), mp_obj_t timeout_obj) {
    bool forever = timeout_obj == mp_const_none;
    uint64_t timeout_ms = forever ? 0 : (uint64_t)(mp_obj_get_float(timeout_obj) * 1000);
    uint64_t start = supervisor_ticks_ms64();
    while (!ready()) {
        if (!forever && supervisor_ticks_ms64() - start >= timeout_ms) {
            return false;
        }
        RUN_BACKGROUND_TASKS;
        if (mp_hal_is_interrupted()) {
            return false;
        }
    }
    return true;
}

static bool message_ready(void) {
    return usb_tmc_message_available() > 0;
}

static bool response_sent(void) {
    return !usb_tmc_response_pending();
}

//| def read(timeout: Optional[float] = None) -> Optional[bytes]:
//|     """Return the oldest command message the host has sent, or ``None``
//|     when none arrives within ``timeout`` seconds. ``None`` waits forever,
//|     ``0`` does not wait. The message is exactly what the host sent,
//|     including any trailing newline."""
//|     ...
//|
//|
static mp_obj_t usb_tmc_read(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_timeout };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_timeout, MP_ARG_OBJ, {.u_rom_obj = mp_const_none} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    if (!wait_for(message_ready, args[ARG_timeout].u_obj)) {
        return mp_const_none;
    }
    size_t len = usb_tmc_message_available();
    vstr_t vstr;
    vstr_init_len(&vstr, len);
    vstr.len = usb_tmc_read_message((uint8_t *)vstr.buf, len);
    return mp_obj_new_bytes_from_vstr(&vstr);
}
MP_DEFINE_CONST_FUN_OBJ_KW(usb_tmc_read_obj, 0, usb_tmc_read);

//| def write(data: ReadableBuffer, timeout: Optional[float] = None) -> None:
//|     """Queue one response message for the host's next read. If an earlier
//|     response has not been collected yet, wait up to ``timeout`` seconds for
//|     that first; ``None`` waits forever, ``0`` does not wait. Raises
//|     ``OSError`` when the earlier response is still pending at the end of
//|     the wait, and ``ValueError`` when ``data`` is longer than the buffer,
//|     1024 bytes."""
//|     ...
//|
//|
static mp_obj_t usb_tmc_write(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_data, ARG_timeout };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_data, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_timeout, MP_ARG_OBJ, {.u_rom_obj = mp_const_none} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(args[ARG_data].u_obj, &bufinfo, MP_BUFFER_READ);
    mp_arg_validate_length_range(bufinfo.len, 1, USB_TMC_BUFFER_SIZE, MP_QSTR_data);
    if (!wait_for(response_sent, args[ARG_timeout].u_obj)) {
        mp_raise_OSError_msg(MP_ERROR_TEXT("Previous response not collected"));
    }
    usb_tmc_write_message(bufinfo.buf, bufinfo.len);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_KW(usb_tmc_write_obj, 1, usb_tmc_write);

//| def in_waiting() -> int:
//|     """Length of the oldest command message waiting to be read, 0 when
//|     there is none."""
//|     ...
//|
//|
static mp_obj_t usb_tmc_in_waiting(void) {
    return MP_OBJ_NEW_SMALL_INT(usb_tmc_message_available());
}
MP_DEFINE_CONST_FUN_OBJ_0(usb_tmc_in_waiting_obj, usb_tmc_in_waiting);

//| def status_byte() -> int:
//|     """The IEEE 488.2 status byte the host reads with ``read_stb()``. Bit
//|     4 (``0x10``, message available) is set while a response is pending."""
//|     ...
//|
//|
static mp_obj_t usb_tmc_obj_status_byte(void) {
    return MP_OBJ_NEW_SMALL_INT(usb_tmc_get_status_byte());
}
MP_DEFINE_CONST_FUN_OBJ_0(usb_tmc_status_byte_obj, usb_tmc_obj_status_byte);

//| def set_status_byte(value: int) -> None:
//|     """Set the status byte's other bits, such as the event status bit
//|     (``0x20``). Bit 4 is managed by the module and ignored here."""
//|     ...
//|
//|
static mp_obj_t usb_tmc_obj_set_status_byte(mp_obj_t value) {
    usb_tmc_set_status_byte((uint8_t)mp_arg_validate_int_range(mp_obj_get_int(value), 0, 255, MP_QSTR_value));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(usb_tmc_set_status_byte_obj, usb_tmc_obj_set_status_byte);

static const mp_rom_map_elem_t usb_tmc_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_usb_tmc) },
    { MP_ROM_QSTR(MP_QSTR_enable), MP_ROM_PTR(&usb_tmc_enable_obj) },
    { MP_ROM_QSTR(MP_QSTR_disable), MP_ROM_PTR(&usb_tmc_disable_obj) },
    { MP_ROM_QSTR(MP_QSTR_is_enabled), MP_ROM_PTR(&usb_tmc_is_enabled_obj) },
    { MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&usb_tmc_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&usb_tmc_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_in_waiting), MP_ROM_PTR(&usb_tmc_in_waiting_obj) },
    { MP_ROM_QSTR(MP_QSTR_status_byte), MP_ROM_PTR(&usb_tmc_status_byte_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_status_byte), MP_ROM_PTR(&usb_tmc_set_status_byte_obj) },
};

static MP_DEFINE_CONST_DICT(usb_tmc_module_globals, usb_tmc_module_globals_table);

const mp_obj_module_t usb_tmc_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&usb_tmc_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_usb_tmc, usb_tmc_module);
