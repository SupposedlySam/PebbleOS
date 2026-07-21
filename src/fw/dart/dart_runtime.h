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

//! Destroy the runtime + module cache + pool references so PSRAM can power off.
void dart_runtime_teardown(void);

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
//! Takes ownership of wasm_buf (a WAMR-pool RAM buffer): WAMR rewrites it in
//! place. The parsed module + buffer survive dart_app_stop as a warm cache
//! (relaunches skip load+validate); they are freed only on failure or when a
//! new buffer replaces them. wasm_buf == NULL relaunches FROM that cache and
//! fails ("no cached module") if none exists -- callers should check first
//! (see dart_app_start_flutter_counter).
//! @return true if main() ran and the first frame was pumped without trapping.
bool dart_app_start(uint8_t *wasm_buf, uint32_t wasm_size);

//! Deliver a tap at physical pixel (x, y) to the resident app (calls its
//! injectTap export), then drain the event loop so the resulting frame renders.
//! No-op if no app is running or it has no injectTap export.
//! @return true if the tap dispatched without trapping.
bool dart_app_inject_tap(double x, double y);

//! Deliver [count] taps as a BATCH: all pointer events dispatch first (each is
//! cheap -- the handler mutates state and the framework's scheduleFrame
//! coalesces), then ONE event-loop drain renders the final state. Flutter
//! semantics for burst input: N presses -> one frame showing the net result.
bool dart_app_dispatch_taps(int count, double x, double y);

//! Deliver ONE tap's pointer events (Listener handler -> setState ->
//! scheduleFrame) WITHOUT rendering -- cheap (no build/layout/paint), returns
//! in ~ms. Pair with dart_app_pump() on a coalescing timer: N rapid presses
//! each dispatch instantly (count keeps climbing) and one deferred pump renders
//! the latest value, exactly like Flutter (press 3 -> mash -> next frame 13).
bool dart_app_dispatch_only(double x, double y);

//! Render any frame the dispatches scheduled (drains the event loop once).
void dart_app_pump(void);

//! Deliver a hardware button as a KEY event to the resident app via the engine's
//! `injectKey(physical, logical)` export (USB-HID physical + Flutter logical key
//! ids): Flutter's Focus/Shortcuts/Actions activate the focused widget (Enter ->
//! its onPressed) or move focus (arrows), so no coordinate is needed and any
//! focusable widget works. WITHOUT rendering (the frame tick renders). No-op if
//! no app is running or it lacks the export. @return true if it dispatched
//! without trapping.
bool dart_app_inject_key(int64_t physical, int64_t logical);

//! Tear down the resident app (exec env, instance, module, RAM copy). Idempotent.
void dart_app_stop(void);

//! Which module a Flutter app loads. Lets the watchface and counter apps run
//! different modules from the ONE Dart runtime (they never run concurrently).
typedef enum {
  DART_SRC_AUTO = 0,   //!< PFS -> flash-XIP -> resource, by precedence (console default)
  DART_SRC_PFS,        //!< PFS only (dartpush'd flutter_app.wasm) -- the watchface
  DART_SRC_FLASH_XIP,  //!< DART_MODULE flash-XIP only (+resource fallback) -- the counter
} DartModuleSrc;

//! Start the resident Flutter app from the given module source (renders frame 0
//! via presentFrame). PSRAM must be up first. The warm cache is reused only when
//! it holds a module from the SAME source. @return true on success.
bool dart_app_start_flutter(DartModuleSrc src);

//! Load the FLUTTER_COUNTER_WASM resource + start the resident Flutter counter app
//! (renders frame 0 via the presentFrame native). PSRAM must be up first. @return
//! true on success. Used by the `dart flutter` console command and the Counter app.
bool dart_app_start_flutter_counter(void);

//! Parse + cache the Flutter module on KernelBG so the first app open skips
//! load+validate. Call after PSRAM is up; no-op if already cached.
void dart_runtime_schedule_precache(void);

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
