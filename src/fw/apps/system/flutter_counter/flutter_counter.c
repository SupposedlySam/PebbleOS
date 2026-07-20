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
#include "applib/touch_service.h"
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

//! Flutter's frame model, decoupled from input like a real vsync. A button
//! press ONLY dispatches (Listener -> setState -> count++ -> scheduleFrame),
//! which is ~ms and never renders -- so any mash rate just keeps bumping the
//! count with no queue backlog and no event-loop overflow. A steady ~30fps
//! timer pumps the event loop: it renders exactly ONE frame if presses
//! scheduled one since the last tick (showing the net count -- 3 -> mash ->
//! 13), or does nothing when idle. Renders can never outnumber frame ticks, so
//! the display always converges to the current count instead of replaying a
//! render per press.
#define COUNTER_FRAME_MS 33
static AppTimer *s_frame_timer;

static void prv_frame_tick(void *context) {
  dart_app_pump(); /* renders iff a frame was scheduled; ~0 when idle */
  s_frame_timer = app_timer_register(COUNTER_FRAME_MS, prv_frame_tick, NULL);
}

// USB-HID physical + Flutter logical key ids for Enter, which Flutter's default
// shortcuts map to ActivateIntent -> the focused widget's onPressed. Delivering
// this on an Up press activates the focused FAB, using Flutter's own D-pad/remote
// machinery -- no coordinate, works for any focusable widget.
#define KEY_ENTER_PHYSICAL 0x00070028LL
#define KEY_ENTER_LOGICAL  0x0010000000dLL

static void prv_tap(ClickRecognizerRef recognizer, void *context) {
  /* Up = "activate the focused widget". count++ now; the frame tick renders. */
  dart_app_inject_key(KEY_ENTER_PHYSICAL, KEY_ENTER_LOGICAL);
}

//! Real touchscreen input: forward each touchdown to Flutter at the actual
//! panel coordinate, so hit-testing works (tap the FAB, not just anywhere).
//! Rendering stays with the frame tick, same as button input.
static void prv_touch(const TouchEvent *event, void *context) {
  APP_LOG(APP_LOG_LEVEL_INFO, "counter: touch type=%d (%d,%d)",
          (int)event->type, (int)event->x, (int)event->y);
  if (event->type == TouchEvent_Touchdown) {
    dart_app_dispatch_only((double)event->x, (double)event->y);
  }
}

static void prv_click_config(void *context) {
  // Only UP increments the counter (taps the FAB); SELECT/DOWN are intentionally
  // left unbound. BACK stays the app's exit, handled by the window framework.
  window_single_click_subscribe(BUTTON_ID_UP, prv_tap);
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
  // Supersede any power-down the previous exit queued (atomic vs. the KernelBG
  // callback), BEFORE trusting is_ready() -- otherwise a fast reopen could skip
  // bring-up on stale is_ready and then have PSRAM cut out from under it.
  sf32lb52_psram_cancel_powerdown();
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
  bool dart_ok = dart_app_start_flutter_counter();
  if (!dart_ok) {
    prv_fail(data, "dart_app_start_flutter_counter failed");
    return;
  }
  touch_service_subscribe(prv_touch, NULL);
  s_frame_timer = app_timer_register(COUNTER_FRAME_MS, prv_frame_tick, NULL);
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

#if defined(CONFIG_BOARD_FAMILY_OBELIX)
//! Runs on KernelBG (privileged, like bring-up): powers the whole PSRAM stack
//! down (die DPD + 288MHz PLL + controller + pad park) so an idle watch isn't
//! paying its always-on cost -- it halved multi-day battery life. Safe ONLY
//! after the Dart runtime is fully torn down (its pool lives in PSRAM); the
//! next launch pays the full bring-up + module parse again (~8s), the right
//! trade on a wrist.
static void prv_psram_powerdown_cb(void *unused) {
  // if_armed: a no-op when the next app enter already cancelled it, so PSRAM is
  // never cut out from under a fast reopen (atomic vs. cancel_powerdown).
  sf32lb52_psram_powerdown_if_armed();
}
#endif

static void prv_deinit(void) {
  CounterData *data = app_state_get_user_data();
  touch_service_unsubscribe();
  if (s_frame_timer) {
    app_timer_cancel(s_frame_timer);
    s_frame_timer = NULL;
  }
  // Fully tear down the Dart runtime -- NOT just the instance (dart_app_stop) --
  // because we are about to power PSRAM down and the runtime's pool lives there.
  // Leaving the runtime initialized over dead PSRAM is exactly what raced the
  // next launch into a silent reset when on-exit power-down was tried before
  // (that path used dart_app_stop, so s_initialized stayed set and the next
  // dart_runtime_init reused a stale, powered-down pool). dart_runtime_teardown
  // clears that state; this is the console `psram off` order (teardown FIRST).
  dart_runtime_teardown();
#if defined(CONFIG_BOARD_FAMILY_OBELIX)
  // Power PSRAM down for battery whenever the app exits (incl. the Back button).
  // ARM it, then run the actual power-down on KernelBG (privileged, like bring-up).
  // We deliberately do NOT wait: prv_deinit runs inside the ~3s graceful-close
  // deadline, and blocking here risks a force-kill/croak. The race a wait would
  // guard is instead closed by making the power-down cancelable -- the next enter
  // calls sf32lb52_psram_cancel_powerdown() so a fast reopen supersedes a pending
  // power-down atomically (see prv_start). Bring-up on the next enter is prv_start's.
  sf32lb52_psram_arm_powerdown();
  if (!system_task_add_callback(prv_psram_powerdown_cb, NULL)) {
    // KernelBG queue full: PSRAM stays up one extra cycle (battery only). It
    // self-heals -- the next enter cancels the stale arm, or the next exit
    // re-queues and powers down then. No crash. Log so the leak is visible.
    APP_LOG(APP_LOG_LEVEL_WARNING, "counter: PSRAM power-down not queued (KernelBG full)");
  }
#endif
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
