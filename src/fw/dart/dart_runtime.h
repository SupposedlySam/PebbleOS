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
//! Takes ownership of wasm_buf (a WAMR-pool RAM buffer, e.g. from PFS): WAMR
//! rewrites it in place and dart_app_stop frees it. Freed too on failure.
//! @return true if main() ran and the first frame was pumped without trapping.
bool dart_app_start(uint8_t *wasm_buf, uint32_t wasm_size);

//! Deliver a tap at physical pixel (x, y) to the resident app (calls its
//! injectTap export), then drain the event loop so the resulting frame renders.
//! No-op if no app is running or it has no injectTap export.
//! @return true if the tap dispatched without trapping.
bool dart_app_inject_tap(double x, double y);

//! Tear down the resident app (exec env, instance, module, RAM copy). Idempotent.
void dart_app_stop(void);

//! Load the FLUTTER_COUNTER_WASM resource + start the resident Flutter counter app
//! (renders frame 0 via the presentFrame native). PSRAM must be up first. @return
//! true on success. Used by the `dart flutter` console command and the Counter app.
bool dart_app_start_flutter_counter(void);

//! DIAG (throwaway, INV2): register a callback invoked with a short stage name ("load",
//! "instantiate", "exec_env", "invokeMain", "evloop", "evloop-done") before each phase of
//! dart_app_start, so the Counter app can APP_LOG real-time markers over BLE to localize
//! where it crashes on the app task (the reboot clears RAM). NULL clears. Remove once fixed.
void dart_set_stage_cb(void (*cb)(const char *stage));

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

//! True if a resident Flutter app is loaded and running (instance alive).
bool dart_app_is_running(void);

//! Human-readable diagnostic for the last dart_app_start() call (empty if OK).
//! Set when the event loop exits with an exception or no frame is presented.
const char *dart_app_last_fail(void);
