/* SPDX-FileCopyrightText: 2026 Jonah Walker */
/* SPDX-License-Identifier: Apache-2.0 */

#include "system/log_ring.h"

#include <stdbool.h>
#include <stddef.h>

#define LOG_RING_SIZE 8192u

static char s_buf[LOG_RING_SIZE];
static volatile uint32_t s_head;   // next write index
static volatile uint32_t s_count;  // bytes stored (<= LOG_RING_SIZE)
static volatile bool s_busy;       // re-entrancy guard (avoid recursion via logging)

void log_ring_append(const char *str) {
  if (str == NULL || s_busy) {
    return;
  }
  s_busy = true;
  while (*str) {
    s_buf[s_head] = *str++;
    s_head = (s_head + 1u) % LOG_RING_SIZE;
    if (s_count < LOG_RING_SIZE) {
      s_count++;
    }
  }
  s_busy = false;
}

uint32_t log_ring_count(void) {
  uint32_t count = s_count;
  return (count > LOG_RING_SIZE) ? LOG_RING_SIZE : count;
}

uint32_t log_ring_read(uint32_t offset, uint8_t *out, uint32_t maxlen) {
  // Snapshot head/count once; clamp so a concurrent append can't drive us OOB.
  uint32_t count = s_count;
  uint32_t head = s_head;
  if (count > LOG_RING_SIZE) {
    count = LOG_RING_SIZE;
  }
  if (out == NULL || offset >= count) {
    return 0;
  }
  uint32_t avail = count - offset;
  uint32_t n = (avail < maxlen) ? avail : maxlen;
  uint32_t oldest = (head + LOG_RING_SIZE - count) % LOG_RING_SIZE;
  uint32_t start = (oldest + offset) % LOG_RING_SIZE;
  for (uint32_t i = 0; i < n; i++) {
    out[i] = (uint8_t)s_buf[(start + i) % LOG_RING_SIZE];
  }
  return n;
}
