/* SPDX-FileCopyrightText: 2026 Jonah Walker */
/* SPDX-License-Identifier: Apache-2.0 */

#include "watchface_api.h"
#include <string.h>

// ============================================================================
// Callback storage
// ============================================================================

static watchface_time_tick_callback g_on_time_tick = NULL;
static watchface_button_event_callback g_on_button_event = NULL;
static watchface_render_request_callback g_on_render_request = NULL;
static watchface_activated_callback g_on_activated = NULL;
static watchface_deactivated_callback g_on_deactivated = NULL;

// ============================================================================
// Host API Implementation (Flutter → Firmware)
// ============================================================================

WatchfaceDisplaySpecs watchface_get_display_specs(void) {
    WatchfaceDisplaySpecs specs;
    
    // TODO: Get actual display specs from firmware
    // For now, return Pebble Time specs as default
    specs.width = 144;
    specs.height = 168;
    specs.color_depth = 8;
    specs.dpi = 175;
    specs.is_color = true;
    
    return specs;
}

int32_t watchface_send_frame(const WatchfaceFrameData* frame) {
    if (frame == NULL || frame->pixels == NULL) {
        return -1;
    }
    
    // TODO: Copy frame data to display framebuffer
    // This will integrate with the compositor/display driver
    
    // Example stub:
    // compositor_set_framebuffer(frame->pixels, frame->width, frame->height);
    // display_refresh();
    
    return 0;
}

int32_t watchface_initialize(void) {
    // TODO: Initialize Flutter engine
    // TODO: Load watchface resources
    // TODO: Set up display context
    
    return 0;
}

void watchface_shutdown(void) {
    // TODO: Cleanup Flutter engine
    // TODO: Free resources
    
    // Clear callbacks
    g_on_time_tick = NULL;
    g_on_button_event = NULL;
    g_on_render_request = NULL;
    g_on_activated = NULL;
    g_on_deactivated = NULL;
}

// ============================================================================
// Flutter API Registration (Flutter → Firmware)
// ============================================================================

void watchface_register_callbacks(
    watchface_time_tick_callback on_time_tick,
    watchface_button_event_callback on_button_event,
    watchface_render_request_callback on_render_request,
    watchface_activated_callback on_activated,
    watchface_deactivated_callback on_deactivated
) {
    g_on_time_tick = on_time_tick;
    g_on_button_event = on_button_event;
    g_on_render_request = on_render_request;
    g_on_activated = on_activated;
    g_on_deactivated = on_deactivated;
}

// ============================================================================
// Firmware Helpers (Firmware → Flutter)
// ============================================================================

void watchface_trigger_time_tick(int64_t timestamp_ms) {
    if (g_on_time_tick != NULL) {
        WatchfaceTimeData time;
        time.timestamp_ms = timestamp_ms;
        g_on_time_tick(&time);
    }
}

void watchface_trigger_button_event(uint8_t button_id, uint8_t event_type) {
    if (g_on_button_event != NULL) {
        WatchfaceButtonEvent event;
        event.button_id = button_id;
        event.event_type = event_type;
        g_on_button_event(&event);
    }
}

void watchface_trigger_render_request(void) {
    if (g_on_render_request != NULL) {
        g_on_render_request();
    }
}

