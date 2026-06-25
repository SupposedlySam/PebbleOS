/* SPDX-FileCopyrightText: 2026 Jonah Walker */
/* SPDX-License-Identifier: Apache-2.0 */

#include "dart_runtime.h"
#include "dart_embedder.h"
#include "dart_test_module.h"
#include "wasm_smoketest_module.h"

#include "console/dbgserial.h"
#include "console/prompt.h"
#include "kernel/kernel_heap.h"
#include "kernel/pbl_malloc.h"
#include "pbl/services/system_task.h"
#include "system/logging.h"
#include "util/heap.h"

#include "wasm_export.h"
#include "gc_export.h"

#if defined(CONFIG_BOARD_FAMILY_OBELIX)
#include "soc/sf32lb/sf32lb52x/psram.h"
#endif

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* The WasmGC objects live in the GC heap (init_args.gc_heap_size); the
   instantiate heap is the wasm linear memory + module malloc. These, plus the
   writable module copy, currently draw from the kernel heap (SRAM) and so do
   NOT fit SRAM-only boards (Emery/QEMU): a real module needs the PSRAM pool
   (Alloc_With_Pool over the ~8 MB PSRAM on obelix). Validated on QEMU: the
   runtime initializes and dispatches; allocation is the gate. */
#ifndef DART_GC_HEAP_SIZE
/* Allocated per-instance at instantiate even for non-GC modules. On real obelix
   the SRAM kernel heap is tighter than the emulator, and 32 KB here made
   instantiate fail on hardware (it succeeded in QEMU). The no-GC smoke test
   needs essentially no GC heap, so keep this small; the full Dart module will
   need a much larger GC heap from a PSRAM pool. */
#define DART_GC_HEAP_SIZE (8 * 1024)
#endif
/* When the WAMR pool is backed by PSRAM (obelix), the GC heap draws from that 8MB pool,
   not SRAM -- so the full dart2wasm module gets a generous GC heap for its constant object
   graph + instance state (8KB above is only enough for the no-GC smoke test). */
#ifndef DART_GC_HEAP_SIZE_POOL
#define DART_GC_HEAP_SIZE_POOL (1 * 1024 * 1024)  /* 1MB from the PSRAM pool (WAMR default is 128KB) */
#endif
/* Minimum probed-usable PSRAM to bother routing the pool through it: the full module needs
   the 67KB writable copy + GC heap + linear memory (~194KB). Below this, fall back to the
   SRAM system allocator (which only fits the no-GC smoke test). */
#ifndef DART_POOL_MIN_SIZE
#define DART_POOL_MIN_SIZE (256 * 1024)
#endif
#define DART_APP_STACK_SIZE (12 * 1024)
#define DART_APP_HEAP_SIZE (12 * 1024)
#define DART_EXEC_STACK_SIZE (12 * 1024)

static bool s_initialized = false;

/* WASM memory source. A real dart2wasm module needs far more than the SRAM
   kernel heap can give (the hello module alone needs >194 KB: ~67 KB writable
   copy + the GC heap for its constant object graph + instance state). On
   obelix the production source is a pool over PSRAM @0x60000000 (Alloc_With_Pool)
   — pending PSRAM bring-up. Until then we use the system allocator (kernel
   heap) and fail gracefully when a module doesn't fit (proven on QEMU/Emery:
   init + load succeed; instantiate is gated on RAM). A dedicated pool can be
   provided by overriding dart_runtime_pool() (weak) to return a PSRAM region. */
__attribute__((weak)) bool dart_runtime_pool(void **buf, uint32_t *size) {
#if defined(CONFIG_BOARD_FAMILY_OBELIX)
  // Once `psram` has brought up the 16 MB PSRAM, back the WAMR pool with it so
  // the full module's 67 KB copy + GC heap + linear memory fit. Bring up PSRAM
  // (run `psram`) BEFORE the first dart command so this is seen at WAMR init.
  if (sf32lb52_psram_is_ready()) {
    *buf = (void *)SF32LB52_PSRAM_BASE;
    // TEMP (bypassing sf32lb52_psram_size): the USABLE probe reports 0KB even with the PSRAM
    // window now cacheable, but that probe forces a whole-cache CleanInvalidate round-trip which
    // may not reflect NORMAL coherent cached access (how WAMR/dart actually use the pool: writes
    // and reads both via the D-cache, with natural line eviction/refill to PSRAM). Hand dart a
    // fixed cached pool and let real use be the test. If sum=45 works, the cached path is good
    // and the probe just needs fixing; if it corrupts/crashes, cached bulk access is genuinely
    // broken and we move to write-through / tap / DMA.
    *size = 2u * 1024u * 1024u;
    return true;
  }
#endif
  (void)buf;
  (void)size;
  return false;
}

bool dart_runtime_init(void) {
  if (s_initialized) {
    return true;
  }

  RuntimeInitArgs init_args;
  memset(&init_args, 0, sizeof(init_args));

  void *pool_buf = NULL;
  uint32_t pool_size = 0;
  if (dart_runtime_pool(&pool_buf, &pool_size) && pool_size >= DART_POOL_MIN_SIZE) {
    init_args.mem_alloc_type = Alloc_With_Pool;
    init_args.mem_alloc_option.pool.heap_buf = pool_buf;
    init_args.mem_alloc_option.pool.heap_size = pool_size;
    // The GC heap is carved from the pool; cap it to a quarter of the (possibly small)
    // usable region so it always fits alongside the 67KB module copy + linear memory.
    uint32_t gc_heap = DART_GC_HEAP_SIZE_POOL;
    if (gc_heap > pool_size / 4u) {
      gc_heap = pool_size / 4u;
    }
    init_args.gc_heap_size = gc_heap;
  } else {
    init_args.mem_alloc_type = Alloc_With_System_Allocator;
    init_args.gc_heap_size = DART_GC_HEAP_SIZE;        // small SRAM heap (smoke test / QEMU)
  }
  init_args.native_module_name = "dart";
  init_args.native_symbols =
      dart_embedder_get_natives(&init_args.n_native_symbols);

  if (!wasm_runtime_full_init(&init_args)) {
    PBL_LOG_ERR("dart: WAMR runtime init failed");
    return false;
  }

  s_initialized = true;
  PBL_LOG_DBG("dart: WAMR runtime initialized");
  return true;
}

//! Build the empty (ref $Array<externref>) that $invokeMain expects (Dart's
//! List<String> args) and call it. Returns true if it ran without trapping.
static bool prv_call_invoke_main(wasm_module_t module, wasm_module_inst_t inst,
                                 wasm_exec_env_t exec_env) {
  wasm_function_inst_t main_func =
      wasm_runtime_lookup_function(inst, "$invokeMain");
  if (!main_func) {
    PBL_LOG_ERR("dart: no $invokeMain export");
    return false;
  }

  /* Find the module's array-of-externref type (the args list element type). */
  uint32_t arr_type_idx = (uint32_t)-1;
  uint32_t ndt = wasm_get_defined_type_count(module);
  for (uint32_t ti = 0; ti < ndt; ti++) {
    wasm_defined_type_t dt = wasm_get_defined_type(module, ti);
    if (wasm_defined_type_is_array_type(dt)) {
      bool mut = false;
      wasm_ref_type_t et =
          wasm_array_type_get_elem_type((wasm_array_type_t)dt, &mut);
      if (et.value_type == VALUE_TYPE_EXTERNREF) {
        arr_type_idx = ti;
        break;
      }
    }
  }
  if (arr_type_idx == (uint32_t)-1) {
    PBL_LOG_ERR("dart: no array<externref> args type");
    return false;
  }

  wasm_array_obj_t args_arr =
      wasm_array_obj_new_with_typeidx(exec_env, arr_type_idx, 0, NULL);
  if (!args_arr) {
    PBL_LOG_ERR("dart: failed to build args array");
    return false;
  }

  /* A reference argument occupies sizeof(uintptr_t) (2 cells on the call ABI). */
  uint32_t argv[4] = {0};
  uintptr_t arg_ptr = (uintptr_t)args_arr;
  memcpy(argv, &arg_ptr, sizeof(arg_ptr));
  if (!wasm_runtime_call_wasm(exec_env, main_func, 2, argv)) {
    PBL_LOG_ERR("dart: trap: %s", wasm_runtime_get_exception(inst));
    return false;
  }
  return true;
}

//! Human-readable reason the last dart_run_module() failed (for the console).
static char s_module_fail[96];

bool dart_run_module(const uint8_t *wasm_buf, uint32_t wasm_size) {
  char error_buf[128];
  uint8_t *module_buf = NULL;
  wasm_module_t module = NULL;
  wasm_module_inst_t inst = NULL;
  wasm_exec_env_t exec_env = NULL;
  bool ok = false;
  s_module_fail[0] = '\0';

  if (!dart_runtime_init()) {
    strncpy(s_module_fail, "runtime init", sizeof(s_module_fail) - 1);
    return false;
  }

  /* WAMR's loader writes the input buffer in place, so a flash-resident
     (const) module must be copied to RAM; the copy stays valid until unload.
     On SRAM-only boards this competes with the GC heap; a PSRAM pool removes
     the constraint. */
  // Step markers (shipped over the console) to pin where a big module stalls on-device.
  prompt_send_response("dart: [1] rt init OK; copying module");
  module_buf = (uint8_t *)wasm_runtime_malloc(wasm_size);
  if (!module_buf) {
    PBL_LOG_ERR("dart: no RAM for module copy (%" PRIu32 " bytes)", wasm_size);
    snprintf(s_module_fail, sizeof(s_module_fail), "no RAM for %u-byte copy",
             (unsigned)wasm_size);
    return false;
  }
  memcpy(module_buf, wasm_buf, wasm_size);

  prompt_send_response("dart: [2] copied; loading (WAMR parse/validate)");
  module = wasm_runtime_load(module_buf, wasm_size, error_buf, sizeof(error_buf));
  if (!module) {
    PBL_LOG_ERR("dart: load failed: %s", error_buf);
    snprintf(s_module_fail, sizeof(s_module_fail), "load: %.70s", error_buf);
    goto cleanup;
  }

  prompt_send_response("dart: [3] loaded; instantiating (GC heap)");
  inst = wasm_runtime_instantiate(module, DART_APP_STACK_SIZE,
                                  DART_APP_HEAP_SIZE, error_buf,
                                  sizeof(error_buf));
  if (!inst) {
    PBL_LOG_ERR("dart: instantiate failed: %s", error_buf);
    snprintf(s_module_fail, sizeof(s_module_fail), "instantiate: %.70s", error_buf);
    goto cleanup;
  }

  prompt_send_response("dart: [4] instantiated; creating exec env");
  exec_env = wasm_runtime_create_exec_env(inst, DART_EXEC_STACK_SIZE);
  if (!exec_env) {
    PBL_LOG_ERR("dart: exec_env create failed");
    goto cleanup;
  }

  prompt_send_response("dart: [5] invoking main");
  ok = prv_call_invoke_main(module, inst, exec_env);
  if (ok) {
    PBL_LOG_DBG("dart: module ran OK");
  }

cleanup:
  if (exec_env) {
    wasm_runtime_destroy_exec_env(exec_env);
  }
  if (inst) {
    wasm_runtime_deinstantiate(inst);
  }
  if (module) {
    wasm_runtime_unload(module);
  }
  if (module_buf) {
    wasm_runtime_free(module_buf);
  }
  return ok;
}

bool dart_run_test_module(void) {
  dbgserial_putstr("dart: running built-in hello module...");
  return dart_run_module(g_dart_test_module, g_dart_test_module_size);
}

//! Console command: `dart test` runs the built-in hello module. Reports the
//! free kernel heap (the SRAM gap for the full module) and the failing stage.
void command_dart_test(void) {
  char buf[128];
  unsigned int used, free_bytes, max_free;
  heap_calc_totals(kernel_heap_get(), &used, &free_bytes, &max_free);
  prompt_send_response_fmt(buf, sizeof(buf),
                           "kernel heap: free=%u max_block=%u (module=%u)",
                           free_bytes, max_free, (unsigned)g_dart_test_module_size);
  bool ok = dart_run_test_module();
  prompt_send_response_fmt(buf, sizeof(buf), "dart: test %s%s%s", ok ? "OK" : "FAILED",
                           s_module_fail[0] ? " - " : "", s_module_fail);
}

//! Execute a tiny no-GC wasm module (add(40,2)) to verify WAMR runs wasm in the
//! firmware. Small enough for SRAM, so it works without PSRAM. Returns true if
//! the result is 42.
bool dart_run_wasm_smoketest(void) {
  char error_buf[128];
  uint8_t *buf = NULL;
  wasm_module_t module = NULL;
  wasm_module_inst_t inst = NULL;
  wasm_exec_env_t exec_env = NULL;
  bool ok = false;

  // Step markers (PBL_LOG_ALWAYS so they hit the flash log): if the test faults
  // on hardware, the last marker in `pebble fw flash-logs` names the dead stage.
  PBL_LOG_ALWAYS("dart: smoketest [1] init");
  if (!dart_runtime_init()) {
    return false;
  }

  PBL_LOG_ALWAYS("dart: smoketest [2] malloc %u", (unsigned)g_wasm_add_module_size);
  buf = (uint8_t *)wasm_runtime_malloc(g_wasm_add_module_size);
  if (!buf) {
    PBL_LOG_ERR("dart: smoketest no RAM");
    return false;
  }
  memcpy(buf, g_wasm_add_module, g_wasm_add_module_size);

  PBL_LOG_ALWAYS("dart: smoketest [3] load");
  module = wasm_runtime_load(buf, g_wasm_add_module_size, error_buf,
                             sizeof(error_buf));
  if (!module) {
    PBL_LOG_ERR("dart: smoketest load failed: %s", error_buf);
    goto done;
  }
  PBL_LOG_ALWAYS("dart: smoketest [4] instantiate");
  inst = wasm_runtime_instantiate(module, 8 * 1024, 8 * 1024, error_buf,
                                  sizeof(error_buf));
  if (!inst) {
    PBL_LOG_ERR("dart: smoketest instantiate failed: %s", error_buf);
    goto done;
  }
  PBL_LOG_ALWAYS("dart: smoketest [5] exec_env");
  exec_env = wasm_runtime_create_exec_env(inst, 8 * 1024);
  wasm_function_inst_t add_func = wasm_runtime_lookup_function(inst, "add");
  if (!add_func) {
    PBL_LOG_ERR("dart: smoketest no 'add' export");
    goto done;
  }

  PBL_LOG_ALWAYS("dart: smoketest [6] call add(40,2)");
  uint32_t argv[2] = {40, 2};
  if (!wasm_runtime_call_wasm(exec_env, add_func, 2, argv)) {
    PBL_LOG_ERR("dart: smoketest trap: %s", wasm_runtime_get_exception(inst));
    goto done;
  }

  PBL_LOG_ALWAYS("dart: wasm smoketest add(40,2)=%u %s", (unsigned)argv[0],
               argv[0] == 42 ? "OK" : "WRONG");
  ok = (argv[0] == 42);

done:
  if (exec_env) {
    wasm_runtime_destroy_exec_env(exec_env);
  }
  if (inst) {
    wasm_runtime_deinstantiate(inst);
  }
  if (module) {
    wasm_runtime_unload(module);
  }
  if (buf) {
    wasm_runtime_free(buf);
  }
  return ok;
}

//! Console command: `dart wasm` runs the tiny WAMR smoke test.
void command_dart_wasm(void) {
  bool ok = dart_run_wasm_smoketest();
  char buf[64];
  prompt_send_response_fmt(buf, sizeof(buf), "dart: wasm smoketest %s",
                           ok ? "OK (add(40,2)=42)" : "FAILED");
}

/* Stepped smoke test for the on-screen diagnostic app. Each WAMR stage runs in
 * a separate call so the app can render "step N" to the screen BEFORE executing
 * it: if a stage faults on hardware, the screen freezes on the dead stage. Holds
 * state in file statics (single-user, diagnostic only). */
static uint8_t *s_step_buf;
static wasm_module_t s_step_module;
static wasm_module_inst_t s_step_inst;
static wasm_exec_env_t s_step_exec_env;
static wasm_function_inst_t s_step_add_func;
static char s_step_error[160];

const char *dart_smoketest_last_error(void) { return s_step_error; }

static void prv_step_cleanup(void) {
  if (s_step_exec_env) { wasm_runtime_destroy_exec_env(s_step_exec_env); s_step_exec_env = NULL; }
  if (s_step_inst) { wasm_runtime_deinstantiate(s_step_inst); s_step_inst = NULL; }
  if (s_step_module) { wasm_runtime_unload(s_step_module); s_step_module = NULL; }
  if (s_step_buf) { wasm_runtime_free(s_step_buf); s_step_buf = NULL; }
  s_step_add_func = NULL;
}

int dart_smoketest_run_step(int step, int *result_out) {
  char err[128];
  s_step_error[0] = '\0';
  switch (step) {
    case 1:
      if (!dart_runtime_init()) {
        snprintf(s_step_error, sizeof(s_step_error), "runtime init failed");
        return -1;
      }
      return 1;
    case 2:
      s_step_buf = (uint8_t *)wasm_runtime_malloc(g_wasm_add_module_size);
      if (!s_step_buf) {
        snprintf(s_step_error, sizeof(s_step_error), "malloc %u failed",
                 (unsigned)g_wasm_add_module_size);
        return -1;
      }
      memcpy(s_step_buf, g_wasm_add_module, g_wasm_add_module_size);
      s_step_module = wasm_runtime_load(s_step_buf, g_wasm_add_module_size, err, sizeof(err));
      if (!s_step_module) {
        snprintf(s_step_error, sizeof(s_step_error), "%s", err);
        prv_step_cleanup();
        return -1;
      }
      return 1;
    case 3:
      s_step_inst = wasm_runtime_instantiate(s_step_module, 8 * 1024, 8 * 1024, err, sizeof(err));
      if (!s_step_inst) {
        snprintf(s_step_error, sizeof(s_step_error), "%s", err);
        prv_step_cleanup();
        return -1;
      }
      return 1;
    case 4:
      s_step_exec_env = wasm_runtime_create_exec_env(s_step_inst, 8 * 1024);
      s_step_add_func = wasm_runtime_lookup_function(s_step_inst, "add");
      if (!s_step_exec_env || !s_step_add_func) {
        snprintf(s_step_error, sizeof(s_step_error), "%s",
                 s_step_exec_env ? "no 'add' export" : "exec_env alloc failed");
        prv_step_cleanup();
        return -1;
      }
      return 1;
    case 5: {
      uint32_t argv[2] = {40, 2};
      bool ok = wasm_runtime_call_wasm(s_step_exec_env, s_step_add_func, 2, argv);
      if (!ok) {
        snprintf(s_step_error, sizeof(s_step_error), "trap: %s",
                 wasm_runtime_get_exception(s_step_inst));
        prv_step_cleanup();
        return -1;
      }
      if (result_out) { *result_out = (int)argv[0]; }
      prv_step_cleanup();
      return 0;  // done
    }
    default:
      return -1;
  }
}

static void prv_dart_smoketest_cb(void *data) {
  (void)data;
  dart_run_wasm_smoketest();
}

void dart_runtime_schedule_smoketest(void) {
  // Queue on the KernelBG system task. This runs OFF the boot path, so it can
  // never block KernelMain / trip the watchdog (the bug that bricked a watch).
  // The smoke test is tiny (no GC, no PSRAM) so it completes well within the
  // system task's budget. Result is logged (retrievable via `pebble fw
  // flash-logs`).
  system_task_add_callback(prv_dart_smoketest_cb, NULL);
}
