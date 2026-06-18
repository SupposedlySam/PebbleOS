/* SPDX-FileCopyrightText: 2026 Jonah Walker */
/* SPDX-License-Identifier: Apache-2.0 */

//! On-device Dart runtime: runs dart2wasm --standalone WasmGC modules on WAMR,
//! so watchfaces/apps authored in Dart run natively on the watch.

#pragma once

#include <stdbool.h>
#include <stdint.h>

//! Initialize WAMR and register the `dart:_embedder` native module. Idempotent.
//! @return true on success.
bool dart_runtime_init(void);

//! Load, instantiate and run a dart2wasm --standalone module (calls
//! $invokeMain with an empty args list). Output goes to the debug serial.
//! @return true if the module ran without trapping.
bool dart_run_module(const uint8_t *wasm_buf, uint32_t wasm_size);

//! Run the built-in hello-world test module (when CONFIG_DART_RUNTIME). Useful
//! from the console to validate the runtime end to end.
bool dart_run_test_module(void);

//! Run a tiny no-GC wasm module (add(40,2)==42) to verify WAMR executes wasm in
//! the firmware. Small enough to run from SRAM (no PSRAM needed).
bool dart_run_wasm_smoketest(void);

//! Queue the wasm smoke test onto the KernelBG system task (runs off the boot
//! path; safe to call from boot without blocking it). Result is logged.
void dart_runtime_schedule_smoketest(void);
