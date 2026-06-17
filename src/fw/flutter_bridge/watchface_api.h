/* SPDX-FileCopyrightText: 2026 Jonah Walker */
/* SPDX-License-Identifier: Apache-2.0 */

#ifndef WATCHFACE_API_H
#define WATCHFACE_API_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Data Structures
// ============================================================================

/// Display specifications for the Pebble watch
typedef struct {
    int32_t width;
    int32_t height;
    int32_t color_depth;  // 1 for monochrome, 8 for color
    int32_t dpi;          // -1 if not available
    bool is_color;        // true for color displays
} WatchfaceDisplaySpecs;

/// Frame data containing rendered pixels from Flutter
typedef struct {
    uint8_t* pixels;
    size_t pixels_length;
    int32_t width;
    int32_t height;
    int32_t bytes_per_pixel;
} WatchfaceFrameData;

/// Time data for tick events
typedef struct {
    int64_t timestamp_ms;  // Unix timestamp in milliseconds
} WatchfaceTimeData;

/// Button event data
typedef struct {
    uint8_t button_id;     // 0=up, 1=select, 2=down, 3=back
    uint8_t event_type;    // 0=click, 1=long_click, 2=hold, 3=release
} WatchfaceButtonEvent;

// ============================================================================
// Host API: Flutter calls these C functions
// ============================================================================

/// Get the display specifications from the firmware
/// Returns: Display specs structure
WatchfaceDisplaySpecs watchface_get_display_specs(void);

/// Send a rendered frame from Flutter to the firmware
/// @param frame: Pointer to frame data
/// @return: 0 on success, negative on error
int32_t watchface_send_frame(const WatchfaceFrameData* frame);

/// Initialize the Flutter watchface runtime
/// @return: 0 on success, negative on error
int32_t watchface_initialize(void);

/// Shutdown the Flutter watchface runtime
void watchface_shutdown(void);

// ============================================================================
// Flutter API: C firmware calls these (via function pointers)
// ============================================================================

/// Callback function types that Flutter will implement
typedef void (*watchface_time_tick_callback)(WatchfaceTimeData* time);
typedef void (*watchface_button_event_callback)(WatchfaceButtonEvent* event);
typedef void (*watchface_render_request_callback)(void);
typedef void (*watchface_activated_callback)(void);
typedef void (*watchface_deactivated_callback)(void);

/// Register Flutter callbacks with the firmware
void watchface_register_callbacks(
    watchface_time_tick_callback on_time_tick,
    watchface_button_event_callback on_button_event,
    watchface_render_request_callback on_render_request,
    watchface_activated_callback on_activated,
    watchface_deactivated_callback on_deactivated
);

// ============================================================================
// Firmware helpers (called by firmware, not Flutter)
// ============================================================================

/// Trigger a time tick event (called by firmware tick service)
void watchface_trigger_time_tick(int64_t timestamp_ms);

/// Trigger a button event (called by firmware button handler)
void watchface_trigger_button_event(uint8_t button_id, uint8_t event_type);

/// Request a render from the Flutter watchface
void watchface_trigger_render_request(void);

#ifdef __cplusplus
}
#endif

#endif // WATCHFACE_API_H

