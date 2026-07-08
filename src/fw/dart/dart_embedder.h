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
uint32_t dart_embedder_diag_tasks_drained(void);
//! Push all permanent GC-root slots at the stable bottom of the exec env's
//! local-ref chain (call ONCE right after creating the env, before any module
//! code runs) / forget the fills when the env dies.
void dart_embedder_bind_roots(wasm_exec_env_t env);
void dart_embedder_reset_roots(void);

//! True once a WASM trap escaped a drained callback: the module's Dart-side
//! state is undefined and it must not be pumped again.
bool dart_embedder_module_trapped(void);
void dart_embedder_clear_trapped(void);
void dart_embedder_run_event_loop(wasm_exec_env_t env, wasm_module_inst_t inst);

//! True once the running Flutter app has painted at least one frame into the app
//! framebuffer via presentFrame. The Counter app's root layer uses this to draw a
//! loading screen until the first frame, then leave the framebuffer to Flutter.
bool dart_embedder_frame_presented(void);

//! Number of frames presented since the last dart_embedder_reset_frame().
int dart_embedder_frame_count(void);

//! Clear the frame-presented flag (call when (re)starting an app so the loading screen
//! shows again until its first frame).
void dart_embedder_reset_frame(void);

//! The last frame Flutter presented, as a GColor8 pixel buffer (stride == width), or
//! NULL before the first present. Persists across compositor repaints: presentFrame
//! blits into the FOREGROUND app's framebuffer (task-local, app_state_get_framebuffer),
//! so from the console path the watchface overwrites Flutter's pixels on its next
//! repaint -- this snapshot is the only stable record of what Flutter drew. Read by
//! the `ssapp` console command. @param[out] w_out/h_out snapshot dimensions.
const uint8_t *dart_embedder_frame_snapshot(int32_t *w_out, int32_t *h_out);
