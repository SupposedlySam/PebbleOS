/* SPDX-FileCopyrightText: 2026 Jonah Walker */
/* SPDX-License-Identifier: Apache-2.0 */

//! The Dart `dart:_embedder` standalone interface, implemented as a WAMR native
//! "dart" module. Strings are host objects (UTF-16) wrapped as externref.
//! print() goes to the debug serial. Adapted from the proven host harness.

#include "dart_embedder.h"

#include "console/dbgserial.h"
#include "kernel/pbl_malloc.h"
#include "system/logging.h"

#include "wasm_export.h"
#include "gc_export.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---- host string object (UTF-16), wrapped as externref ---- */
typedef struct {
  uint16_t *data;
  uint32_t length;   /* code units */
  uint32_t capacity; /* for string buffers */
  int is_buffer;
} HStr;

static HStr *prv_hstr_new(uint32_t length) {
  HStr *s = (HStr *)kernel_zalloc(sizeof(HStr));
  if (!s) {
    return NULL;
  }
  s->length = length;
  s->capacity = length ? length : 8;
  s->data = (uint16_t *)kernel_malloc(s->capacity * sizeof(uint16_t));
  if (!s->data) {
    kernel_free(s);
    return NULL;
  }
  return s;
}

static void prv_hstr_push(HStr *b, uint16_t u) {
  if (b->length == b->capacity) {
    b->capacity *= 2;
    b->data = (uint16_t *)kernel_realloc(b->data, b->capacity * sizeof(uint16_t));
  }
  b->data[b->length++] = u;
}

/* UTF-16 -> UTF-8 into a kernel-malloc'd NUL-terminated buffer (caller frees). */
static char *prv_hstr_to_utf8(const HStr *s) {
  char *out = (char *)kernel_malloc(s->length * 3 + 1);
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

/* externref <-> HStr* helpers */
static wasm_externref_obj_t prv_wrap(wasm_exec_env_t env, HStr *s) {
  return wasm_externref_obj_new(env, s);
}

static HStr *prv_unwrap(wasm_externref_obj_t ref) {
  return ref ? (HStr *)wasm_externref_obj_get_value(ref) : NULL;
}

/* ---- the 11 dart.* native functions ---- */

static void prv_print(wasm_exec_env_t env, wasm_externref_obj_t line) {
  HStr *s = prv_unwrap(line);
  if (s) {
    char *u = prv_hstr_to_utf8(s);
    dbgserial_putstr(u ? u : "");
    // Also log so Dart's print() is visible over `pebble logs` on a sealed watch.
    PBL_LOG_ALWAYS("dart print: %s", u ? u : "");
    kernel_free(u);
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
  prv_hstr_push(r, '"');
  for (i = 0; i < s->length; i++) {
    prv_hstr_push(r, s->data[i]);
  }
  prv_hstr_push(r, '"');
  return prv_wrap(env, r);
}

/* signature chars: 'r' = externref/ref, 'i' = i32, 'I' = i64, 'F' = f64 */
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
};

NativeSymbol *dart_embedder_get_natives(uint32_t *count_out) {
  *count_out = sizeof(s_dart_natives) / sizeof(s_dart_natives[0]);
  return s_dart_natives;
}
