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
    /* Returning NULL disables WAMR's native-stack-overflow guard, which is
       acceptable for a single trusted module; the interpreter bounds its own
       value stack. */
    return NULL;
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
