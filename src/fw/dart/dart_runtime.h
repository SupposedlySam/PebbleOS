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

//! Start a resident Flutter app from a dart2wasm --standalone module: load,
//! instantiate, run main(), and drain the event loop so the first frame renders
//! (via the presentFrame native). The instance stays alive for input injection.
//! @return true if main() ran and the first frame was pumped without trapping.
bool dart_app_start(const uint8_t *wasm_buf, uint32_t wasm_size);

//! Deliver a tap at physical pixel (x, y) to the resident app (calls its
//! injectTap export), then drain the event loop so the resulting frame renders.
//! No-op if no app is running or it has no injectTap export.
//! @return true if the tap dispatched without trapping.
bool dart_app_inject_tap(double x, double y);

//! Tear down the resident app (exec env, instance, module, RAM copy). Idempotent.
void dart_app_stop(void);

//! Run a tiny no-GC wasm module (add(40,2)==42) to verify WAMR executes wasm in
//! the firmware. Small enough to run from SRAM (no PSRAM needed).
bool dart_run_wasm_smoketest(void);

//! Queue the wasm smoke test onto the KernelBG system task (runs off the boot
//! path; safe to call from boot without blocking it). Result is logged.
void dart_runtime_schedule_smoketest(void);

//! Run one stage of the smoke test (step = 1..5) so a UI can show "step N"
//! before executing it. Returns 1 if more steps remain, 0 when finished (and
//! sets *result_out, expected 42), -1 on failure. Single-user; diagnostic only.
int dart_smoketest_run_step(int step, int *result_out);

//! After dart_smoketest_run_step() returns -1, the WAMR error string for the
//! failing stage (e.g. "allocate memory failed"). Empty if no error.
const char *dart_smoketest_last_error(void);
