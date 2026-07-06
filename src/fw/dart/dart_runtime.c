/* SPDX-FileCopyrightText: 2026 Jonah Walker */
/* SPDX-License-Identifier: Apache-2.0 */

#include "dart_runtime.h"
#include "dart_embedder.h"
#include "dart_test_module.h"
#include "wasm_smoketest_module.h"
/* The ~1.18MB stripped Flutter counter is too big for the 3MB firmware FLASH
   partition (fw is ~2.07MB), so it is NOT embedded in .text -- it ships in the
   resource pack (resources/normal/obelix/resource_map.json FLUTTER_COUNTER_WASM,
   on external flash) and the runtime loads it via resource_load_byte_range_system
   into a RAM buffer (see prv_load_flutter_module + M3_PLAN). */
#include "resource/resource.h"
#include "resource/resource_ids.auto.h"

#include "apps/system/flutter_counter/flutter_counter.h"
#include "console/dbgserial.h"
#include "console/prompt.h"
#include "drivers/task_watchdog.h"
#include "kernel/event_loop.h"
#include "kernel/kernel_heap.h"
#include "kernel/pbl_malloc.h"
#include "os/mutex.h"
#include "wamr_pebble_glue.h"
#include "pbl/services/system_task.h"
#include "process_management/app_manager.h"
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
#define DART_GC_HEAP_SIZE_POOL (2 * 1024 * 1024)  /* 2MB from the 8MB PSRAM pool (WAMR default is 128KB) */
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

/* The Flutter framework recurses far deeper than the hello/smoke modules (text +
   paragraph layout): off-device, a 64KB operand stack OVERFLOWED and 1MB was
   ample (dart_host.c). These draw from the PSRAM pool on obelix, so 1MB is cheap.
   Separate from the small DART_APP_* above so the SRAM-friendly smoke tests are
   unaffected. Tune from on-device behaviour if needed. */
#define DART_FLUTTER_STACK_SIZE (64 * 1024)        /* instantiate default operand stack */
#define DART_FLUTTER_HEAP_SIZE (256 * 1024)        /* wasm linear-memory app heap */
#define DART_FLUTTER_EXEC_STACK_SIZE (1024 * 1024) /* exec_env operand stack (the deep one) */

static bool s_initialized = false;

/* Hardware-truth timings (ms), reported by dart status: BLE-side measurements
   stack console round-trip latency on top of the real work and overstate it
   badly (polling said 12s/tap when the render is far faster). */
static uint32_t s_init_ms;
static uint32_t s_last_tap_ms;
/* Millisecond tick from the WAMR platform layer (FreeRTOS tick underneath). */
extern uint64_t bh_get_tick_ms(void);

/* WASM memory source. A real dart2wasm module needs far more than the SRAM
   kernel heap can give (the Flutter module needs ~5.5 MB). On obelix the
   production source is a pool over PSRAM @0x60000000 (Alloc_With_Pool),
   returned below once `psram 2` has brought the controller up; the kernel-heap
   fallback covers boards without PSRAM (proven on QEMU/Emery: init + load
   succeed; instantiate is gated on RAM). Override dart_runtime_pool() (weak)
   to supply a different region. */
__attribute__((weak)) bool dart_runtime_pool(void **buf, uint32_t *size) {
#if defined(CONFIG_BOARD_FAMILY_OBELIX)
  // Bring up PSRAM (run `psram 2`) BEFORE the first dart command: WAMR init
  // caches its allocator, so a pool that appears later is never seen.
  if (sf32lb52_psram_is_ready()) {
    *buf = (void *)SF32LB52_PSRAM_BASE;
    // Fixed 8MB window rather than sf32lb52_psram_size(): that probe's
    // whole-cache CleanInvalidate round-trip reports 0KB even though normal
    // cached access is fine (long since proven by every module run).
    // 8MB of the 16MB PSRAM: the Flutter counter needs ~5.5MB (1.18MB module
    // copy + WAMR load structures + 1MB GC heap + 1MB operand stack + linear
    // memory). 2MB was too small ("allocate memory failed" during load).
    *size = 8u * 1024u * 1024u;
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
static char s_module_fail[224];

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

/* ---- Resident Flutter app ----
 * Unlike dart_run_module (run-to-completion), this keeps the instance + exec env
 * alive after main() so input can be injected and the UI re-rendered. main()'s
 * runApp queues a warm-up frame via a timer; we drain the event loop to render
 * the first frame, then again after each injected tap. Single resident app.
 *
 * CONCURRENCY: the one exec env is entered from BOTH KernelBG (console commands)
 * and the app task (button clicks). WAMR exec envs are single-threaded state
 * (operand stack, frame chain, local-ref frames), and the embedder's event
 * queues are plain globals -- so every entry point below takes s_app_mutex for
 * the WHOLE call. A tap arriving while another tap/start is in flight blocks
 * (seconds, on the interpreter) rather than corrupting the frame chain. */
static uint8_t *s_app_buf;
static wasm_module_t s_app_module;
static wasm_module_inst_t s_app_inst;
static wasm_exec_env_t s_app_exec_env;
static wasm_function_inst_t s_app_inject_tap;
static PebbleMutex *s_app_mutex;

static void prv_app_lock(void) {
  if (!s_app_mutex) {
    s_app_mutex = mutex_create(); /* first use is single-threaded (console/menu) */
  }
  mutex_lock(s_app_mutex);
}
static void prv_app_unlock(void) { mutex_unlock(s_app_mutex); }

//! Tear down the resident app INSTANCE. The parsed module + its buffer are
//! deliberately kept (a warm cache in PSRAM): load+validate of the 1.18MB
//! module is a large share of launch time, and reusing the parsed module lets
//! every launch after the first skip it. Caller must hold s_app_mutex.
static void prv_app_stop_locked(void) {
  // Drop the frame-presented flag so the next app launch shows its loading screen until
  // Flutter paints again. (present-frame renders through the normal app-framebuffer
  // path, so there is no compositor freeze to undo here.)
  dart_embedder_reset_frame();
  if (s_app_exec_env) { wasm_runtime_destroy_exec_env(s_app_exec_env); s_app_exec_env = NULL; }
  if (s_app_inst) { wasm_runtime_deinstantiate(s_app_inst); s_app_inst = NULL; }
  s_app_inject_tap = NULL;
}

//! True when a parsed module survives from a previous run (warm cache).
static bool prv_app_has_cached_module(void) {
  prv_app_lock();
  bool cached = s_app_module != NULL;
  prv_app_unlock();
  return cached;
}

//! Drop the cached parsed module too (full teardown; e.g. before loading a
//! DIFFERENT module). Caller must hold s_app_mutex.
static void prv_app_unload_module_locked(void) {
  if (s_app_module) { wasm_runtime_unload(s_app_module); s_app_module = NULL; }
  if (s_app_buf) { wasm_runtime_free(s_app_buf); s_app_buf = NULL; }
}

void dart_app_stop(void) {
  prv_app_lock();
  prv_app_stop_locked();
  prv_app_unlock();
}

bool dart_app_start(uint8_t *wasm_buf, uint32_t wasm_size) {
  char error_buf[128];
  prv_app_lock();
  s_module_fail[0] = '\0';
  if (s_app_inst) {
    prv_app_stop_locked(); /* one resident app at a time */
  }
  if (!dart_runtime_init()) {
    if (wasm_buf) wasm_runtime_free(wasm_buf);
    strncpy(s_module_fail, "runtime init", sizeof(s_module_fail) - 1);
    prv_app_unlock();
    return false;
  }
  /* The 1.18MB load + instantiate + runApp + first frame run synchronously on the
     interpreter for many seconds -- far longer than the 500ms task-watchdog window
     -- so this (KernelBG) task can't check in and the watchdog reboots us mid-init.
     Pause it for the duration (the PebbleOS pattern for known-long ops, e.g. flashing);
     the 240s cap still reboots a TRUE hang. Resumed at every exit below. */
  task_watchdog_pause(240);
  uint32_t t_start_ms = (uint32_t)bh_get_tick_ms();
  /* Phase timing (PBL_LOG lines carry timestamps; grep "dart phase"). */
  PBL_LOG_ALWAYS("dart phase: load start");
  if (s_app_module && wasm_buf == NULL) {
    /* Warm cache: the module survived the previous stop; skip load+validate. */
    PBL_LOG_ALWAYS("dart phase: load SKIPPED (cached module)");
  } else {
    prv_app_unload_module_locked(); /* replace any cached module */
    /* Take ownership of the caller's RAM buffer (loaded from storage). WAMR
       rewrites the load buffer in place; kept until the module is unloaded. */
    s_app_buf = wasm_buf;
    s_app_module = wasm_runtime_load(s_app_buf, wasm_size, error_buf, sizeof(error_buf));
    if (!s_app_module) {
      snprintf(s_module_fail, sizeof(s_module_fail), "load: %.70s", error_buf);
      goto fail;
    }
  }
  PBL_LOG_ALWAYS("dart phase: instantiate start");
  s_app_inst = wasm_runtime_instantiate(s_app_module, DART_FLUTTER_STACK_SIZE,
                                        DART_FLUTTER_HEAP_SIZE, error_buf, sizeof(error_buf));
  if (!s_app_inst) {
    snprintf(s_module_fail, sizeof(s_module_fail), "instantiate: %.70s", error_buf);
    goto fail;
  }
  s_app_exec_env = wasm_runtime_create_exec_env(s_app_inst, DART_FLUTTER_EXEC_STACK_SIZE);
  if (!s_app_exec_env) {
    strncpy(s_module_fail, "exec_env", sizeof(s_module_fail) - 1);
    goto fail;
  }
  PBL_LOG_ALWAYS("dart phase: main start");
  if (!prv_call_invoke_main(s_app_module, s_app_inst, s_app_exec_env)) {
    snprintf(s_module_fail, sizeof(s_module_fail), "main: %.70s",
             wasm_runtime_get_exception(s_app_inst));
    goto fail;
  }
  PBL_LOG_ALWAYS("dart phase: evloop start");
  /* Drain runApp's queued warm-up frame -> first render (presentFrame). */
  dart_embedder_run_event_loop(s_app_exec_env, s_app_inst);
  PBL_LOG_ALWAYS("dart phase: first frame done");
  s_init_ms = (uint32_t)bh_get_tick_ms() - t_start_ms;
  task_watchdog_resume(); /* heavy init done -- re-arm the watchdog */

  /* Diagnose why the first frame didn't render (most common: GC OOM in render()
     throws a WASM exception that exits the event loop before presentFrame). */
  const char *ev_exc = wasm_runtime_get_exception(s_app_inst);
  if (ev_exc && ev_exc[0]) {
    snprintf(s_module_fail, sizeof(s_module_fail), "evloop exc: %.150s", ev_exc);
    PBL_LOG_ALWAYS("dart: app event-loop exception: %s", ev_exc);
  } else if (!dart_embedder_frame_presented()) {
    strncpy(s_module_fail, "evloop ok, no frame (timer not fired?)",
            sizeof(s_module_fail) - 1);
    PBL_LOG_ALWAYS("dart: app event-loop ok but presentFrame never called");
  }

  s_app_inject_tap = wasm_runtime_lookup_function(s_app_inst, "injectTap");
  if (!s_app_inject_tap) {
    PBL_LOG_ALWAYS("dart: app has no injectTap export (input disabled)");
  }
  /* Note: s_module_fail can be non-empty here (evloop exception / no frame) while
     we still return true -- "started but degraded"; dart status surfaces it. */
  prv_app_unlock();
  return true;
fail:
  task_watchdog_resume(); /* re-arm before bailing (pause was active past the load) */
  prv_app_stop_locked();
  prv_app_unload_module_locked(); /* a half-initialized module is not a valid cache */
  prv_app_unlock();
  return false;
}

bool dart_app_inject_tap(double x, double y) {
  prv_app_lock();
  if (!s_app_exec_env || !s_app_inject_tap) {
    prv_app_unlock();
    return false;
  }
  /* The exec env's stack-overflow guard describes the LAST bound task's stack;
     re-bind to this task (KernelBG console vs app-task buttons) so deep
     re-renders trap instead of hard-faulting. Serialized by s_app_mutex. */
  wamr_pebble_bind_exec_env_to_current_task(s_app_exec_env);
  /* injectTap(f64 x, f64 y): two doubles occupy 4 arg cells. */
  uint32_t argv[4];
  memcpy(&argv[0], &x, sizeof(double));
  memcpy(&argv[2], &y, sizeof(double));
  /* tap -> setState -> scheduleFrame -> build/layout/paint is also heavy on the
     interpreter; pause the watchdog around it (see dart_app_start). */
  task_watchdog_pause(240);
  uint32_t t_tap_ms = (uint32_t)bh_get_tick_ms();
  if (!wasm_runtime_call_wasm(s_app_exec_env, s_app_inject_tap, 4, argv)) {
    task_watchdog_resume();
    PBL_LOG_ERR("dart: injectTap trap: %s", wasm_runtime_get_exception(s_app_inst));
    prv_app_unlock();
    return false;
  }
  /* tap -> setState -> scheduleFrame queued a frame; drain it -> re-render. */
  dart_embedder_run_event_loop(s_app_exec_env, s_app_inst);
  s_last_tap_ms = (uint32_t)bh_get_tick_ms() - t_tap_ms;
  task_watchdog_resume();
  prv_app_unlock();
  return true;
}

bool dart_app_is_running(void) {
  return s_app_inst != NULL;
}

const char *dart_app_last_fail(void) {
  return s_module_fail;
}

//! Minimal substring check (avoids pulling in <string.h> here).
static bool prv_str_contains(const char *hay, const char *needle) {
  if (!hay || !needle) {
    return false;
  }
  for (; *hay; hay++) {
    const char *a = hay, *b = needle;
    while (*a && *b && *a == *b) {
      a++;
      b++;
    }
    if (!*b) {
      return true;
    }
  }
  return false;
}

//! Console command: `dart test` runs the built-in hello module (which computes 0+1+..+9 and prints
//! "sum=45"). Reports the free kernel heap, the failing stage, AND asserts the literal computed
//! result -- "OK" alone only means no WASM trap, which a silently-corrupted PSRAM heap could still
//! produce with a wrong value.
void command_dart_test(void) {
  char buf[160];
  unsigned int used, free_bytes, max_free;
  heap_calc_totals(kernel_heap_get(), &used, &free_bytes, &max_free);
  prompt_send_response_fmt(buf, sizeof(buf),
                           "kernel heap: free=%u max_block=%u (module=%u)",
                           free_bytes, max_free, (unsigned)g_dart_test_module_size);
  dart_embedder_clear_last_print();
  bool ok = dart_run_test_module();
  prompt_send_response_fmt(buf, sizeof(buf), "dart: test %s%s%s", ok ? "OK" : "FAILED",
                           s_module_fail[0] ? " - " : "", s_module_fail);
  const char *out = dart_embedder_last_print();
  bool sum_ok = ok && prv_str_contains(out, "sum=45");
  prompt_send_response_fmt(buf, sizeof(buf), "dart: sum=45 %s [print: %s]",
                           sum_ok ? "VERIFIED" : "NOT VERIFIED", out[0] ? out : "(none)");
}

//! Load the Flutter counter module (the FLUTTER_COUNTER_WASM resource, shipped in
//! the resource pack) into a RAM buffer the caller owns. Uses the WAMR pool
//! allocator (PSRAM) since the module is ~1.18MB; needs the runtime initialized
//! first. @return the buffer (free with wasm_runtime_free) or NULL on failure.
static uint8_t *prv_load_flutter_module(uint32_t *size_out) {
  *size_out = 0;
  if (!dart_runtime_init()) {
    strncpy(s_module_fail, "runtime init", sizeof(s_module_fail) - 1);
    return NULL;
  }
  size_t sz = resource_size(SYSTEM_APP, RESOURCE_ID_FLUTTER_COUNTER_WASM);
  if (sz == 0) {
    strncpy(s_module_fail, "counter resource missing", sizeof(s_module_fail) - 1);
    return NULL;
  }
  uint8_t *buf = (uint8_t *)wasm_runtime_malloc(sz);
  if (!buf) {
    /* The classic cause: the runtime initialized before `psram 2`, so the
       cached allocator is the small SRAM heap and a 1.18MB module can't fit.
       Recovery: `reset`, then `psram 2` BEFORE any dart command. */
    snprintf(s_module_fail, sizeof(s_module_fail),
             "no RAM for %u-byte module (psram before first dart?)", (unsigned)sz);
    return NULL;
  }
  size_t rd = resource_load_byte_range_system(SYSTEM_APP, RESOURCE_ID_FLUTTER_COUNTER_WASM,
                                              0, buf, sz);
  if (rd != sz) {
    snprintf(s_module_fail, sizeof(s_module_fail), "resource short read %u/%u",
             (unsigned)rd, (unsigned)sz);
    wasm_runtime_free(buf);
    return NULL;
  }
  *size_out = (uint32_t)sz;
  return buf;
}

//! Load the FLUTTER_COUNTER_WASM resource and start the resident Flutter app,
//! rendering frame 0. PSRAM must already be up (sf32lb52_psram_bringup) so
//! dart_runtime_init takes the PSRAM pool. Shared by the console command and the
//! Counter system app. @return true if it started + pumped the first frame.
bool dart_app_start_flutter_counter(void) {
  if (prv_app_has_cached_module()) {
    /* Warm cache: reuse the parsed module; skips the resource read AND WAMR
       load/validate of the 1.18MB module. */
    return dart_app_start(NULL, 0);
  }
  uint32_t size = 0;
  uint8_t *mod = prv_load_flutter_module(&size);
  if (!mod) {
    return false;
  }
  return dart_app_start(mod, size); /* takes ownership of mod */
}

//! `dart flutter`: console entry to start the Flutter counter (renders frame 0).
//! The instance stays resident so `dart tap` (or the Counter app's buttons) drive it.
void command_dart_flutter(void) {
  char buf[160];
  bool ok = dart_app_start_flutter_counter();
  prompt_send_response_fmt(buf, sizeof(buf), "dart: flutter %s%s%s",
                           ok ? "OK (frame 0)" : "FAILED",
                           s_module_fail[0] ? " - " : "", s_module_fail);
}

//! `dart tap`: inject a tap at the screen center into the resident app -> the
//! counter increments and re-renders. Mirrors a physical button/touch press.
void command_dart_tap(void) {
  char buf[160];
  bool ok = dart_app_inject_tap(100.0, 114.0);
  prompt_send_response_fmt(buf, sizeof(buf), "dart: tap %s",
                           ok ? "OK (re-rendered)" : "FAILED (no app running?)");
}

//! DIAG (throwaway, INV2): `dart gc` -- force a collection with live phase
//! markers over the BLE console, to pin WHERE the first-major-GC crash dies
//! (the count-8 watch reboot). Runs on KernelBG (console), same task the
//! crash reproduces on via dart tap.
static void prv_gc_trace(const char *phase) {
  char buf[64];
  prompt_send_response_fmt(buf, sizeof(buf), "gc: %s", phase);
}
void command_dart_gc(void) {
  prv_app_lock();
  if (!s_app_inst) {
    prompt_send_response("gc: no app running");
    prv_app_unlock();
    return;
  }
  wamr_pebble_set_gc_trace_cb(prv_gc_trace);
  bool ok = wamr_pebble_force_gc(s_app_inst);
  wamr_pebble_set_gc_trace_cb(NULL);
  prompt_send_response(ok ? "gc: done OK" : "gc: FAILED");
  prv_app_unlock();
}

//! `dart status`: report resident-app state + last-failure reason over the BLE console.
//! Safe to call at any time; reads only global flags (no WAMR calls).
void command_dart_status(void) {
  char buf[256];
  prompt_send_response_fmt(buf, sizeof(buf),
      "dart: running=%s frames=%d init_ms=%u tap_ms=%u fail=%s",
      dart_app_is_running() ? "yes" : "no",
      dart_embedder_frame_count(),
      (unsigned)s_init_ms, (unsigned)s_last_tap_ms,
      s_module_fail[0] ? s_module_fail : "(none)");
}

//! Callback that runs on KernelMain (the launcher task) to start the Counter app.
static void prv_launch_counter_cb(void *unused) {
  app_manager_launch_new_app(&(AppLaunchConfig) {
    .md = flutter_counter_get_app_info(),
    .restart = true,
  });
}

//! `dart counter`: launch the Counter system app via the normal app-manager path
//! (app task, 32KB stack). Equivalent to selecting it from the menu.
void command_dart_counter(void) {
  launcher_task_add_callback(prv_launch_counter_cb, NULL);
  prompt_send_response("dart: launching Counter app");
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
