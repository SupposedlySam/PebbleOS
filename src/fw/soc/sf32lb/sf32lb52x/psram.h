/* SPDX-FileCopyrightText: 2026 Jonah Walker */
/* SPDX-License-Identifier: Apache-2.0 */

//! obelix (SF32LB52J) PSRAM: Winbond HYPERBUS, MPI1, mapped at 0x60000000. Brought
//! up on demand by the `psram` console command (sf32lb/sf32lb52x/psram.c), then used
//! to back the Dart/WAMR allocation pool.
//!
//! SIZE: SF32LB52_PSRAM_SIZE is the configured/maximum window. The MPI controller has
//! NO size register (it transmits full-width addresses), so a die physically smaller
//! than this ALIASES (high addresses wrap onto the low region). PID=6 ("Winbond
//! HYPERBUS 32/64/128p") does not encode density, so the REAL usable size is detected
//! at runtime by sf32lb52_psram_size() (alias-boundary probe) -- always size the pool
//! to that, never to SF32LB52_PSRAM_SIZE.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SF32LB52_PSRAM_BASE 0x60000000u
#define SF32LB52_PSRAM_SIZE (16u * 1024u * 1024u)

//! True once `psram` has brought up + verified the controller.
bool sf32lb52_psram_is_ready(void);

//! Real usable PSRAM size in bytes, detected by probing the alias-wrap boundary (the
//! die may be smaller than SF32LB52_PSRAM_SIZE). Returns 0 if PSRAM is not ready. The
//! result is cached after the first call. NOTE: the probe writes a few marker words at
//! power-of-two offsets, so call it before the region is in use (pool setup / diag).
uint32_t sf32lb52_psram_size(void);
