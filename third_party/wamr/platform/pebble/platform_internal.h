/* SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception */
/* WAMR platform layer for PebbleOS (SiFli SF32LB52J, Cortex-M33, FreeRTOS).
   Opaque handle types keep the WAMR engine TUs free of FreeRTOS/PebbleOS
   headers; the concrete handles live in platform_pebble.c. */
#ifndef _PLATFORM_INTERNAL_H
#define _PLATFORM_INTERNAL_H

#include <inttypes.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <ctype.h>
#include <limits.h>
#include <errno.h>

#include "os/mutex.h" /* opaque PebbleMutex */

#ifdef __cplusplus
extern "C" {
#endif

#ifndef BH_PLATFORM_PEBBLE
#define BH_PLATFORM_PEBBLE
#endif

/* Native stack reserved for an applet thread's C frames. WAMR runs on an
   existing PebbleOS task, so this is advisory. */
#define BH_APPLET_PRESERVED_STACK_SIZE (4 * 1024)
#define BH_THREAD_DEFAULT_PRIORITY 0

/* Opaque handles (real types: TaskHandle_t / SemaphoreHandle_t, cast in .c). */
typedef void *korp_tid;
typedef void *korp_thread;
typedef PebbleMutex *korp_mutex;
typedef void *korp_sem;
typedef struct {
    void *sem; /* FreeRTOS binary semaphore */
} korp_cond;
typedef struct {
    int dummy; /* rwlock unused on-device */
} korp_rwlock;

#define os_thread_local_attribute
#define bh_socket_t int

/* File-handle types referenced by platform_api_extension.h (unused on-device). */
typedef int os_file_handle;
typedef void *os_dir_stream;
typedef int os_raw_file_handle;
typedef int os_poll_file_handle;
typedef unsigned int os_nfds_t;
typedef int os_timespec;

static inline os_file_handle
os_get_invalid_handle(void)
{
    return -1;
}

unsigned os_getpagesize(void);

#ifdef __cplusplus
}
#endif
#endif /* _PLATFORM_INTERNAL_H */
