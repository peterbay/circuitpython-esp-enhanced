// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-License-Identifier: MIT

#include "shared-bindings/registers/__init__.h"

uint64_t shared_registers_buf_to_u64(const uint8_t *buf, size_t len, bool lsb_first) {
    uint64_t reg = 0;
    if (lsb_first) {
        // buf[0] is the least significant byte.
        for (size_t i = 0; i < len; i++) {
            reg |= (uint64_t)buf[i] << (8 * i);
        }
    } else {
        // buf[0] is the most significant byte.
        for (size_t i = 0; i < len; i++) {
            reg = (reg << 8) | buf[i];
        }
    }
    return reg;
}

void shared_registers_u64_to_buf(uint8_t *buf, size_t len, uint64_t value, bool lsb_first) {
    if (lsb_first) {
        for (size_t i = 0; i < len; i++) {
            buf[i] = value & 0xFF;
            value >>= 8;
        }
    } else {
        for (size_t i = len; i-- > 0;) {
            buf[i] = value & 0xFF;
            value >>= 8;
        }
    }
}
