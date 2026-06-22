/* SPDX-FileCopyrightText: 2026 Jonah Walker */
/* SPDX-License-Identifier: Apache-2.0 */

//! In-RAM circular log buffer. Every line that goes to the serial console is also
//! captured here, so the full firmware log can be pulled over plain GATT (the debug
//! log service) WITHOUT a PPoG/console session -- i.e. even when a connection fails
//! to fully come up. This is what makes the firmware observable in the states where
//! the normal console is unavailable.

#pragma once

#include <stdint.h>

//! Append a NUL-terminated string to the ring (called from the serial-console sink,
//! so it captures all PBL_LOG output). Lock-free + bounded: never crashes, worst case
//! a rare interleave under concurrency.
void log_ring_append(const char *str);

//! Number of logical bytes currently available (<= the ring size).
uint32_t log_ring_count(void);

//! Linearized read: copy up to maxlen bytes starting at logical `offset` (0 = oldest
//! byte still in the ring). Returns the number of bytes copied.
uint32_t log_ring_read(uint32_t offset, uint8_t *out, uint32_t maxlen);
