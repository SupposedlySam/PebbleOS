/* SPDX-FileCopyrightText: 2026 Jonah Walker */
/* SPDX-License-Identifier: Apache-2.0 */

#include "dart_runtime.h"
#include "dart_embedder.h"
#include "dart_test_module.h"

#include "console/dbgserial.h"
#include "kernel/pbl_malloc.h"
#include "system/logging.h"

#include "wasm_export.h"
#include "gc_export.h"

#include <inttypes.h>
#include <string.h>

/* The WasmGC objects live in the GC heap (init_args.gc_heap_size); the
   instantiate heap is the wasm linear memory + module malloc. These, plus the
   writable module copy, currently draw from the kernel heap (SRAM) and so do
   NOT fit SRAM-only boards (Emery/QEMU): a real module needs the PSRAM pool
   (Alloc_With_Pool over the ~8 MB PSRAM on obelix). Validated on QEMU: the
   runtime initializes and dispatches; allocation is the gate. */
#define DART_GC_HEAP_SIZE (512 * 1024)
#define DART_APP_STACK_SIZE (32 * 1024)
#define DART_APP_HEAP_SIZE (64 * 1024)
#define DART_EXEC_STACK_SIZE (32 * 1024)

static bool s_initialized = false;

bool dart_runtime_init(void) {
  if (s_initialized) {
    return true;
  }

  RuntimeInitArgs init_args;
  memset(&init_args, 0, sizeof(init_args));

  /* WAMR calls our os_malloc/os_free (libos -> kernel heap). Moving to a fixed
     PSRAM pool (Alloc_With_Pool) is a follow-up once PSRAM is brought up. */
  init_args.mem_alloc_type = Alloc_With_System_Allocator;
  init_args.gc_heap_size = DART_GC_HEAP_SIZE;
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

bool dart_run_module(const uint8_t *wasm_buf, uint32_t wasm_size) {
  char error_buf[128];
  uint8_t *module_buf = NULL;
  wasm_module_t module = NULL;
  wasm_module_inst_t inst = NULL;
  wasm_exec_env_t exec_env = NULL;
  bool ok = false;

  if (!dart_runtime_init()) {
    return false;
  }

  /* WAMR's loader writes the input buffer in place, so a flash-resident
     (const) module must be copied to RAM; the copy stays valid until unload.
     On SRAM-only boards this competes with the GC heap; a PSRAM pool removes
     the constraint. */
  module_buf = (uint8_t *)kernel_malloc(wasm_size);
  if (!module_buf) {
    PBL_LOG_ERR("dart: no RAM for module copy (%" PRIu32 " bytes)", wasm_size);
    return false;
  }
  memcpy(module_buf, wasm_buf, wasm_size);

  module = wasm_runtime_load(module_buf, wasm_size, error_buf, sizeof(error_buf));
  if (!module) {
    PBL_LOG_ERR("dart: load failed: %s", error_buf);
    goto cleanup;
  }

  inst = wasm_runtime_instantiate(module, DART_APP_STACK_SIZE,
                                  DART_APP_HEAP_SIZE, error_buf,
                                  sizeof(error_buf));
  if (!inst) {
    PBL_LOG_ERR("dart: instantiate failed: %s", error_buf);
    goto cleanup;
  }

  exec_env = wasm_runtime_create_exec_env(inst, DART_EXEC_STACK_SIZE);
  if (!exec_env) {
    PBL_LOG_ERR("dart: exec_env create failed");
    goto cleanup;
  }

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
  kernel_free(module_buf);
  return ok;
}

bool dart_run_test_module(void) {
  dbgserial_putstr("dart: running built-in hello module...");
  return dart_run_module(g_dart_test_module, g_dart_test_module_size);
}

//! Console command: `dart test` runs the built-in hello module.
void command_dart_test(void) {
  bool ok = dart_run_test_module();
  dbgserial_putstr(ok ? "dart: test OK" : "dart: test FAILED");
}
