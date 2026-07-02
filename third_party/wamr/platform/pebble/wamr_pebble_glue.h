/* SPDX-FileCopyrightText: 2026 Jonah Walker */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

//! Engine-glue exports implemented in platform_pebble.c, which compiles with the
//! engine's own defines (WASM_ENABLE_GC etc.) and can therefore use internal WAMR
//! headers that firmware TUs must not include. Declared here so both sides share
//! one compiler-checked signature.

//! Re-bind an exec env's thread handle and native-stack-overflow boundary to the
//! CURRENT task. Required before entering a shared exec env from a task other
//! than the one that last entered it: the overflow guard compares SP against the
//! bound task's stack, so a stale binding either hard-faults or never trips.
//! @param exec_env a wasm_exec_env_t (void* to keep engine types out of here).
void wamr_pebble_bind_exec_env_to_current_task(void *exec_env);
