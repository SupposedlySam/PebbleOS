/* SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception */
/* WAMR platform layer for PebbleOS - implementation.
   Binds WAMR's os_ and bh_ platform surface to PebbleOS services: the kernel
   heap, PebbleOS mutexes, FreeRTOS binary semaphores, the FreeRTOS tick, and
   the debug serial. WAMR is initialized with a fixed pool (Alloc_With_Pool,
   see src/fw/dart), so os_malloc is rarely hit; the wasm heap is managed by
   WAMR's own GC allocator inside that pool. */

#include "bh_platform.h"

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include "os/mutex.h"
#include "os/tick.h"
#include "kernel/pbl_malloc.h"
#include "console/dbgserial.h"

#include "wasm_runtime.h"

/* Re-bind an exec env's thread handle + native-stack boundary to the CURRENT
   task. The dart runtime's single exec env is entered from both KernelBG (the
   console path) and the app task (button clicks); the stack-overflow guard set
   at creation time describes the creating task's stack, so every cross-task
   entry must re-bind or the guard compares against the wrong stack. Lives here
   because wasm_exec_env_set_thread_info is an internal header and this TU is
   compiled with the engine's defines (firmware TUs are not). */
#include "wasm_exec_env.h"
void
dart_wamr_bind_exec_env_to_current_task(void *exec_env)
{
    wasm_exec_env_set_thread_info((WASMExecEnv *)exec_env);
}

/* ---- lifecycle ---- */

int
bh_platform_init(void)
{
    return 0;
}

void
bh_platform_destroy(void)
{
}

/* ---- memory ----
   os_malloc/os_free are provided (WEAK) by src/libos; we only add realloc and
   the mmap shims. No MMU: mappings are plain heap allocations, protect is a
   no-op. */

void *
os_realloc(void *ptr, unsigned size)
{
    return kernel_realloc(ptr, size);
}

unsigned
os_getpagesize(void)
{
    return 4096;
}

void *
os_mmap(void *hint, size_t size, int prot, int flags, os_file_handle file)
{
    (void)hint;
    (void)prot;
    (void)flags;
    (void)file;
    return kernel_malloc(size);
}

void
os_munmap(void *addr, size_t size)
{
    (void)size;
    kernel_free(addr);
}

int
os_mprotect(void *addr, size_t size, int prot)
{
    (void)addr;
    (void)size;
    (void)prot;
    return 0; /* no MMU/MPU page protection for the wasm heap */
}

void *
os_mremap(void *old_addr, size_t old_size, size_t new_size)
{
    return os_mremap_slow(old_addr, old_size, new_size);
}

/* ---- time ---- */

uint64
os_time_get_boot_us(void)
{
    return (uint64)ticks_to_milliseconds(xTaskGetTickCount()) * 1000;
}

uint64
os_time_thread_cputime_us(void)
{
    return os_time_get_boot_us();
}

uint64
bh_get_tick_ms(void)
{
    return (uint64)ticks_to_milliseconds(xTaskGetTickCount());
}

/* ---- threads ---- */

korp_tid
os_self_thread(void)
{
    return (korp_tid)xTaskGetCurrentTaskHandle();
}

uint8 *
os_thread_get_stack_boundary(void)
{
    /* Find the CURRENT task's stack base (lowest address) so WAMR's
       native-stack-overflow guard raises a catchable trap instead of the task
       hard-faulting: the classic interpreter recurses in C per WASM call and a
       deep Flutter re-render (tap -> build/layout/paint) can exceed the 32KB
       app-task stack. FreeRTOS has no per-task accessor in this kernel, so scan
       uxTaskGetSystemState for our handle (configUSE_TRACE_FACILITY=1 exposes
       pxStack). Called once per exec-env bind, not per wasm call, so the scan
       cost is fine. NOTE: the dart exec env is entered from BOTH KernelBG (the
       console/dev path) and the app task (button clicks) -- the embedder must
       re-bind the boundary at each cross-task entry (see dart_runtime.c). */
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    static TaskStatus_t statuses[24];
    UBaseType_t n = uxTaskGetSystemState(statuses,
                                         sizeof(statuses) / sizeof(statuses[0]),
                                         NULL);
    for (UBaseType_t i = 0; i < n; i++) {
        if (statuses[i].xHandle == self) {
            return (uint8_t *)statuses[i].pxStack;
        }
    }
    return NULL; /* unknown task: guard disabled, previous behavior */
}

void
os_thread_jit_write_protect_np(bool enabled)
{
    (void)enabled; /* no JIT */
}

/* ---- mutex (PebbleOS) ---- */

int
os_mutex_init(korp_mutex *mutex)
{
    PebbleMutex *m = mutex_create();
    if (m == (PebbleMutex *)INVALID_MUTEX_HANDLE)
        return BHT_ERROR;
    *mutex = m;
    return BHT_OK;
}

int
os_mutex_destroy(korp_mutex *mutex)
{
    if (mutex && *mutex)
        mutex_destroy(*mutex);
    return BHT_OK;
}

int
os_mutex_lock(korp_mutex *mutex)
{
    mutex_lock(*mutex);
    return BHT_OK;
}

int
os_mutex_unlock(korp_mutex *mutex)
{
    mutex_unlock(*mutex);
    return BHT_OK;
}

/* ---- condition variable (FreeRTOS binary semaphore) ----
   Minimal correct implementation; only exercised if the runtime spawns
   threads, which the on-device single-module path does not. */

int
os_cond_init(korp_cond *cond)
{
    cond->sem = (void *)xSemaphoreCreateBinary();
    return cond->sem ? BHT_OK : BHT_ERROR;
}

int
os_cond_destroy(korp_cond *cond)
{
    if (cond->sem)
        vSemaphoreDelete((SemaphoreHandle_t)cond->sem);
    return BHT_OK;
}

int
os_cond_signal(korp_cond *cond)
{
    xSemaphoreGive((SemaphoreHandle_t)cond->sem);
    return BHT_OK;
}

int
os_cond_broadcast(korp_cond *cond)
{
    xSemaphoreGive((SemaphoreHandle_t)cond->sem);
    return BHT_OK;
}

int
os_cond_reltimedwait(korp_cond *cond, korp_mutex *mutex, uint64 useconds)
{
    TickType_t ticks;

    if (useconds == (uint64)-1)
        ticks = portMAX_DELAY;
    else
        ticks = milliseconds_to_ticks((uint32_t)(useconds / 1000));

    os_mutex_unlock(mutex);
    xSemaphoreTake((SemaphoreHandle_t)cond->sem, ticks);
    os_mutex_lock(mutex);
    return BHT_OK;
}

/* ---- console ---- */

int
os_vprintf(const char *format, va_list ap)
{
    char buf[128];
    int n = vsnprintf(buf, sizeof(buf), format, ap);
    dbgserial_putstr(buf);
    return n;
}

int
os_printf(const char *format, ...)
{
    int n;
    va_list ap;
    va_start(ap, format);
    n = os_vprintf(format, ap);
    va_end(ap);
    return n;
}

int
os_dumps_proc_mem_info(char *out, unsigned int size)
{
    (void)out;
    (void)size;
    return -1;
}
