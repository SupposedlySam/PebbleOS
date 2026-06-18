/* SPDX-FileCopyrightText: 2026 Jonah Walker */
/* SPDX-License-Identifier: Apache-2.0 */

//! On-screen Dart/wasm smoke-test diagnostic. Launched from the app menu, it
//! runs the WAMR smoke test one stage at a time, showing "step N" on the screen
//! BEFORE executing it. If a stage faults on hardware, the screen freezes on the
//! dead stage, naming the culprit with zero reliance on logs/relay. Each step is
//! also APP_LOG'd (streams live to `pebble logs` when that channel is up).

#include "applib/app.h"
#include "applib/app_logging.h"
#include "applib/app_timer.h"
#include "applib/fonts/fonts.h"
#include "applib/ui/app_window_stack.h"
#include "applib/ui/text_layer.h"
#include "kernel/pbl_malloc.h"
#include "process_management/pebble_process_md.h"
#include "process_state/app_state/app_state.h"

#include "dart/dart_runtime.h"

#include <stdio.h>
#include <string.h>

#define STATUS_LEN 96
#define NUM_STEPS 5

static const char *const s_step_labels[] = {
  "",                  // 0 unused
  "1 init",
  "2 load",
  "3 instantiate",
  "4 exec_env",
  "5 CALL add(40,2)",  // the call trampoline — prime hardware-fault suspect
};

typedef struct {
  Window *window;
  TextLayer *text_layer;
  char status[STATUS_LEN];
  int step;
} DartDiagData;

static void prv_set_status(DartDiagData *data, const char *msg) {
  strncpy(data->status, msg, STATUS_LEN - 1);
  data->status[STATUS_LEN - 1] = '\0';
  text_layer_set_text(data->text_layer, data->status);
  layer_mark_dirty(window_get_root_layer(data->window));
}

static void prv_execute(void *ctx);

//! Phase 1: show "step N" + APP_LOG it, then schedule execution after a render.
static void prv_display(void *ctx) {
  DartDiagData *data = app_state_get_user_data();
  char line[STATUS_LEN];
  snprintf(line, sizeof(line), "Running:\n%s ...", s_step_labels[data->step]);
  prv_set_status(data, line);
  APP_LOG(APP_LOG_LEVEL_INFO, "dart diag: %s", s_step_labels[data->step]);
  app_timer_register(500, prv_execute, NULL);
}

//! Phase 2: execute the step. A fault here leaves the step's name on screen.
static void prv_execute(void *ctx) {
  DartDiagData *data = app_state_get_user_data();
  char line[STATUS_LEN];
  int result = 0;
  int r = dart_smoketest_run_step(data->step, &result);
  if (r < 0) {
    snprintf(line, sizeof(line), "FAILED at:\n%s", s_step_labels[data->step]);
    prv_set_status(data, line);
    APP_LOG(APP_LOG_LEVEL_ERROR, "dart diag: FAILED at %s", s_step_labels[data->step]);
    return;
  }
  if (r == 0) {
    snprintf(line, sizeof(line), "RESULT: %d\n%s", result, result == 42 ? "OK :)" : "WRONG");
    prv_set_status(data, line);
    APP_LOG(APP_LOG_LEVEL_INFO, "dart diag: result=%d %s", result,
            result == 42 ? "OK" : "WRONG");
    return;
  }
  data->step++;
  app_timer_register(300, prv_display, NULL);
}

static void prv_init(void) {
  DartDiagData *data = task_malloc_check(sizeof(DartDiagData));
  memset(data, 0, sizeof(*data));

  data->window = window_create();
  Layer *window_layer = window_get_root_layer(data->window);
  GRect bounds = window_layer->bounds;

  data->text_layer = text_layer_create((GRect) {
      .origin = { 0, 30 }, .size = { bounds.size.w, bounds.size.h - 30 } });
  text_layer_set_font(data->text_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text_alignment(data->text_layer, GTextAlignmentCenter);
  layer_add_child(window_layer, text_layer_get_layer(data->text_layer));

  app_state_set_user_data(data);
  prv_set_status(data, "Dart smoke test\nstarting...");
  app_window_stack_push(data->window, true);

  data->step = 1;
  app_timer_register(700, prv_display, NULL);
}

static void prv_deinit(void) {
  DartDiagData *data = app_state_get_user_data();
  text_layer_destroy(data->text_layer);
  window_destroy(data->window);
  task_free(data);
}

static void s_main(void) {
  prv_init();
  app_event_loop();
  prv_deinit();
}

const PebbleProcessMd* dart_diag_get_app_info(void) {
  static const PebbleProcessMdSystem s_app_md = {
    .common = {
      // UUID: da770001-0000-4000-8000-000000000001
      .uuid = { 0xda, 0x77, 0x00, 0x01, 0x00, 0x00, 0x40, 0x00,
                0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 },
      .main_func = s_main,
      .process_type = ProcessTypeApp,
      .visibility = ProcessVisibilityShown,
    },
    .name = "Dart Diag",
  };
  return (const PebbleProcessMd*) &s_app_md;
}
