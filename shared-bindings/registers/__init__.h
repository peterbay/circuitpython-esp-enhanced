// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// Read a register's bytes into one integer, honouring the byte order.
uint64_t shared_registers_buf_to_u64(const uint8_t *buf, size_t len, bool lsb_first);

// Write an integer back into a register's bytes, honouring the byte order.
void shared_registers_u64_to_buf(uint8_t *buf, size_t len, uint64_t value, bool lsb_first);
