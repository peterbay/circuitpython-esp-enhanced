// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Peter Vavrin
//
// SPDX-License-Identifier: MIT

#include "supervisor/board.h"

// Nothing to do at reset: the battery sense divider is only enabled when a
// program asks for it, and there is no peripheral power rail to bring up.
//
// Use the MP_WEAK supervisor/shared/board.c versions of routines not defined
// here.
