/* SPDX-FileCopyrightText: 2026 Jonah Walker */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdint.h>

#include "wasm_export.h"

//! Returns the `dart:_embedder` native symbol table for the "dart" module.
//! @param[out] count_out number of symbols.
NativeSymbol *dart_embedder_get_natives(uint32_t *count_out);

//! The most recent line passed to Dart's print() (captured for result assertion). Empty until a
//! module prints. Valid until the next print() or dart_embedder_clear_last_print().
const char *dart_embedder_last_print(void);

//! Clear the captured print() buffer (call before running a module to avoid a stale match).
void dart_embedder_clear_last_print(void);
