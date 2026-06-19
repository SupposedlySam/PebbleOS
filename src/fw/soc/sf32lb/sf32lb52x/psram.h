/* SPDX-FileCopyrightText: 2026 Jonah Walker */
/* SPDX-License-Identifier: Apache-2.0 */

//! obelix (SF32LB52J) PSRAM: 16 MB OPI, MPI1, mapped at 0x60000000. Brought up
//! on demand by the `psram` console command (sf32lb/sf32lb52x/psram.c), then
//! used to back the Dart/WAMR allocation pool.

#pragma once

#include <stdbool.h>

#define SF32LB52_PSRAM_BASE 0x60000000u
#define SF32LB52_PSRAM_SIZE (16u * 1024u * 1024u)

//! True once `psram` has brought up + verified the controller.
bool sf32lb52_psram_is_ready(void);
