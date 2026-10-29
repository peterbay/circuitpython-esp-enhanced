// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Petr Vavrin
//
// SPDX-License-Identifier: MIT

// espidf.bitscramble(); see extended.h.

#include <string.h>

#include "py/obj.h"
#include "py/runtime.h"

#include "bindings/espidf/__init__.h"
#include "bindings/espidf/extended.h"

// The headers below exist only on chips that have the peripheral.
#if CIRCUITPY_ESPIDF_BITSCRAMBLER

#include "driver/bitscrambler.h"
#include "driver/bitscrambler_loopback.h"
#include "esp_heap_caps.h"
#include "hal/bitscrambler_ll.h"
#include "hal/bitscrambler_peri_select.h"

// The BitScrambler: a small programmable unit in a DMA path that rearranges
// bits on the way through -- byte and bit order, packing, lookup tables -- at
// DMA speed. Here in its memory-to-memory form. Programs are written in
// Espressif's .bsasm language and assembled on a computer with the IDF's
// tools/bsasm.py; Python hands over the resulting binary.

#define BITSCRAMBLER_MAX_BYTES (65536)
// The driver's v1 image header, which it reads without being told the image's
// length; checked here first so that a short or corrupt buffer cannot make it
// read past the end.
#define HDR_BYTES (12)
#define MAX_INSTRUCTIONS (8)
#define MAX_LUT_WORDS (2048)

static size_t check_program(const uint8_t *p, size_t len) {
    if (len < HDR_BYTES) {
        mp_raise_ValueError(MP_ERROR_TEXT("program too short"));
    }
    size_t hdr_words = p[2];
    size_t instructions = p[3];
    size_t lut_words = p[4] | (p[5] << 8);
    if (p[0] != 1 || p[1] != 0 || hdr_words * 4 < HDR_BYTES) {
        mp_raise_ValueError(MP_ERROR_TEXT("not a BitScrambler v1 program"));
    }
    if (instructions > MAX_INSTRUCTIONS || lut_words > MAX_LUT_WORDS) {
        mp_raise_ValueError(MP_ERROR_TEXT("program too large"));
    }
    size_t need = (hdr_words + instructions * BITSCRAMBLER_LL_INST_LEN_WORDS + lut_words) * 4;
    if (need > len) {
        mp_raise_ValueError(MP_ERROR_TEXT("program truncated"));
    }
    return need;
}

//| def bitscramble(program: ReadableBuffer, data: ReadableBuffer, out: WriteableBuffer) -> int:
//|     """Run ``data`` through a BitScrambler program into ``out``, by DMA, and
//|     return how many bytes it wrote. Up to 65536 bytes each way.
//|
//|     ``program`` is the binary from the IDF's assembler, e.g.
//|     ``python tools/bsasm.py swap16.bsasm swap16.bsbin -c
//|     components/esp_driver_bitscrambler/bsasm_targets/esp32c5.json``.
//|     ESP32-C5 only. Uses the BitScrambler of the UHCI DMA channel, which
//|     CircuitPython does not otherwise use."""
//|     ...
//|
//|
static mp_obj_t espidf_bitscramble(mp_obj_t program_in, mp_obj_t data_in, mp_obj_t out_in) {
    mp_buffer_info_t program, data, out;
    mp_get_buffer_raise(program_in, &program, MP_BUFFER_READ);
    mp_get_buffer_raise(data_in, &data, MP_BUFFER_READ);
    mp_get_buffer_raise(out_in, &out, MP_BUFFER_WRITE);
    size_t program_len = check_program(program.buf, program.len);
    mp_arg_validate_length_range(data.len, 1, BITSCRAMBLER_MAX_BYTES, MP_QSTR_data);
    mp_arg_validate_length_range(out.len, 1, BITSCRAMBLER_MAX_BYTES, MP_QSTR_out);

    // Internal, DMA-capable and word-aligned: the driver reads the LUT as
    // 32-bit words and the DMA cannot reach PSRAM here.
    uint32_t caps = MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL;
    uint8_t *prog = heap_caps_aligned_alloc(4, program_len, MALLOC_CAP_INTERNAL);
    uint8_t *in = heap_caps_aligned_alloc(4, data.len, caps);
    uint8_t *result_buf = heap_caps_aligned_alloc(4, out.len, caps);
    bitscrambler_handle_t bs = NULL;
    esp_err_t err = (prog && in && result_buf) ? ESP_OK : ESP_ERR_NO_MEM;
    size_t written = 0;
    if (err == ESP_OK) {
        memcpy(prog, program.buf, program_len);
        memcpy(in, data.buf, data.len);
        size_t max = data.len > out.len ? data.len : out.len;
        err = bitscrambler_loopback_create(&bs, SOC_BITSCRAMBLER_ATTACH_UHCI, max);
    }
    if (err == ESP_OK) {
        err = bitscrambler_load_program(bs, prog);
    }
    if (err == ESP_OK) {
        err = bitscrambler_loopback_run(bs, in, data.len, result_buf, out.len, &written);
    }
    if (err == ESP_OK) {
        memcpy(out.buf, result_buf, written);
    }
    if (bs != NULL) {
        bitscrambler_free(bs);
    }
    heap_caps_free(prog);
    heap_caps_free(in);
    heap_caps_free(result_buf);
    if (err == ESP_ERR_NO_MEM) {
        m_malloc_fail(data.len + out.len);
    }
    CHECK_ESP_RESULT(err);
    return mp_obj_new_int_from_uint(written);
}
MP_DEFINE_CONST_FUN_OBJ_3(espidf_bitscramble_obj, espidf_bitscramble);

#endif
