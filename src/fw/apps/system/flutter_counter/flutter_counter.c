/* SPDX-FileCopyrightText: 2026 Jonah Walker */
/* SPDX-License-Identifier: Apache-2.0 */

//! The "Counter" app: a real Flutter (dart2wasm) counter on the watch, launchable from
//! the app menu. On launch it brings up PSRAM (on-demand bulk memory) on KernelBG, starts
//! the resident Flutter module (FLUTTER_COUNTER_WASM), and the module renders the digit
//! straight into THIS app's framebuffer via the presentFrame native -- so the compositor
//! composites it to the panel the same way it does for every app. Up/Select/Down inject a
//! tap into the Flutter app (onPointerDataPacket), which increments + re-renders.

#include "applib/app.h"
#include "applib/app_logging.h"
#include "applib/app_timer.h"
#include "applib/graphics/gcolor_definitions.h"
#include "applib/graphics/graphics.h"
#include "applib/ui/app_window_stack.h"
#include "applib/ui/layer.h"
#include "applib/ui/window.h"
#include "drivers/button_id.h"
#include "kernel/pbl_malloc.h"
#include "kernel/util/sleep.h"
#include "pbl/services/system_task.h"
#include "process_management/pebble_process_md.h"
#include "process_state/app_state/app_state.h"

#include "dart/dart_embedder.h"
#include "dart/dart_runtime.h"
#if defined(CONFIG_BOARD_FAMILY_OBELIX)
#include "soc/sf32lb/sf32lb52x/psram.h"
#endif

#include <string.h>

typedef struct {
  Window *window;
  bool failed;  //!< PSRAM bring-up or Flutter start failed; the root layer shows it red.
} CounterData;

//! Root layer renderer. Until Flutter paints its first frame, this app owns the screen:
//! white while loading, red if start failed. Once presentFrame has written the app
//! framebuffer (dart_embedder_frame_presented()), leave it untouched so the Flutter frame
//! shows -- layer_render_tree only calls update_procs, so a no-op here preserves it.
static void prv_root_update(Layer *layer, GContext *ctx) {
  if (dart_embedder_frame_presented()) {
    return;
  }
  CounterData *data = app_state_get_user_data();
  graphics_context_set_fill_color(ctx, (data && data->failed) ? GColorRed : GColorWhite);
  GRect bounds = layer_get_bounds_by_value(layer);
  graphics_fill_rect(ctx, &bounds);
}

//! A button press = a tap at the screen centre, injected into the Flutter app. The
//! resulting setState -> scheduleFrame -> render repaints the new count via presentFrame.
static void prv_tap(ClickRecognizerRef recognizer, void *context) {
  dart_app_inject_tap(100.0, 114.0);
}

static void prv_click_config(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, prv_tap);
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_tap);
  window_single_click_subscribe(BUTTON_ID_DOWN, prv_tap);
}

#if defined(CONFIG_BOARD_FAMILY_OBELIX)
//! Runs on KernelBG (the system task). PSRAM bring-up touches privileged controller
//! registers; on the unprivileged app task it runs but never sets is_ready, so we
//! dispatch it here -- the same privileged context the console 'psram' command uses.
static void prv_psram_bringup_cb(void *unused) {
  if (!sf32lb52_psram_is_ready()) {
    sf32lb52_psram_bringup(2);  // on-demand; never at boot
  }
}
#endif

static void prv_fail(CounterData *data, const char *why) {
  data->failed = true;
  layer_mark_dirty(window_get_root_layer(data->window));
  APP_LOG(APP_LOG_LEVEL_ERROR, "counter: %s", why);
}

//! Deferred (after the loading frame renders) so the heavy bring-up doesn't block the
//! window push: bring up PSRAM on KernelBG + wait for it, then start the Flutter app.
//! dart_app_start stays on the app task because presentFrame writes THIS app's framebuffer
//! (app_state). On success presentFrame paints the counter; on failure the screen goes red.
static void prv_start(void *ctx) {
  CounterData *data = app_state_get_user_data();
#if defined(CONFIG_BOARD_FAMILY_OBELIX)
  if (!sf32lb52_psram_is_ready()) {
    if (!system_task_add_callback(prv_psram_bringup_cb, NULL)) {
      prv_fail(data, "could not queue PSRAM bring-up");
      return;
    }
    for (int i = 0; i < 250 && !sf32lb52_psram_is_ready(); i++) {
      psleep(20);  // wait up to ~5s for KernelBG to finish the bring-up
    }
    if (!sf32lb52_psram_is_ready()) {
      prv_fail(data, "PSRAM bring-up timed out");
      return;
    }
  }
#endif
  if (!dart_app_start_flutter_counter()) {
    prv_fail(data, "dart_app_start_flutter_counter failed");
  }
}

static void prv_init(void) {
  CounterData *data = task_malloc_check(sizeof(CounterData));
  memset(data, 0, sizeof(*data));

  // Clear any stale "frame presented" flag from a prior run/console use up front, so the
  // loading (and any failure) screen is guaranteed to show until THIS run paints a frame
  // -- don't rely solely on dart_app_stop's reset (it wouldn't run on a PSRAM-fail path).
  dart_embedder_reset_frame();

  data->window = window_create();
  window_set_click_config_provider(data->window, prv_click_config);
  // Own the root layer's rendering: draw the loading/failed screen until Flutter's first
  // frame, then yield the framebuffer to presentFrame (which writes it directly).
  layer_set_update_proc(window_get_root_layer(data->window), prv_root_update);

  app_state_set_user_data(data);
  app_window_stack_push(data->window, true);
  app_timer_register(400, prv_start, NULL);
}

static void prv_deinit(void) {
  CounterData *data = app_state_get_user_data();
  dart_app_stop();
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
