/* SPDX-FileCopyrightText: 2026 Jonah Walker */
/* SPDX-License-Identifier: Apache-2.0 */

//! The Dart `dart:_embedder` standalone interface, implemented as a WAMR native
//! "dart" module. Strings are host objects (UTF-16) wrapped as externref.
//! print() goes to the debug serial. Adapted from the proven host harness.

#include "dart_embedder.h"

#include "console/dbgserial.h"
#include "system/logging.h"

#include "applib/app.h"
#include "applib/graphics/gcolor_definitions.h"
#include "applib/graphics/gtypes.h"
#include "pbl/services/compositor/compositor.h"
#include "drivers/rtc.h"
#include "util/time/time.h"

#include "wasm_export.h"
#include "gc_export.h"
#include "wamr_pebble_glue.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>
#include <string.h>

/* The firmware builds -ffreestanding with a reduced libc header set; strtod is
   implemented by dart_libc_shim.c but not declared by the in-scope headers. */
extern double strtod(const char *nptr, char **endptr);
/* libm (newlib) is linked; the freestanding math.h omits these decls. */
extern double sin(double), cos(double), asin(double), exp(double), log(double);
/* atan2/pow are not in the firmware libm subset -- provided below. */

/* ---- host string object (UTF-16), wrapped as externref ---- */
typedef struct {
  uint16_t *data;
  uint32_t length;   /* code units */
  uint32_t capacity; /* for string buffers */
  int is_buffer;
} HStr;

/* ALL host-string allocations in this section (headers here, data buffers via
   wasm_runtime_malloc directly) draw from the WAMR pool (8MB PSRAM), NOT the
   SRAM kernel heap: HStrs are freed only when the wasm GC runs their
   finalizers, and the low-garbage render pipeline can defer GC for many frames
   -- long enough for per-frame strings to exhaust the small kernel heap (the
   count~8/19 "safely rebooted due to OOM"). The pool has three orders of
   magnitude more headroom and the GC's own heap lives there anyway. */
static void *prv_hstr_alloc(uint32_t size) {
  void *p = wasm_runtime_malloc(size);
  if (p) memset(p, 0, size);
  return p;
}

static HStr *prv_hstr_new(uint32_t length) {
  HStr *s = (HStr *)prv_hstr_alloc(sizeof(HStr));
  if (!s) {
    return NULL;
  }
  s->length = length;
  s->capacity = length ? length : 8;
  s->data = (uint16_t *)wasm_runtime_malloc(s->capacity * sizeof(uint16_t));
  if (!s->data) {
    wasm_runtime_free(s);
    return NULL;
  }
  return s;
}

static void prv_hstr_push(HStr *b, uint16_t u) {
  if (b->length == b->capacity) {
    uint16_t *grown =
        (uint16_t *)wasm_runtime_malloc(b->capacity * 2 * sizeof(uint16_t));
    if (!grown) {
      return; /* OOM: drop the unit rather than deref NULL; string truncates */
    }
    memcpy(grown, b->data, b->length * sizeof(uint16_t));
    wasm_runtime_free(b->data);
    b->data = grown;
    b->capacity *= 2;
  }
  b->data[b->length++] = u;
}

/* UTF-16 -> UTF-8 into a pool-allocated NUL-terminated buffer (caller frees). */
static char *prv_hstr_to_utf8(const HStr *s) {
  char *out = (char *)wasm_runtime_malloc(s->length * 3 + 1);
  uint32_t i, o = 0;
  if (!out) {
    return NULL;
  }
  for (i = 0; i < s->length; i++) {
    uint32_t cp = s->data[i];
    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s->length &&
        s->data[i + 1] >= 0xDC00 && s->data[i + 1] <= 0xDFFF) {
      cp = 0x10000 + ((cp - 0xD800) << 10) + (s->data[i + 1] - 0xDC00);
      i++;
    }
    if (cp < 0x80) {
      out[o++] = (char)cp;
    } else if (cp < 0x800) {
      out[o++] = (char)(0xC0 | (cp >> 6));
      out[o++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out[o++] = (char)(0xE0 | (cp >> 12));
      out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
      out[o++] = (char)(0x80 | (cp & 0x3F));
    } else {
      out[o++] = (char)(0xF0 | (cp >> 18));
      out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
      out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
      out[o++] = (char)(0x80 | (cp & 0x3F));
    }
  }
  out[o] = 0;
  return out;
}

static HStr *prv_hstr_from_utf8(const char *s, uint32_t len) {
  HStr *r = prv_hstr_new(0);
  uint32_t i = 0;
  if (!r) {
    return NULL;
  }
  while (i < len) {
    uint8_t c = (uint8_t)s[i];
    uint32_t cp, n;
    if (c < 0x80) {
      cp = c;
      n = 1;
    } else if ((c & 0xE0) == 0xC0) {
      cp = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F);
      n = 2;
    } else if ((c & 0xF0) == 0xE0) {
      cp = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F);
      n = 3;
    } else {
      cp = ((uint32_t)(c & 0x07) << 18) | ((s[i + 1] & 0x3F) << 12) |
           ((s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F);
      n = 4;
    }
    i += n;
    if (cp > 0xFFFF) {
      cp -= 0x10000;
      prv_hstr_push(r, (uint16_t)(0xD800 + (cp >> 10)));
      prv_hstr_push(r, (uint16_t)(0xDC00 + (cp & 0x3FF)));
    } else {
      prv_hstr_push(r, (uint16_t)cp);
    }
  }
  return r;
}

/* Free the host HStr (or a single malloc'd payload) when its externref wrapper
   is GC-collected. Without this every string op leaks pool RAM (host-side payload
   behind the externref is not GC-managed). */
static void prv_hstr_finalizer(const wasm_obj_t obj, void *data) {
  HStr *s = (HStr *)data;
  (void)obj;
  if (s) {
    wasm_runtime_free(s->data);
    wasm_runtime_free(s);
  }
}
static void prv_host_free_finalizer(const wasm_obj_t obj, void *data) {
  (void)obj;
  wasm_runtime_free(data);
}

/* externref <-> HStr* helpers */
static wasm_externref_obj_t prv_wrap(wasm_exec_env_t env, HStr *s) {
  wasm_externref_obj_t ref = wasm_externref_obj_new(env, s);
  if (ref && s) {
    wasm_obj_set_gc_finalizer(env, (wasm_obj_t)ref, prv_hstr_finalizer, s);
  } else if (s) {
    /* No wrapper means no finalizer will ever run; free the host side now. */
    wasm_runtime_free(s->data);
    wasm_runtime_free(s);
  }
  return ref;
}

static HStr *prv_unwrap(wasm_externref_obj_t ref) {
  return ref ? (HStr *)wasm_externref_obj_get_value(ref) : NULL;
}

/* ---- the 11 dart.* native functions ---- */

// Capture the most recent Dart print() line so C can ASSERT the computed value (e.g. "sum=45")
// rather than trusting "ran without trapping" -- silent PSRAM/heap corruption could yield a wrong
// but non-trapping result. dart print() goes to dbgserial + PBL_LOG (neither reaches the BLE
// console), so this buffer is how the runtime surfaces + checks the literal result.
static char s_dart_last_print[160];
const char *dart_embedder_last_print(void) { return s_dart_last_print; }
void dart_embedder_clear_last_print(void) { s_dart_last_print[0] = '\0'; }

static void prv_print(wasm_exec_env_t env, wasm_externref_obj_t line) {
  HStr *s = prv_unwrap(line);
  if (s) {
    char *u = prv_hstr_to_utf8(s);
    dbgserial_putstr(u ? u : "");
    // Also log so Dart's print() is visible over `pebble logs` on a sealed watch.
    PBL_LOG_ALWAYS("dart print: %s", u ? u : "");
    if (u) {
      snprintf(s_dart_last_print, sizeof(s_dart_last_print), "%s", u);
    }
    wasm_runtime_free(u);
  } else {
    dbgserial_putstr("(null)");
  }
}

static wasm_externref_obj_t prv_string_from_ascii_bytes(wasm_exec_env_t env,
                                                        wasm_array_obj_t arr,
                                                        int32_t start,
                                                        int32_t length) {
  HStr *s = prv_hstr_new((uint32_t)length);
  int32_t i;
  if (!s) {
    return NULL;
  }
  for (i = 0; i < length; i++) {
    wasm_value_t v;
    wasm_array_obj_get_elem(arr, (uint32_t)(start + i), false, &v);
    s->data[i] = (uint16_t)(v.i32 & 0xFF);
  }
  s->length = (uint32_t)length;
  return prv_wrap(env, s);
}

static wasm_externref_obj_t prv_i64_to_string(wasm_exec_env_t env, int64_t value,
                                              int32_t radix) {
  char buf[72];
  if (radix == 16) {
    snprintf(buf, sizeof buf, "%" PRIx64, value);
  } else {
    snprintf(buf, sizeof buf, "%" PRId64, value);
  }
  return prv_wrap(env, prv_hstr_from_utf8(buf, (uint32_t)strlen(buf)));
}

static wasm_externref_obj_t prv_f64_to_string(wasm_exec_env_t env, double value) {
  char buf[40];
  /* Dart has specific double formatting; %g is adequate for the first cut. */
  snprintf(buf, sizeof buf, "%g", value);
  return prv_wrap(env, prv_hstr_from_utf8(buf, (uint32_t)strlen(buf)));
}

static wasm_externref_obj_t prv_string_buffer_create(wasm_exec_env_t env) {
  HStr *b = prv_hstr_new(0);
  if (b) {
    b->is_buffer = 1;
  }
  return prv_wrap(env, b);
}

static int32_t prv_string_length(wasm_exec_env_t env,
                                 wasm_externref_obj_t string) {
  HStr *s = prv_unwrap(string);
  return s ? (int32_t)s->length : 0;
}

static void prv_string_buffer_write_string(wasm_exec_env_t env,
                                           wasm_externref_obj_t buffer,
                                           wasm_externref_obj_t string) {
  HStr *b = prv_unwrap(buffer), *s = prv_unwrap(string);
  uint32_t i;
  if (!b || !s) {
    return;
  }
  for (i = 0; i < s->length; i++) {
    prv_hstr_push(b, s->data[i]);
  }
}

static wasm_externref_obj_t prv_string_buffer_to_string(
    wasm_exec_env_t env, wasm_externref_obj_t buffer) {
  HStr *b = prv_unwrap(buffer);
  HStr *s;
  uint32_t i;
  if (!b) {
    return NULL;
  }
  s = prv_hstr_new(b->length);
  if (!s) {
    return NULL;
  }
  for (i = 0; i < b->length; i++) {
    s->data[i] = b->data[i];
  }
  s->length = b->length;
  return prv_wrap(env, s);
}

static wasm_externref_obj_t prv_stack_trace_get_current(wasm_exec_env_t env) {
  return prv_wrap(env, prv_hstr_from_utf8("", 0));
}

static wasm_externref_obj_t prv_stack_trace_to_string(
    wasm_exec_env_t env, wasm_externref_obj_t trace) {
  HStr *s = prv_unwrap(trace);
  return s ? trace : prv_wrap(env, prv_hstr_from_utf8("", 0));
}

static wasm_externref_obj_t prv_json_encode_string(wasm_exec_env_t env,
                                                   wasm_externref_obj_t line) {
  /* Minimal: wrap in quotes (full JSON escaping is a follow-up). */
  HStr *s = prv_unwrap(line), *r;
  uint32_t i;
  if (!s) {
    return NULL;
  }
  r = prv_hstr_new(0);
  if (!r) {
    return NULL;
  }
  prv_hstr_push(r, '"');
  for (i = 0; i < s->length; i++) {
    prv_hstr_push(r, s->data[i]);
  }
  prv_hstr_push(r, '"');
  return prv_wrap(env, r);
}

/* ---- extended string/number natives (richer modules) ---- */

static int32_t prv_string_equals(wasm_exec_env_t env, wasm_externref_obj_t a, wasm_externref_obj_t b) {
  HStr *sa = prv_unwrap(a), *sb = prv_unwrap(b);
  if (sa == sb) return 1;
  if (!sa || !sb || sa->length != sb->length) return 0;
  for (uint32_t i = 0; i < sa->length; i++) {
    if (sa->data[i] != sb->data[i]) return 0;
  }
  return 1;
}
static int32_t prv_string_code_unit_at(wasm_exec_env_t env, wasm_externref_obj_t a, int32_t index) {
  HStr *s = prv_unwrap(a);
  if (!s || index < 0 || (uint32_t)index >= s->length) return 0;
  return (int32_t)s->data[index];
}
static int32_t prv_string_last_index_of_string(wasm_exec_env_t env, wasm_externref_obj_t a,
                                               wasm_externref_obj_t b, int32_t start) {
  HStr *sa = prv_unwrap(a), *sb = prv_unwrap(b);
  if (!sa || !sb || start < 0) return -1;
  if (sb->length == 0) return (uint32_t)start > sa->length ? (int32_t)sa->length : start;
  if (sb->length > sa->length) return -1;
  int32_t hi = (int32_t)(sa->length - sb->length);
  if (start < hi) hi = start;
  for (int32_t i = hi; i >= 0; i--) {
    uint32_t j = 0;
    while (j < sb->length && sa->data[i + j] == sb->data[j]) j++;
    if (j == sb->length) return i;
  }
  return -1;
}
static wasm_externref_obj_t prv_string_substring(wasm_exec_env_t env, wasm_externref_obj_t a,
                                                 int32_t start, int32_t end) {
  HStr *s = prv_unwrap(a);
  if (!s) return NULL;
  if (start < 0) start = 0;
  if (end > (int32_t)s->length) end = (int32_t)s->length;
  if (end < start) end = start;
  uint32_t len = (uint32_t)(end - start);
  HStr *r = prv_hstr_new(len);
  if (!r) return NULL;
  for (uint32_t i = 0; i < len; i++) r->data[i] = s->data[start + i];
  r->length = len;
  return prv_wrap(env, r);
}
static wasm_externref_obj_t prv_string_concat(wasm_exec_env_t env, wasm_externref_obj_t a,
                                              wasm_externref_obj_t b) {
  HStr *sa = prv_unwrap(a), *sb = prv_unwrap(b);
  uint32_t la = sa ? sa->length : 0, lb = sb ? sb->length : 0;
  HStr *r = prv_hstr_new(la + lb);
  if (!r) return NULL;
  for (uint32_t i = 0; i < la; i++) r->data[i] = sa->data[i];
  for (uint32_t i = 0; i < lb; i++) r->data[la + i] = sb->data[i];
  r->length = la + lb;
  return prv_wrap(env, r);
}
static wasm_externref_obj_t prv_string_repeat(wasm_exec_env_t env, wasm_externref_obj_t a, int32_t times) {
  HStr *s = prv_unwrap(a);
  if (!s || times < 0) return NULL;
  uint32_t total = s->length * (uint32_t)times;
  HStr *r = prv_hstr_new(total);
  if (!r) return NULL;
  uint32_t o = 0;
  for (int32_t t = 0; t < times; t++) {
    for (uint32_t i = 0; i < s->length; i++) r->data[o++] = s->data[i];
  }
  r->length = total;
  return prv_wrap(env, r);
}
static wasm_externref_obj_t prv_f64_to_fixed(wasm_exec_env_t env, double value, int32_t frac) {
  char buf[64];
  if (frac < 0) frac = 0;
  if (frac > 20) frac = 20;
  snprintf(buf, sizeof buf, "%.*f", (int)frac, value);
  return prv_wrap(env, prv_hstr_from_utf8(buf, (uint32_t)strlen(buf)));
}
static uint64_t s_rng = 0x2545F4914F6CDD1DULL;
static int64_t prv_random_int(wasm_exec_env_t env) {
  s_rng = s_rng * 6364136223846793005ULL + 1442695040888963407ULL;
  return (int64_t)s_rng;
}

/* ---- framework natives (the Flutter surface) ---- */

static int32_t prv_timeline_stream_enabled(wasm_exec_env_t env) { return 0; }
static int32_t prv_report_task_event(wasm_exec_env_t env, int32_t a, int32_t b, int32_t c,
                                     wasm_externref_obj_t d, wasm_externref_obj_t e) { return 0; }
static int32_t prv_monotonic_clock_frequency(wasm_exec_env_t env) { return 1000000; }
extern uint64_t bh_get_tick_ms(void);
static int64_t prv_monotonic_clock_ticks(wasm_exec_env_t env) {
  /* REAL time (ms resolution scaled to the advertised 1MHz tick): Flutter's
     animations, gesture velocity, and frame timestamps all measure this
     clock. The old virtual +16.7ms-per-query version made every animation
     replay its full 60fps frame count regardless of real frame cost -- at
     ~13s/frame that turned one FAB tap into ~44 invisible renders (~10min). */
  return (int64_t)bh_get_tick_ms() * 1000;
}
static int64_t prv_current_time(wasm_exec_env_t env) {
  /* LOCAL wall-clock micros (the tz-offset native returns 0), so DateTime.now()
     shows local time on the watchface. The phone sets the RTC (UTC) + timezone;
     time_utc_to_local applies the offset. */
  return (int64_t)time_utc_to_local(rtc_get_time()) * 1000000LL;
}
static int32_t prv_timezone_offset(wasm_exec_env_t env, int64_t secs) {
  (void)secs;
  return 0; /* prv_current_time already returns local time */
}
static int32_t prv_string_compare(wasm_exec_env_t env, wasm_externref_obj_t a, wasm_externref_obj_t b) {
  HStr *sa = prv_unwrap(a), *sb = prv_unwrap(b);
  uint32_t la = sa ? sa->length : 0, lb = sb ? sb->length : 0, n = la < lb ? la : lb;
  for (uint32_t i = 0; i < n; i++) {
    if (sa->data[i] != sb->data[i]) return sa->data[i] < sb->data[i] ? -1 : 1;
  }
  return la == lb ? 0 : (la < lb ? -1 : 1);
}
static int32_t prv_string_index_of_string(wasm_exec_env_t env, wasm_externref_obj_t a,
                                          wasm_externref_obj_t b, int32_t start) {
  HStr *sa = prv_unwrap(a), *sb = prv_unwrap(b);
  if (!sa || !sb) return -1;
  if (start < 0) start = 0;
  if (sb->length == 0) return (uint32_t)start > sa->length ? (int32_t)sa->length : start;
  if (sb->length > sa->length) return -1;
  for (int32_t i = start; i <= (int32_t)(sa->length - sb->length); i++) {
    uint32_t j = 0;
    while (j < sb->length && sa->data[i + j] == sb->data[j]) j++;
    if (j == sb->length) return i;
  }
  return -1;
}
static wasm_externref_obj_t prv_string_to_lower_case(wasm_exec_env_t env, wasm_externref_obj_t a) {
  HStr *s = prv_unwrap(a);
  if (!s) return NULL;
  HStr *r = prv_hstr_new(s->length);
  if (!r) return NULL;
  for (uint32_t i = 0; i < s->length; i++) {
    uint16_t c = s->data[i];
    r->data[i] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
  }
  r->length = s->length;
  return prv_wrap(env, r);
}
static wasm_externref_obj_t prv_string_to_upper_case(wasm_exec_env_t env, wasm_externref_obj_t a) {
  HStr *s = prv_unwrap(a);
  if (!s) return NULL;
  HStr *r = prv_hstr_new(s->length);
  if (!r) return NULL;
  for (uint32_t i = 0; i < s->length; i++) {
    uint16_t c = s->data[i];
    r->data[i] = (c >= 'a' && c <= 'z') ? c - 32 : c;
  }
  r->length = s->length;
  return prv_wrap(env, r);
}
static void prv_string_buffer_clear(wasm_exec_env_t env, wasm_externref_obj_t buffer) {
  HStr *b = prv_unwrap(buffer);
  if (b) b->length = 0;
}
static int32_t prv_string_buffer_length(wasm_exec_env_t env, wasm_externref_obj_t buffer) {
  HStr *b = prv_unwrap(buffer);
  return b ? (int32_t)b->length : 0;
}
static void prv_string_buffer_write_char_code(wasm_exec_env_t env, wasm_externref_obj_t buffer, int32_t code) {
  HStr *b = prv_unwrap(buffer);
  if (b) prv_hstr_push(b, (uint16_t)code);
}
static wasm_externref_obj_t prv_string_from_char_code_array(wasm_exec_env_t env, wasm_array_obj_t arr,
                                                           int32_t start, int32_t length) {
  HStr *s = prv_hstr_new((uint32_t)length);
  if (!s) return NULL;
  for (int32_t i = 0; i < length; i++) {
    wasm_value_t v;
    wasm_array_obj_get_elem(arr, (uint32_t)(start + i), false, &v);
    s->data[i] = (uint16_t)(v.i32 & 0xFFFF);
  }
  s->length = (uint32_t)length;
  return prv_wrap(env, s);
}
static void prv_string_to_code_units(wasm_exec_env_t env, wasm_externref_obj_t string,
                                     wasm_array_obj_t outArray, int32_t startIndex) {
  HStr *s = prv_unwrap(string);
  if (!s) return;
  for (uint32_t i = 0; i < s->length; i++) {
    wasm_value_t v;
    v.i32 = (int32_t)s->data[i];
    wasm_array_obj_set_elem(outArray, (uint32_t)(startIndex + (int32_t)i), &v);
  }
}
static wasm_externref_obj_t prv_string_replace_all_string(wasm_exec_env_t env, wasm_externref_obj_t string,
                                                         wasm_externref_obj_t needle, wasm_externref_obj_t repl) {
  HStr *s = prv_unwrap(string), *n = prv_unwrap(needle), *rep = prv_unwrap(repl);
  if (!s) return NULL;
  HStr *r = prv_hstr_new(0);
  if (!r) return NULL;
  if (!n || n->length == 0) {
    for (uint32_t i = 0; i < s->length; i++) prv_hstr_push(r, s->data[i]);
    return prv_wrap(env, r);
  }
  uint32_t i = 0;
  while (i < s->length) {
    if (i + n->length <= s->length) {
      uint32_t j = 0;
      while (j < n->length && s->data[i + j] == n->data[j]) j++;
      if (j == n->length) {
        if (rep) for (uint32_t k = 0; k < rep->length; k++) prv_hstr_push(r, rep->data[k]);
        i += n->length;
        continue;
      }
    }
    prv_hstr_push(r, s->data[i++]);
  }
  return prv_wrap(env, r);
}
static wasm_externref_obj_t prv_string_replace_range(wasm_exec_env_t env, wasm_externref_obj_t string,
                                                     int32_t start, int32_t end, wasm_externref_obj_t repl) {
  HStr *s = prv_unwrap(string), *rep = prv_unwrap(repl);
  if (!s) return NULL;
  if (start < 0) start = 0;
  if (end > (int32_t)s->length) end = (int32_t)s->length;
  if (end < start) end = start;
  HStr *r = prv_hstr_new(0);
  if (!r) return NULL;
  for (int32_t i = 0; i < start; i++) prv_hstr_push(r, s->data[i]);
  if (rep) for (uint32_t k = 0; k < rep->length; k++) prv_hstr_push(r, rep->data[k]);
  for (uint32_t i = (uint32_t)end; i < s->length; i++) prv_hstr_push(r, s->data[i]);
  return prv_wrap(env, r);
}
static double prv_double_parse_infallible(wasm_exec_env_t env, wasm_externref_obj_t string) {
  HStr *s = prv_unwrap(string);
  if (!s) return 0.0;
  char *u = prv_hstr_to_utf8(s);
  double d = u ? strtod(u, NULL) : 0.0;
  wasm_runtime_free(u);
  return d;
}
typedef struct { double value; } DoubleBox;
static wasm_externref_obj_t prv_double_try_parse(wasm_exec_env_t env, wasm_externref_obj_t string) {
  HStr *s = prv_unwrap(string);
  if (!s) return NULL;
  char *u = prv_hstr_to_utf8(s);
  if (!u) return NULL;
  char *endp = NULL;
  double d = strtod(u, &endp);
  int ok = (endp != u && *endp == '\0');
  wasm_runtime_free(u);
  if (!ok) return NULL;
  DoubleBox *box = (DoubleBox *)wasm_runtime_malloc(sizeof(DoubleBox));
  if (!box) return NULL;
  box->value = d;
  wasm_externref_obj_t ref = wasm_externref_obj_new(env, box);
  if (ref) wasm_obj_set_gc_finalizer(env, (wasm_obj_t)ref, prv_host_free_finalizer, box);
  else wasm_runtime_free(box);
  return ref;
}
static double prv_try_parse_result_get_double(wasm_exec_env_t env, wasm_externref_obj_t result) {
  DoubleBox *box = result ? (DoubleBox *)wasm_externref_obj_get_value(result) : NULL;
  return box ? box->value : 0.0;
}
static int s_regexp_marker;

/* Persistent GC roots for weakref/expando targets+values (documented strong-ref
   cheat: pinned alive for the app's lifetime). Uses the local-obj-ref stack,
   which is a GC root and IS linked (wasm_runtime_pin_object is not). */
#define DART_PIN_MAX 16384
/* Pool-allocated (PSRAM), NOT static .bss: obelix SRAM is tight and a large
   static array here starved the boot-time allocations (v178 crash-looped). */
static WASMLocalObjectRef *s_pins;
static int s_pin_n;
static bool s_roots_bound;
//! Set when a WASM trap escaped a drained callback: Dart-side state is
//! undefined (finally blocks skipped) and the module must not be pumped again.
static bool s_module_trapped;

//! LIFO CONTRACT (hard-won): WAMR's local-obj-ref chain is strictly LIFO --
//! pushing a long-lived ref from inside a native call freezes every ref BELOW
//! it, and a later out-of-order pop leaves a DEAD STACK ref in the GC root
//! chain (the GC then chases FreeRTOS stack poison 0xa5a5a5a5 forever; that
//! was the tap hang). So: ALL slots are pushed ONCE here, at exec-env setup,
//! forming the stable bottom of the chain with val=NULL (the traverse range-
//! checks val, NULL is skipped). Pinning later only FILLS a slot -- no push,
//! no pop, LIFO untouched.
static void dart_embedder_bind_ev_roots(wasm_exec_env_t env);
static void dart_embedder_reset_ev_roots(void);

void dart_embedder_bind_roots(wasm_exec_env_t env) {
  if (s_roots_bound) return;
  if (!s_pins) {
    s_pins = (WASMLocalObjectRef *)wasm_runtime_malloc(DART_PIN_MAX * sizeof(WASMLocalObjectRef));
  }
  if (!s_pins) return;
  for (int i = 0; i < DART_PIN_MAX; i++) {
    wasm_runtime_push_local_obj_ref(env, &s_pins[i]);
  }
  dart_embedder_bind_ev_roots(env);
  s_roots_bound = true;
}

void dart_embedder_reset_roots(void) {
  /* The exec env (and its chain) is being destroyed; forget our fills. The
     pin array itself lives in the WAMR pool -- after a runtime teardown that
     memory is gone/reissued, so DROP the pointer (rebind reallocates). */
  s_roots_bound = false;
  s_pin_n = 0;
  s_pins = NULL;
  dart_embedder_reset_ev_roots();
  s_module_trapped = false;
}

static void prv_pin(wasm_exec_env_t env, wasm_obj_t obj) {
  (void)env;
  if (!obj || !s_roots_bound || !s_pins) return;
  if (s_pin_n >= DART_PIN_MAX) {
    /* A silently unpinned expando value is a use-after-free at a distance. */
    PBL_LOG_ERR("dart: pin table FULL (%d) -- object NOT rooted", s_pin_n);
    return;
  }
  s_pins[s_pin_n].val = obj;
  s_pin_n++;
}

/* Expando: per-expando linked list of (hash, target) -> value. */
typedef struct DartExpandoEntry {
  int64_t hash;
  wasm_obj_t target;
  wasm_obj_t value;
  struct DartExpandoEntry *next;
} DartExpandoEntry;
typedef struct { DartExpandoEntry *head; } DartExpandoHead;

static wasm_externref_obj_t prv_expando_create(wasm_exec_env_t env) {
  DartExpandoHead *h = (DartExpandoHead *)wasm_runtime_malloc(sizeof(DartExpandoHead));
  if (!h) return NULL;
  h->head = NULL;
  wasm_externref_obj_t ref = wasm_externref_obj_new(env, h);
  if (!ref) { wasm_runtime_free(h); return NULL; }
  return ref;
}
static wasm_obj_t prv_expando_get(wasm_exec_env_t env, wasm_externref_obj_t expando,
                                  wasm_obj_t target, int64_t hash) {
  (void)env;
  if (!expando) return NULL;
  DartExpandoHead *h = (DartExpandoHead *)wasm_externref_obj_get_value(expando);
  if (!h) return NULL;
  for (DartExpandoEntry *e = h->head; e; e = e->next) {
    if (e->hash == hash && e->target == target) return e->value;
  }
  return NULL;
}
static void prv_expando_set(wasm_exec_env_t env, wasm_externref_obj_t expando,
                            wasm_obj_t target, int64_t hash, wasm_obj_t value) {
  if (!expando) return;
  DartExpandoHead *h = (DartExpandoHead *)wasm_externref_obj_get_value(expando);
  if (!h) return;
  DartExpandoEntry *prev = NULL;
  for (DartExpandoEntry *e = h->head; e; prev = e, e = e->next) {
    if (e->hash == hash && e->target == target) {
      /* Overwrite WITHOUT re-pinning: the old pin slot keeps the old value
         alive harmlessly, and re-pinning every setState burns a slot per
         frame until the table exhausts. New values get pinned only when the
         ENTRY is created. (Slot-precise unpinning needs per-entry slot ids;
         not worth it while entries are append-mostly.) */
      if (!value) { if (prev) prev->next = e->next; else h->head = e->next; wasm_runtime_free(e); }
      else { e->value = value; prv_pin(env, value); }
      return;
    }
  }
  if (!value) return;
  DartExpandoEntry *e = (DartExpandoEntry *)wasm_runtime_malloc(sizeof(DartExpandoEntry));
  if (!e) return;
  e->hash = hash; e->target = target; e->value = value; e->next = h->head; h->head = e;
  prv_pin(env, target);
  prv_pin(env, value);
}

/* Weak references (strong-ref cheat: target pinned alive). */
static wasm_externref_obj_t prv_weak_ref_create(wasm_exec_env_t env, wasm_obj_t target) {
  wasm_obj_t *box = (wasm_obj_t *)wasm_runtime_malloc(sizeof(wasm_obj_t));
  if (!box) return NULL;
  *box = target;
  wasm_externref_obj_t ref = wasm_externref_obj_new(env, box);
  if (!ref) { wasm_runtime_free(box); return NULL; }
  prv_pin(env, target);
  return ref;
}
static wasm_obj_t prv_weak_ref_get(wasm_exec_env_t env, wasm_externref_obj_t ref) {
  (void)env;
  if (!ref) return NULL;
  wasm_obj_t *box = (wasm_obj_t *)wasm_externref_obj_get_value(ref);
  return box ? *box : NULL;
}

/* Math. sin/cos/asin/exp/log are in the firmware libm subset; atan2 + pow are
   not, so derive them (adequate precision for layout/colors). */
static double prv_atan_poly(double x) { /* |x| <= 1 minimax-ish */
  const double a = 0.9998660, b = -0.3302995, c = 0.1801410, d = -0.0851330, e = 0.0208351;
  double x2 = x * x;
  return x * (a + x2 * (b + x2 * (c + x2 * (d + x2 * e))));
}
static double prv_atan(double x) {
  const double HALF_PI = 1.5707963267948966;
  if (x >= 0) return x <= 1 ? prv_atan_poly(x) : HALF_PI - prv_atan_poly(1.0 / x);
  return x >= -1 ? prv_atan_poly(x) : -HALF_PI - prv_atan_poly(1.0 / x);
}
static double prv_atan2(double y, double x) {
  const double PI = 3.141592653589793, HALF_PI = 1.5707963267948966;
  if (x > 0) return prv_atan(y / x);
  if (x < 0) return y >= 0 ? prv_atan(y / x) + PI : prv_atan(y / x) - PI;
  return y > 0 ? HALF_PI : (y < 0 ? -HALF_PI : 0.0);
}
static double prv_pow(double b, double x) {
  if (b > 0) return exp(x * log(b));
  if (b == 0) return x == 0 ? 1.0 : 0.0;
  /* negative base: only integer exponents are real; use |b| and sign parity */
  double r = exp(x * log(-b));
  long xi = (long)x;
  return ((double)xi == x && (xi & 1)) ? -r : r;
}
static double prv_math_sin(wasm_exec_env_t e, double x) { (void)e; return sin(x); }
static double prv_math_cos(wasm_exec_env_t e, double x) { (void)e; return cos(x); }
static double prv_math_asin(wasm_exec_env_t e, double x) { (void)e; return asin(x); }
static double prv_math_atan2(wasm_exec_env_t e, double y, double x) { (void)e; return prv_atan2(y, x); }
static double prv_math_exp(wasm_exec_env_t e, double x) { (void)e; return exp(x); }
static double prv_math_log(wasm_exec_env_t e, double x) { (void)e; return log(x); }
static double prv_math_pow(wasm_exec_env_t e, double b, double x) { (void)e; return prv_pow(b, x); }

static wasm_externref_obj_t prv_f64_to_precision(wasm_exec_env_t env, double value, int32_t digits) {
  char buf[40];
  snprintf(buf, sizeof buf, "%.*g", (int)(digits < 1 ? 1 : digits), value);
  return prv_wrap(env, prv_hstr_from_utf8(buf, (uint32_t)strlen(buf)));
}
static wasm_externref_obj_t prv_regexp_create(wasm_exec_env_t env, wasm_externref_obj_t s,
                                              int32_t ml, int32_t cs, int32_t uni, int32_t da) {
  return wasm_externref_obj_new(env, &s_regexp_marker);
}
static int32_t prv_regexp_is_regexp(wasm_exec_env_t env, wasm_externref_obj_t r) { return 0; }
static wasm_externref_obj_t prv_regexp_match(wasm_exec_env_t env, wasm_externref_obj_t re,
                                             wasm_externref_obj_t s, int32_t start, int32_t asPrefix) {
  return NULL;
}
static wasm_externref_obj_t prv_regexp_match_get_group(wasm_exec_env_t env, wasm_externref_obj_t m, int32_t idx) {
  return NULL;
}
static int32_t prv_regexp_match_get_group_count(wasm_exec_env_t env, wasm_externref_obj_t m) { return 0; }

/* ---- event loop: microtasks + timers, drained after $invokeMain ---- */
typedef struct {
  wasm_func_obj_t cb;
  wasm_obj_t arg;
  int64_t due;
  uint32_t id; /* unique, nonzero; identifies the timer for clearSchedule */
} EvTask;
/* Sized from measured host peaks for the counter (root_peak=22, micro_peak=1,
   timer_peak=5, task_total=11) with ~20x margin -- MCU SRAM is tight (8192, the
   host value, overflowed the link by ~300KB). 512 -> ~24KB of .bss for the three
   arrays. EV_ROOT_MAX bounds total-tasks-per-drain (roots batch-release at drain
   end); an animation-heavy app would need per-task root recycling (arch #2). */
#define EV_MAX 512
static EvTask s_micro[EV_MAX];
static int s_micro_n;
static EvTask s_timer[EV_MAX];
static int s_timer_n;
static uint32_t s_next_timer_id = 1;
#define EV_ROOT_MAX (4 * EV_MAX) /* 2 fills per task, micro + timer */
static WASMLocalObjectRef *s_roots; /* pool-allocated at bind (SRAM .bss is tight) */
static int s_root_fill_hw; /* high-water of filled slots, for tail clearing */

//! Pre-pushed at exec-env setup (see dart_embedder_bind_roots); fill-only.
static void dart_embedder_bind_ev_roots(wasm_exec_env_t env) {
  if (!s_roots) {
    s_roots = (WASMLocalObjectRef *)wasm_runtime_malloc(EV_ROOT_MAX * sizeof(WASMLocalObjectRef));
  }
  if (!s_roots) {
    PBL_LOG_ERR("dart: ev root pool alloc failed");
    return;
  }
  for (int i = 0; i < EV_ROOT_MAX; i++) {
    wasm_runtime_push_local_obj_ref(env, &s_roots[i]);
  }
}

//! Rewrite the root fills from the CURRENT queue contents (call after every
//! queue mutation). Queued tasks stay rooted across drains; consumed ones
//! unroot at once. O(queued) -- bounded by EV_MAX, trivial at drain rates.
static void prv_ev_sync_roots(void) {
  if (!s_roots) return;
  int k = 0;
  for (int i = 0; i < s_micro_n; i++) {
    s_roots[k++].val = (wasm_obj_t)s_micro[i].cb;
    s_roots[k++].val = s_micro[i].arg;
  }
  for (int i = 0; i < s_timer_n; i++) {
    s_roots[k++].val = (wasm_obj_t)s_timer[i].cb;
    s_roots[k++].val = s_timer[i].arg;
  }
  for (int i = k; i < s_root_fill_hw; i++) {
    s_roots[i].val = NULL;
  }
  s_root_fill_hw = k;
}
static void dart_embedder_reset_ev_roots(void) {
  if (s_micro_n > 0 || s_timer_n > 0) {
    PBL_LOG_WRN("dart: %d micro + %d timers discarded at teardown (dead-instance funcrefs)",
                s_micro_n, s_timer_n);
  }
  s_micro_n = 0;
  s_timer_n = 0;
  s_root_fill_hw = 0;
  s_roots = NULL; /* pool memory dies with the runtime; rebind reallocates */
}
static void prv_ev_pin(wasm_exec_env_t env, const EvTask *t) {
  (void)env;
  (void)t;
  prv_ev_sync_roots();
}
static void prv_ev_invoke(wasm_exec_env_t env, const EvTask *t) {
  /* WAMR GC call ABI: a reference argument occupies 2 cells regardless of host
     pointer size (matches prv_call_invoke_main in dart_runtime.c). argv[1]=0. */
  uint32_t argv[2] = {0};
  uintptr_t a = (uintptr_t)t->arg;
  memcpy(argv, &a, sizeof a);
  wasm_runtime_call_func_ref(env, t->cb, 2, argv);
}
static void prv_queue_microtask(wasm_exec_env_t env, wasm_obj_t callback, wasm_obj_t arg) {
  if (s_micro_n >= EV_MAX) {
    /* Dart schedules its microtask loop exactly once per empty->non-empty
       transition; dropping this request stalls ALL microtasks forever. */
    PBL_LOG_ERR("dart: microtask queue FULL (%d) -- ASYNC STALLED", s_micro_n);
    return;
  }
  EvTask *t = &s_micro[s_micro_n++];
  t->cb = (wasm_func_obj_t)callback;
  t->arg = arg;
  t->due = 0;
  prv_ev_pin(env, t);
}
static wasm_externref_obj_t prv_schedule_once(wasm_exec_env_t env, int64_t delay,
                                              wasm_obj_t callback, wasm_obj_t arg) {
  if (s_timer_n >= EV_MAX) {
    PBL_LOG_ERR("dart: timer queue FULL (%d) -- timer dropped", s_timer_n);
    return NULL; /* NULL handle = Dart sees isActive=false (honest) */
  }
  EvTask *t = &s_timer[s_timer_n++];
  t->cb = (wasm_func_obj_t)callback;
  t->arg = arg;
  /* Absolute deadline: `delay` arrives in MICROseconds (Dart Duration). */
  int64_t now_ms = (int64_t)bh_get_tick_ms();
  t->due = now_ms + (delay < 0 ? 0 : delay / 1000);
  t->id = s_next_timer_id++;
  if (s_next_timer_id == 0) s_next_timer_id = 1; /* skip 0 (the "none" sentinel) */
  prv_ev_pin(env, t);
  /* Return the id boxed as an externref so Timer.cancel -> clearSchedule can
     find THIS timer. A no-op clearSchedule let InkWell's activation timer fire
     AFTER its widget was disposed -> statesController's internalStatesController!
     null-asserted -> device-only TypeError that killed every tap after the first. */
  uint32_t *box = (uint32_t *)wasm_runtime_malloc(sizeof(uint32_t));
  if (box) {
    *box = t->id;
    wasm_externref_obj_t h = wasm_externref_obj_new(env, box);
    if (h) {
      /* Free the box when Dart drops the handle (else a 4B pool leak per timer). */
      wasm_obj_set_gc_finalizer(env, (wasm_obj_t)h, prv_host_free_finalizer, box);
      return h;
    }
    wasm_runtime_free(box);
  }
  return NULL;
}
static void prv_clear_schedule(wasm_exec_env_t env, wasm_externref_obj_t handle) {
  (void)env;
  if (!handle) return;
  uint32_t *box = (uint32_t *)wasm_externref_obj_get_value(handle);
  if (!box) return;
  uint32_t id = *box;
  for (int i = 0; i < s_timer_n; i++) {
    if (s_timer[i].id == id) {
      memmove(s_timer + i, s_timer + i + 1, (size_t)(s_timer_n - i - 1) * sizeof(EvTask));
      s_timer_n--;
      prv_ev_sync_roots(); /* the removed timer's cb/arg unroot */
      return;
    }
  }
}

/* Drain microtasks then the earliest-due timer until both empty (a static app
   quiesces) or a cap trips. Public so dart_runtime.c can pump it after the
   module's main() returns and after each injected input. */
static uint32_t s_diag_tasks_drained;
uint32_t dart_embedder_diag_tasks_drained(void) {
  return s_diag_tasks_drained;
}

void dart_embedder_run_event_loop(wasm_exec_env_t env, wasm_module_inst_t inst) {
  /* The guard caps runaway callback chains (an app rescheduling itself forever)
     without tripping on real workloads: a Flutter frame drains in tens of tasks. */
  int guard = 0;
  bool tripped = false;
  s_diag_tasks_drained += (uint32_t)(s_micro_n + s_timer_n);
  while ((s_micro_n > 0 || s_timer_n > 0) && guard++ < 200000) {
    while (s_micro_n > 0) {
      EvTask t = s_micro[0];
      memmove(s_micro, s_micro + 1, (size_t)(--s_micro_n) * sizeof(EvTask));
      /* t is briefly unrooted here, but no GC-heap allocation can occur
         between this sync and the callee frame rooting the args (frame
         setup uses the exec-env stack, not the GC heap). */
      prv_ev_sync_roots();
      prv_ev_invoke(env, &t);
      if (wasm_runtime_get_exception(inst)) goto trapped;
    }
    if (s_timer_n > 0) {
      int best = 0, i;
      for (i = 1; i < s_timer_n; i++) {
        if (s_timer[i].due < s_timer[best].due) best = i;
      }
      /* Only run DUE timers: a not-yet-due earliest timer ends this drain --
         the frame tick pumps again in 33ms. Running future timers immediately
         made one pump play an entire animation to exhaustion (multi-second
         drains, per-drain root-cap pressure, wildly-early delayed timers). */
      if (s_timer[best].due > (int64_t)bh_get_tick_ms()) {
        break;
      }
      EvTask t = s_timer[best];
      memmove(s_timer + best, s_timer + best + 1, (size_t)(s_timer_n - best - 1) * sizeof(EvTask));
      s_timer_n--;
      prv_ev_sync_roots();
      prv_ev_invoke(env, &t);
      if (wasm_runtime_get_exception(inst)) goto trapped;
    }
  }
  if (guard >= 200000) {
    tripped = true;
    PBL_LOG_ERR("dart: event-loop guard tripped with %d micro + %d timers queued",
                s_micro_n, s_timer_n);
  }
  goto out;
trapped:
  tripped = true;
  PBL_LOG_ERR("dart: trap mid-drain (%d micro + %d timers dropped): %s",
              s_micro_n, s_timer_n, wasm_runtime_get_exception(inst));
  /* A trap skips Dart's finally blocks: the module's zone/microtask machinery
     is in an undefined state and MUST NOT be pumped again (the contract says
     a trapped instance is dead). Clearing the exception here only keeps the
     runtime's later teardown calls functional. */
  wasm_runtime_clear_exception(inst);
  s_module_trapped = true;
out:
  if (tripped) {
    /* A trapped/guard-tripped module must not be pumped again; its queued
       callbacks die here (and unroot via the sync below). */
    s_micro_n = 0;
    s_timer_n = 0;
  }
  prv_ev_sync_roots();
}

bool dart_embedder_module_trapped(void) { return s_module_trapped; }
void dart_embedder_clear_trapped(void) { s_module_trapped = false; }

/* ---- M3 present-frame + per-watch geometry bridges ---- */

/* Set once Flutter has painted at least one frame into the app framebuffer. The
   Counter app's root-layer update_proc reads this: before the first frame it draws a
   loading screen; after, it leaves the framebuffer alone (Flutter owns it). */
static bool s_frame_presented;
static int s_frame_count;
bool dart_embedder_frame_presented(void) { return s_frame_presented; }
int dart_embedder_frame_count(void) { return s_frame_count; }

/* Snapshot of the last presented frame (GColor8, stride == s_snap_w). Allocated from
   the WAMR pool (PSRAM) on first present, kept for the runtime's lifetime: presentFrame
   writes the FOREGROUND app's framebuffer, which that app repaints over, so this copy
   is the only stable record of what Flutter drew (read by the `ssapp` console command). */
static uint8_t *s_frame_snap;
static int32_t s_snap_w, s_snap_h;
const uint8_t *dart_embedder_frame_snapshot(int32_t *w_out, int32_t *h_out) {
  *w_out = s_snap_w;
  *h_out = s_snap_h;
  return s_frame_snap;
}

void dart_embedder_reset_frame(void) {
  s_frame_presented = false;
  s_frame_count = 0;
  if (s_frame_snap) {
    /* Clear the pixels too, or `ssapp` between app stop and the next first
       present serves the PREVIOUS app's frame as if it were current. */
    memset(s_frame_snap, 0, (size_t)(s_snap_w * s_snap_h));
  }
}

//! Called when the WAMR pool that backs s_frame_snap is destroyed
//! (dart_runtime_teardown), so the snapshot pointer's lifetime matches the pool's.
//! WITHOUT this, after a teardown + PSRAM power-down the pointer dangles into the
//! disabled PSRAM window and the next dart_embedder_reset_frame() would memset
//! into dead memory -> asynchronous bus fault (a silent reset with no coredump).
void dart_embedder_on_pool_destroyed(void) {
  s_frame_snap = NULL;
  s_snap_w = 0;
  s_snap_h = 0;
}

/* Blit a GColor8 frame ([pixels], 1 byte/px row-major [w]x[h] -- the dartui
   rasterizer works in the panel format) into the APP framebuffer with straight
   row copies, then request a normal app render. The compositor composites the
   app framebuffer to the panel every cycle (the same path every app uses), so
   the frame persists. We deliberately do NOT write the system framebuffer or
   freeze the compositor: doing that fought the OS from a non-foreground context
   and the frame never stuck (the watchface owned the screen). Clamps to the
   framebuffer bounds if the source size differs. */
static void prv_present_frame(wasm_exec_env_t env, wasm_array_obj_t pixels,
                              int32_t w, int32_t h) {
  GBitmap bmp = compositor_get_app_framebuffer_as_bitmap();
  uint8_t *fb = (uint8_t *)bmp.addr;
  if (!fb || w <= 0 || h <= 0) {
    return;
  }
  int32_t fbw = bmp.bounds.size.w, fbh = bmp.bounds.size.h;
  uint16_t rs = bmp.row_size_bytes;
  int32_t rows = h < fbh ? h : fbh;
  int32_t cols = w < fbw ? w : fbw;
  if (w != fbw || h != fbh) {
    PBL_LOG_ALWAYS("dart present: source %" PRId32 "x%" PRId32 " vs fb %" PRId32
                   "x%" PRId32 " (clamped)", w, h, fbw, fbh);
  }
  if (!s_frame_snap) {
    s_frame_snap = (uint8_t *)wasm_runtime_malloc((uint32_t)(fbw * fbh));
    if (s_frame_snap) {
      memset(s_frame_snap, 0, (size_t)(fbw * fbh));
      s_snap_w = fbw;
      s_snap_h = fbh;
    } else {
      /* Rendering still works; only the `ssapp` console readback degrades. */
      PBL_LOG_ALWAYS("dart present: no RAM for frame snapshot; ssapp disabled");
    }
  }
  /* The frame arrives in the panel's native GColor8 (1 byte/px, rasterized in
     that format Dart-side), so presenting is a straight row copy -- no per-
     pixel conversion. Raw element pointer via the glue (the array stays rooted
     for the whole native call; nothing below runs wasm/GC). */
  const uint8_t *px_data = wamr_pebble_array_u8_data(pixels);
  if (!px_data) {
    PBL_LOG_ALWAYS("dart present: pixel array is not 8-bit; frame dropped");
    return;
  }
  for (int32_t y = 0; y < rows; y++) {
    memcpy(&fb[y * rs], &px_data[y * w], (size_t)cols);
    if (s_frame_snap) {
      memcpy(&s_frame_snap[y * s_snap_w], &px_data[y * w], (size_t)cols);
    }
  }
  s_frame_presented = true;
  s_frame_count++;
  app_request_render();
}
static int32_t prv_display_width(wasm_exec_env_t env) {
  GBitmap bmp = compositor_get_app_framebuffer_as_bitmap();
  return (int32_t)bmp.bounds.size.w;
}
static int32_t prv_display_height(wasm_exec_env_t env) {
  GBitmap bmp = compositor_get_app_framebuffer_as_bitmap();
  return (int32_t)bmp.bounds.size.h;
}

/* signature chars: 'r' = externref/ref, 'i' = i32, 'I' = i64, 'F' = f64 */

/* Ordered-dither span helpers: the C loop replaces per-pixel interpreter
   writes (the dominant cost of dithered Material fills on-device). */
static const uint8_t k_bayer16[16] = {0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5};

static inline uint8_t prv_q2(int c, int th) {
  /* c 0..255, th 0..15; channel promotes when frac > (th+0.5)/16 */
  int v = c * 3, b = v / 255, rem = v - b * 255;
  if (rem * 32 > th * 510 + 255) b++;
  return b > 3 ? 3 : (uint8_t)b;
}

//! Bounds guard for the raw-pointer fill natives: these write straight into a
//! GC-heap array, so ANY overrun corrupts neighboring heap objects -- the
//! wasm-level fill8 (array.fill) traps on OOB, but these C loops would not.
//! Clamp and scream: the ERR names the culprit op instead of a later GC crash.
static bool prv_fill_bounds(wasm_obj_t fb, int32_t *offset, int32_t *len, const char *who) {
  uint32_t n = wasm_array_obj_length((wasm_array_obj_t)fb);
  if (*offset < 0 || *len <= 0 || (uint32_t)*offset >= n) {
    PBL_LOG_ERR("dart: %s OOB off=%ld len=%ld n=%lu", who, (long)*offset, (long)*len,
                (unsigned long)n);
    return false;
  }
  if ((uint32_t)*offset + (uint32_t)*len > n) {
    PBL_LOG_ERR("dart: %s CLAMP off=%ld len=%ld n=%lu", who, (long)*offset, (long)*len,
                (unsigned long)n);
    *len = (int32_t)(n - (uint32_t)*offset);
  }
  return true;
}

typedef struct { int mode; uint8_t c8; uint8_t p16[16]; int a, sr, sg, sb; } CShade;
static void prv_shade_of(int argb, CShade *sh) {
  int a = (argb >> 24) & 0xff, r = (argb >> 16) & 0xff, g = (argb >> 8) & 0xff, b = argb & 0xff;
  if (a < 0xF8) { sh->mode = 2; sh->a = a; sh->sr = r; sh->sg = g; sh->sb = b; return; }
  int same = 1;
  for (int i = 0; i < 16; i++) {
    int th = k_bayer16[i];
    sh->p16[i] = (uint8_t)(0xC0 | (prv_q2(r, th) << 4) | (prv_q2(g, th) << 2) | prv_q2(b, th));
    if (sh->p16[i] != sh->p16[0]) same = 0;
  }
  sh->mode = same ? 0 : 1;
  sh->c8 = sh->p16[0];
}
static inline void prv_fill_span(uint8_t *fb, int w, int y, int xa, int xb, const CShade *sh) {
  if (xb <= xa) return;
  uint8_t *row = fb + (size_t)y * w;
  if (sh->mode == 0) { memset(row + xa, sh->c8, (size_t)(xb - xa)); return; }
  if (sh->mode == 1) {
    int base = (y & 3) << 2;
    for (int x = xa; x < xb; x++) row[x] = sh->p16[base | (x & 3)];
    return;
  }
  int a = sh->a, ia = 255 - a; const uint8_t *brow = &k_bayer16[(y & 3) << 2];
  for (int x = xa; x < xb; x++) {
    uint8_t d = row[x];
    int r = (sh->sr * a + ((d >> 4) & 3) * 85 * ia) / 255;
    int g = (sh->sg * a + ((d >> 2) & 3) * 85 * ia) / 255;
    int b = (sh->sb * a + (d & 3) * 85 * ia) / 255;
    int th = brow[x & 3];
    row[x] = (uint8_t)(0xC0 | (prv_q2(r, th) << 4) | (prv_q2(g, th) << 2) | prv_q2(b, th));
  }
}

//! Native scanline polygon fill (mirror of engine.dart _fillPath): the
//! O(rows*edges) crossing+sort loop dominated the interpreter (~74% of a
//! frame's raster). meta = [w,h,nsub,argb,evenOdd,cx0,cy0,cx1,cy1,
//! rflag,rL,rT,rR,rB,rtl,rtr,rbr,rbl, len0,...]; rflag..rbl are an optional
//! rounded-rect clip narrowed per scanline (rflag=0 => plain rect scissor).
//! pts = concatenated subpath points in device px (packed to 3 ref args -- the
//! trampoline drops scalar args past the 8th).
// Max edge crossings tracked per scanline. Kept small: xs[]+wind[] live on the
// app task's stack, reached through a deep WASM interpreter call chain, so a
// large frame here risks a hard fault (see the C-stack-depth notes). Material
// paths cross <=~8 edges/scanline; the `nx < FP_MAX_XS` guard clamps any excess
// (that one scanline mis-fills, never crashes).
#define FP_MAX_XS 96
// fillPath meta index layout -- KEEP IN LOCKSTEP with engine.dart (_fpW.._fpHdr)
// and route_a/dart_host.c. Inserting a field shifts FPM_HDR (the lens start) and
// must be mirrored in all three, or every path fill corrupts on device.
enum { FPM_W=0, FPM_H=1, FPM_NSUB=2, FPM_ARGB=3, FPM_EVENODD=4,
       FPM_CX0=5, FPM_CY0=6, FPM_CX1=7, FPM_CY1=8,
       FPM_RFLAG=9, FPM_RL=10, FPM_RT=11, FPM_RR=12, FPM_RB=13,
       FPM_RTL=14, FPM_RTR=15, FPM_RBR=16, FPM_RBL=17, FPM_HDR=18 };
static void prv_fill_path(wasm_exec_env_t env, wasm_obj_t fb_obj, wasm_obj_t pts_obj,
                          wasm_obj_t meta_obj) {
  (void)env;
  // fb is a WasmArray<WasmI8> (1-byte elems); pts/meta are WasmArray<WasmF64>/
  // <WasmI32>, so they need the ANY-elem-size accessor -- wamr_pebble_array_u8_data
  // returns NULL for non-u8 arrays, which silently no-op'd every path fill.
  uint8_t *fb = (uint8_t *)wamr_pebble_array_u8_data(fb_obj);
  const double *pts = (const double *)wamr_pebble_array_raw_data(pts_obj);
  const int32_t *meta = (const int32_t *)wamr_pebble_array_raw_data(meta_obj);
  if (!fb || !pts || !meta) return;
  int w = meta[FPM_W], h = meta[FPM_H], nsub = meta[FPM_NSUB], argb = meta[FPM_ARGB], even_odd = meta[FPM_EVENODD];
  int cx0 = meta[FPM_CX0], cy0 = meta[FPM_CY0], cx1 = meta[FPM_CX1], cy1 = meta[FPM_CY1];
  // Rounded-clip params: rflag=1 => narrow [cx0,cx1] per scanline to a
  // rounded-rect [rL,rT,rR,rB] with circular corner radii (rtl,rtr,rbr,rbl), so
  // a Material clipped fill (a FAB focus highlight, a card body) rounds instead
  // of squaring to the bounding box.
  int rflag = meta[FPM_RFLAG];
  int rL = meta[FPM_RL], rT = meta[FPM_RT], rR = meta[FPM_RR], rB = meta[FPM_RB];
  int rtl = meta[FPM_RTL], rtr = meta[FPM_RTR], rbr = meta[FPM_RBR], rbl = meta[FPM_RBL];
  const int32_t *lens = meta + FPM_HDR;
  if (cx0 < 0) { cx0 = 0; }
  if (cy0 < 0) { cy0 = 0; }
  if (cx1 > w) { cx1 = w; }
  if (cy1 > h) { cy1 = h; }
  if (cx1 <= cx0 || cy1 <= cy0) return;
  CShade sh; prv_shade_of(argb, &sh);
  double xs[FP_MAX_XS]; int wind[FP_MAX_XS];
  for (int y = cy0; y < cy1; y++) {
    double yc = y + 0.5;
    // Per-scanline clip bounds, narrowed by any rounded corners.
    int lx = cx0, rx = cx1;
    if (rflag) {
      if (rtl > 0 && yc < rT + rtl) {
        double d = (rT + rtl) - yc, q = (double)rtl * rtl - d * d;
        int nl = (int)ceil(rL + (q > 0 ? rtl - sqrt(q) : rtl));
        if (nl > lx) { lx = nl; }
      }
      if (rtr > 0 && yc < rT + rtr) {
        double d = (rT + rtr) - yc, q = (double)rtr * rtr - d * d;
        int nr = (int)floor(rR - (q > 0 ? rtr - sqrt(q) : rtr));
        if (nr < rx) { rx = nr; }
      }
      if (rbl > 0 && yc > rB - rbl) {
        double d = yc - (rB - rbl), q = (double)rbl * rbl - d * d;
        int nl = (int)ceil(rL + (q > 0 ? rbl - sqrt(q) : rbl));
        if (nl > lx) { lx = nl; }
      }
      if (rbr > 0 && yc > rB - rbr) {
        double d = yc - (rB - rbr), q = (double)rbr * rbr - d * d;
        int nr = (int)floor(rR - (q > 0 ? rbr - sqrt(q) : rbr));
        if (nr < rx) { rx = nr; }
      }
      if (rx <= lx) continue;
    }
    int nx = 0, base = 0;
    for (int s = 0; s < nsub; s++) {
      int n = lens[s] / 2;
      const double *sp = pts + base;
      base += lens[s];
      if (n < 2) continue;
      for (int i = 0, j = n - 1; i < n; j = i++) {
        double xi = sp[i * 2], yi = sp[i * 2 + 1];
        double xj = sp[j * 2], yj = sp[j * 2 + 1];
        if ((yi <= yc && yj > yc) || (yj <= yc && yi > yc)) {
          if (nx < FP_MAX_XS) {
            xs[nx] = xi + (yc - yi) / (yj - yi) * (xj - xi);
            wind[nx] = yj > yi ? 1 : -1; nx++;
          }
        }
      }
    }
    if (nx == 0) continue;
    for (int a = 1; a < nx; a++) {
      double vx = xs[a]; int vw = wind[a]; int b = a - 1;
      while (b >= 0 && xs[b] > vx) { xs[b + 1] = xs[b]; wind[b + 1] = wind[b]; b--; }
      xs[b + 1] = vx; wind[b + 1] = vw;
    }
    if (even_odd) {
      for (int k = 0; k + 1 < nx; k += 2) {
        int xa = (int)ceil(xs[k]), xb = (int)ceil(xs[k + 1]);
        if (xa < lx) { xa = lx; }
        if (xb > rx) { xb = rx; }
        prv_fill_span(fb, w, y, xa, xb, &sh);
      }
    } else {
      int acc = 0;
      for (int k = 0; k + 1 < nx; k++) {
        acc += wind[k];
        if (acc != 0) {
          int xa = (int)ceil(xs[k]), xb = (int)ceil(xs[k + 1]);
          if (xa < lx) { xa = lx; }
          if (xb > rx) { xb = rx; }
          prv_fill_span(fb, w, y, xa, xb, &sh);
        }
      }
    }
  }
}

static void prv_fill_pattern(wasm_exec_env_t env, wasm_obj_t fb, int32_t offset, int32_t len,
                             int32_t b0, int32_t b1, int32_t b2, int32_t b3, int32_t phase) {
  if (!prv_fill_bounds(fb, &offset, &len, "fillPattern")) return;
  uint8_t *p = (uint8_t *)wamr_pebble_array_u8_data(fb) + offset;
  const uint8_t pat[4] = { (uint8_t)b0, (uint8_t)b1, (uint8_t)b2, (uint8_t)b3 };
  for (int32_t i = 0; i < len; i++) p[i] = pat[(phase + i) & 3];
}

static void prv_blend_fill(wasm_exec_env_t env, wasm_obj_t fb, int32_t offset, int32_t len,
                           int32_t argb, int32_t x_phase, int32_t y_phase) {
  if (!prv_fill_bounds(fb, &offset, &len, "blendFill")) return;
  uint8_t *p = (uint8_t *)wamr_pebble_array_u8_data(fb) + offset;
  int a = (argb >> 24) & 0xFF, ia = 255 - a;
  int sr = (argb >> 16) & 0xFF, sg = (argb >> 8) & 0xFF, sb = argb & 0xFF;
  const uint8_t *brow = &k_bayer16[(y_phase & 3) << 2];
  for (int32_t i = 0; i < len; i++) {
    uint8_t d = p[i];
    int r = (sr * a + ((d >> 4) & 3) * 85 * ia) / 255;
    int g = (sg * a + ((d >> 2) & 3) * 85 * ia) / 255;
    int b = (sb * a + (d & 3) * 85 * ia) / 255;
    int th = brow[(x_phase + i) & 3];
    p[i] = (uint8_t)(0xC0 | (prv_q2(r, th) << 4) | (prv_q2(g, th) << 2) | prv_q2(b, th));
  }
}

static NativeSymbol s_dart_natives[] = {
    {"print", prv_print, "(r)"},
    {"stringFromAsciiBytes", prv_string_from_ascii_bytes, "(rii)r"},
    {"i64ToString", prv_i64_to_string, "(Ii)r"},
    {"f64ToString", prv_f64_to_string, "(F)r"},
    {"stringBufferCreate", prv_string_buffer_create, "()r"},
    {"stringLength", prv_string_length, "(r)i"},
    {"stringBufferWriteString", prv_string_buffer_write_string, "(rr)"},
    {"stringBufferToString", prv_string_buffer_to_string, "(r)r"},
    {"stackTraceGetCurrent", prv_stack_trace_get_current, "()r"},
    {"stackTraceToString", prv_stack_trace_to_string, "(r)r"},
    {"jsonEncodeString", prv_json_encode_string, "(r)r"},
    {"stringEquals", prv_string_equals, "(rr)i"},
    {"stringCodeUnitAt", prv_string_code_unit_at, "(ri)i"},
    {"stringLastIndexOfString", prv_string_last_index_of_string, "(rri)i"},
    {"stringSubstring", prv_string_substring, "(rii)r"},
    {"stringConcat", prv_string_concat, "(rr)r"},
    {"stringRepeat", prv_string_repeat, "(ri)r"},
    {"f64ToFixed", prv_f64_to_fixed, "(Fi)r"},
    {"randomInt", prv_random_int, "()I"},
    {"fillPattern", prv_fill_pattern, "(riiiiiii)"},
    {"fillPath", prv_fill_path, "(rrr)"},
    {"blendFill", prv_blend_fill, "(riiiii)"},
    /* Flutter framework natives (ported from the host harness) */
    {"timelineStreamEnabled", prv_timeline_stream_enabled, "()i"},
    {"reportTaskEvent", prv_report_task_event, "(iiirr)i"},
    {"monotonicClockFrequency", prv_monotonic_clock_frequency, "()i"},
    {"monotonicClockTicks", prv_monotonic_clock_ticks, "()I"},
    {"currentTime", prv_current_time, "()I"},
    {"timeZoneOffsetInSecondsForClampedSeconds", prv_timezone_offset, "(I)i"},
    {"stringCompare", prv_string_compare, "(rr)i"},
    {"stringIndexOfString", prv_string_index_of_string, "(rri)i"},
    {"stringToLowerCase", prv_string_to_lower_case, "(r)r"},
    {"stringToUpperCase", prv_string_to_upper_case, "(r)r"},
    {"stringBufferClear", prv_string_buffer_clear, "(r)"},
    {"stringBufferLength", prv_string_buffer_length, "(r)i"},
    {"stringBufferWriteCharCode", prv_string_buffer_write_char_code, "(ri)"},
    {"stringFromCharCodeArray", prv_string_from_char_code_array, "(rii)r"},
    {"stringToCodeUnits", prv_string_to_code_units, "(rri)"},
    {"stringReplaceAllString", prv_string_replace_all_string, "(rrr)r"},
    {"stringReplaceRange", prv_string_replace_range, "(riir)r"},
    {"doubleParseInfallible", prv_double_parse_infallible, "(r)F"},
    {"doubleTryParse", prv_double_try_parse, "(r)r"},
    {"tryParseResultGetDouble", prv_try_parse_result_get_double, "(r)F"},
    {"expandoCreate", prv_expando_create, "()r"},
    {"expandoGet", prv_expando_get, "(rrI)r"},
    {"expandoSet", prv_expando_set, "(rrIr)"},
    {"weakRefCreate", prv_weak_ref_create, "(r)r"},
    {"weakRefGet", prv_weak_ref_get, "(r)r"},
    {"clearSchedule", prv_clear_schedule, "(r)"},
    {"mathSin", prv_math_sin, "(F)F"},
    {"mathCos", prv_math_cos, "(F)F"},
    {"mathAsin", prv_math_asin, "(F)F"},
    {"mathAtan2", prv_math_atan2, "(FF)F"},
    {"mathExp", prv_math_exp, "(F)F"},
    {"mathLog", prv_math_log, "(F)F"},
    {"mathPow", prv_math_pow, "(FF)F"},
    {"f64ToPrecision", prv_f64_to_precision, "(Fi)r"},
    {"queueMicrotask", prv_queue_microtask, "(rr)"},
    {"scheduleOnce", prv_schedule_once, "(Irr)r"},
    {"regexpCreateOrFailWithString", prv_regexp_create, "(riiii)r"},
    {"regexpIsRegexp", prv_regexp_is_regexp, "(r)i"},
    {"regexpMatch", prv_regexp_match, "(rrii)r"},
    {"regexpMatchGetGroup", prv_regexp_match_get_group, "(ri)r"},
    {"regexpMatchGetGroupCount", prv_regexp_match_get_group_count, "(r)i"},
    /* M3: present-frame + per-watch geometry bridges */
    {"presentFrame", prv_present_frame, "(rii)"},
    {"displayWidth", prv_display_width, "()i"},
    {"displayHeight", prv_display_height, "()i"},
};

NativeSymbol *dart_embedder_get_natives(uint32_t *count_out) {
  *count_out = sizeof(s_dart_natives) / sizeof(s_dart_natives[0]);
  return s_dart_natives;
}
