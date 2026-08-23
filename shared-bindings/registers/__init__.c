// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-License-Identifier: MIT

#include "py/mpconfig.h"
#include "py/obj.h"
#include "py/runtime.h"

#include "shared-bindings/registers/__init__.h"

//| """`registers` module.
//|
//| Pack and unpack bit fields inside a register's bytes, in C. This is the bit
//| twiddling every register driver does in Python — read the register bytes into
//| an integer, mask and shift out a field, or splice a value back in — done once
//| in the firmware instead.
//|
//| Both functions work on a buffer holding one register (1 to 8 bytes). They do
//| no I/O: read the register into the buffer over I2C or SPI yourself, call these,
//| write it back. ``registers.Device`` (a companion Python class) builds a whole
//| named register map on top of them."""
//|
//| from circuitpython_typing import WriteableBuffer, ReadableBuffer
//|
//|
//| def extract(
//|     buffer: ReadableBuffer, mask: int, shift: int, signed: bool = False, lsb_first: bool = True
//| ) -> int:
//|     """Pull a field out of a register.
//|
//|     ``mask`` selects the field's bits in the whole register and already sits at
//|     the field's position; ``shift`` is how far to move them down to get the
//|     value (normally the index of the field's lowest bit). With ``signed`` the
//|     field is treated as two's complement and negative values come back negative.
//|     ``lsb_first`` is the register's byte order.
//|
//|     For a 3-bit field at bits 3..5 of a one-byte register: mask 0x38, shift 3."""
//|     ...
//|
//|
// Positional arguments rather than a keyword map: this runs per field in a hot
// loop, and parsing a kwargs map every call was measured to cost more than the
// bit twiddling it is meant to replace.
static mp_obj_t registers_extract(size_t n_args, const mp_obj_t *args) {
    // args: buffer, mask, shift, [signed], [lsb_first]
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(args[0], &bufinfo, MP_BUFFER_READ);
    if (bufinfo.len < 1 || bufinfo.len > 8) {
        mp_raise_ValueError(MP_ERROR_TEXT("register must be 1 to 8 bytes"));
    }
    uint64_t mask = (uint64_t)mp_obj_get_ll(args[1]);
    // A shift of 64 or more, or a negative one arriving through uint32_t as a
    // huge value, makes the shifts below undefined rather than merely wrong.
    uint32_t shift = mp_arg_validate_int_range(mp_obj_get_int(args[2]), 0, 63, MP_QSTR_shift);
    bool is_signed = n_args > 3 && mp_obj_is_true(args[3]);
    bool lsb_first = n_args <= 4 || mp_obj_is_true(args[4]);

    uint64_t reg = shared_registers_buf_to_u64(bufinfo.buf, bufinfo.len, lsb_first);
    uint64_t result = (mask == 0) ? 0 : ((reg & mask) >> shift);

    if (is_signed && mask != 0) {
        uint64_t field_mask = mask >> shift;
        uint64_t sign_bit = (field_mask + 1) >> 1;
        if (result & sign_bit) {
            // Two's complement: the value is negative.
            return mp_obj_new_int_from_ll((long long)result - (long long)(field_mask + 1));
        }
    }
    return mp_obj_new_int_from_ull(result);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(registers_extract_obj, 3, 5, registers_extract);

//| def insert(
//|     buffer: WriteableBuffer, mask: int, shift: int, value: int, lsb_first: bool = True
//| ) -> None:
//|     """Splice a field into a register, in place, leaving the other bits alone.
//|
//|     ``mask`` and ``shift`` are as in `extract`. ``value`` is shifted up by
//|     ``shift``, masked, and OR'd into the register after the field's old bits are
//|     cleared. A negative value is stored as two's complement. The buffer is
//|     modified in place."""
//|     ...
//|
//|
static mp_obj_t registers_insert(size_t n_args, const mp_obj_t *args) {
    // args: buffer, mask, shift, value, [lsb_first]
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(args[0], &bufinfo, MP_BUFFER_WRITE);
    if (bufinfo.len < 1 || bufinfo.len > 8) {
        mp_raise_ValueError(MP_ERROR_TEXT("register must be 1 to 8 bytes"));
    }
    uint64_t mask = (uint64_t)mp_obj_get_ll(args[1]);
    // See extract(): the shift feeds a << here, with the same undefined range.
    uint32_t shift = mp_arg_validate_int_range(mp_obj_get_int(args[2]), 0, 63, MP_QSTR_shift);
    uint64_t value = (uint64_t)mp_obj_get_ll(args[3]);
    bool lsb_first = n_args <= 4 || mp_obj_is_true(args[4]);

    uint64_t reg = shared_registers_buf_to_u64(bufinfo.buf, bufinfo.len, lsb_first);
    reg = (reg & ~mask) | ((value << shift) & mask);
    shared_registers_u64_to_buf(bufinfo.buf, bufinfo.len, reg, lsb_first);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(registers_insert_obj, 4, 5, registers_insert);

static const mp_rom_map_elem_t registers_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_registers) },
    { MP_ROM_QSTR(MP_QSTR_extract), MP_ROM_PTR(&registers_extract_obj) },
    { MP_ROM_QSTR(MP_QSTR_insert), MP_ROM_PTR(&registers_insert_obj) },
};
static MP_DEFINE_CONST_DICT(registers_module_globals, registers_module_globals_table);

const mp_obj_module_t registers_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&registers_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_registers, registers_module);
