/* SPDX-FileCopyrightText: 2026 Jonah Walker */
/* SPDX-License-Identifier: Apache-2.0 */

//! The "Counter" app: a real Flutter (dart2wasm) counter on the watch, launchable
//! from the app menu. On launch it brings up PSRAM (on-demand bulk memory), starts
//! the resident Flutter module (FLUTTER_COUNTER_WASM), which renders the digit to
//! the framebuffer via the presentFrame native. Up/Select/Down inject a tap into
//! the Flutter app (onPointerDataPacket), which increments + re-renders.

#include "applib/app.h"
#include "applib/app_logging.h"
#include "applib/app_timer.h"
#include "applib/fonts/fonts.h"
#include "applib/ui/app_window_stack.h"
#include "applib/ui/text_layer.h"
#include "applib/ui/window.h"
#include "drivers/button_id.h"
#include "kernel/pbl_malloc.h"
#include "process_management/pebble_process_md.h"
#include "process_state/app_state/app_state.h"

#include "dart/dart_runtime.h"
#if defined(CONFIG_BOARD_FAMILY_OBELIX)
#include "soc/sf32lb/sf32lb52x/psram.h"
#endif

#include <string.h>

typedef struct {
  Window *window;
  TextLayer *text_layer;
} CounterData;

//! A button press = a tap at the screen centre, injected into the Flutter app.
//! The resulting setState -> scheduleFrame -> render repaints the new count via
//! the presentFrame native.
static void prv_tap(ClickRecognizerRef recognizer, void *context) {
  dart_app_inject_tap(100.0, 114.0);
}

static void prv_click_config(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, prv_tap);
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_tap);
  window_single_click_subscribe(BUTTON_ID_DOWN, prv_tap);
}

//! Deferred (after the "Loading" frame renders) so the heavy bring-up doesn't
//! block the window push: bring up PSRAM, then start the Flutter app. On success
//! the presentFrame native has already painted the counter, so no layer draw is
//! needed; on failure the text layer reports it.
static void prv_start(void *ctx) {
  CounterData *data = app_state_get_user_data();
#if defined(CONFIG_BOARD_FAMILY_OBELIX)
  if (!sf32lb52_psram_is_ready()) {
    sf32lb52_psram_bringup(2);  // on-demand; never at boot
  }
#endif
  bool ok = dart_app_start_flutter_counter();
  if (!ok) {
    text_layer_set_text(data->text_layer, "Flutter start FAILED");
    layer_mark_dirty(window_get_root_layer(data->window));
    APP_LOG(APP_LOG_LEVEL_ERROR, "counter: dart_app_start_flutter_counter failed");
  }
}

static void prv_init(void) {
  CounterData *data = task_malloc_check(sizeof(CounterData));
  memset(data, 0, sizeof(*data));

  data->window = window_create();
  window_set_click_config_provider(data->window, prv_click_config);
  Layer *window_layer = window_get_root_layer(data->window);
  GRect bounds = window_layer->bounds;
  data->text_layer = text_layer_create((GRect) {
      .origin = { 0, 90 }, .size = { bounds.size.w, 48 } });
  text_layer_set_font(data->text_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_text_alignment(data->text_layer, GTextAlignmentCenter);
  text_layer_set_text(data->text_layer, "Loading Flutter...");
  layer_add_child(window_layer, text_layer_get_layer(data->text_layer));

  app_state_set_user_data(data);
  app_window_stack_push(data->window, true);
  app_timer_register(400, prv_start, NULL);
}

static void prv_deinit(void) {
  CounterData *data = app_state_get_user_data();
  dart_app_stop();
  text_layer_destroy(data->text_layer);
  window_destroy(data->window);
  task_free(data);
}

static void s_main(void) {
  prv_init();
  app_event_loop();
  prv_deinit();
}

const PebbleProcessMd *flutter_counter_get_app_info(void) {
  static const PebbleProcessMdSystem s_app_md = {
    .common = {
      // UUID: da770002-0000-4000-8000-000000000002
      .uuid = { 0xda, 0x77, 0x00, 0x02, 0x00, 0x00, 0x40, 0x00,
                0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02 },
      .main_func = s_main,
      .process_type = ProcessTypeApp,
      .visibility = ProcessVisibilityShown,
    },
    .name = "Counter",
  };
  return (const PebbleProcessMd *) &s_app_md;
}
