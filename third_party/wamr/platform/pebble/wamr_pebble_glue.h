/* SPDX-FileCopyrightText: 2026 Jonah Walker */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

//! Engine-glue exports implemented in platform_pebble.c, which compiles with the
//! engine's own defines (WASM_ENABLE_GC etc.) and can therefore use internal WAMR
//! headers that firmware TUs must not include. Declared here so both sides share
//! one compiler-checked signature.

#include <stdint.h>

//! Re-bind an exec env's thread handle and native-stack-overflow boundary to the
//! CURRENT task. Required before entering a shared exec env from a task other
//! than the one that last entered it: the overflow guard compares SP against the
//! bound task's stack, so a stale binding either hard-faults or never trips.
//! @param exec_env a wasm_exec_env_t (void* to keep engine types out of here).
void wamr_pebble_bind_exec_env_to_current_task(void *exec_env);

//! Raw element storage of a wasm GC array whose elements are 1 byte wide, or
//! NULL for any other element size. Lets bulk consumers (the presentFrame
//! pixel blit) iterate the data directly instead of paying the per-element
//! accessor's rtt lookup tens of thousands of times per frame. The pointer
//! aliases live GC-heap storage: use it only while the array is rooted (e.g.
//! within the native call that received it) and do not hold it across any
//! call that can run wasm or GC. @param array_obj a wasm_array_obj_t.
const uint8_t *wamr_pebble_array_u8_data(void *array_obj);

//! Raw element storage of a WasmGC array of ANY element type (f64/i32/...),
//! for callers that reinterpret it (e.g. WasmArray<WasmF64> as double*). Same
//! rooting/lifetime rules as wamr_pebble_array_u8_data. @param array_obj a
//! wasm_array_obj_t. Returns NULL only if array_obj is NULL.
const void *wamr_pebble_array_raw_data(void *array_obj);

//! DIAG (throwaway, INV2): GC crash hunt. Register a callback that receives a
//! short phase name at each GC phase boundary (rootset/mark/sweep), and force
//! an immediate collection of a module instance's GC heap. Remove when the
//! count-8 crash is closed.
void wamr_pebble_set_gc_trace_cb(void (*cb)(const char *phase));
bool wamr_pebble_force_gc(void *module_inst);
//! Strong override of the engine's weak hook (ems_gc.c); shared prototype so
//! the two definitions cannot drift silently.
void wamr_pebble_gc_trace(const char *phase);

