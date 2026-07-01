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

//! Drain the Dart event loop (microtasks + timers queued via queueMicrotask/scheduleOnce) until
//! both queues empty or a guard trips. Call after the module's main() returns (to run runApp's
//! deferred warm-up frame) and after each injected input event. @param env/inst the running
//! module's exec env + instance.
void dart_embedder_run_event_loop(wasm_exec_env_t env, wasm_module_inst_t inst);

//! DIAG (throwaway): one-line description of the most recent event-loop callback
//! invocation -- "inv#N fidx=.. pcount=.. k0=.. argc=2 micro=.. timer=..". Lets
//! `dart status` surface the ACTUAL function the loop invoked so a refused argc is
//! observable over BLE. Empty until the first invoke. Remove with the diag in .c.
const char *dart_embedder_ev_diag(void);

//! DIAG (throwaway): record of each event-loop schedule's RECEIVED callback pointer
//! and storage slot ("M:cb=..@.. T:cb=..@.. "). Compared against dart_embedder_ev_diag's
//! invoke-time cb to tell marshalling vs corruption vs GC lifetime apart. Remove with the diag.
const char *dart_embedder_sched_diag(void);

//! DIAG (throwaway): last up-to-8 i64ToString (value hi:lo, radix) pairs -- what the counter
//! app formats via '$count'. radix should be 10 (proves i64+i32 native marshalling); a
//! 0xFFFFFFFF-range value then means the Dart int itself is garbage, not arg transit.
const char *dart_embedder_i64_dbg(void);

//! True once the running Flutter app has painted at least one frame into the app
//! framebuffer via presentFrame. The Counter app's root layer uses this to draw a
//! loading screen until the first frame, then leave the framebuffer to Flutter.
bool dart_embedder_frame_presented(void);

//! Number of frames presented since the last dart_embedder_reset_frame().
int dart_embedder_frame_count(void);

//! Clear the frame-presented flag (call when (re)starting an app so the loading screen
//! shows again until its first frame).
void dart_embedder_reset_frame(void);
