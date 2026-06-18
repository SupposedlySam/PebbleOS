/* SPDX-FileCopyrightText: 2026 Jonah Walker */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdint.h>

#include "wasm_export.h"

//! Returns the `dart:_embedder` native symbol table for the "dart" module.
//! @param[out] count_out number of symbols.
NativeSymbol *dart_embedder_get_natives(uint32_t *count_out);
