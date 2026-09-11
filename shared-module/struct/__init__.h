// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2017 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT
#pragma once

// CIRCUITPY-CHANGE: calcsize_items was declared here although it is static in
// shared-module/struct/__init__.c; it is gone now, its work folded into the
// walk that computes the size.
char get_fmt_type(const char **fmt);
mp_uint_t get_fmt_num(const char **p);
