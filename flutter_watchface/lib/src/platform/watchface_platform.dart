import 'dart:ffi' as ffi;
import 'dart:typed_data';
import 'package:ffi/ffi.dart';

import 'ffi_bindings.g.dart';
import '../graphics/display_specs.dart';

// ============================================================================
// High-level Dart API for Watchface Platform
// ============================================================================

/// Platform bridge for Flutter watchfaces running on Pebble firmware
class WatchfacePlatform {
  static WatchfacePlatform? _instance;
  final WatchfaceNativeBindings _bindings;

  // Callback storage to prevent garbage collection
  ffi.Pointer<ffi.NativeFunction<watchface_time_tick_callbackFunction>>?
  _timeTickPtr;
  ffi.Pointer<ffi.NativeFunction<watchface_button_event_callbackFunction>>?
  _buttonEventPtr;
  ffi.Pointer<ffi.NativeFunction<watchface_render_request_callbackFunction>>?
  _renderRequestPtr;
  ffi.Pointer<ffi.NativeFunction<watchface_activated_callbackFunction>>?
  _activatedPtr;
  ffi.Pointer<ffi.NativeFunction<watchface_deactivated_callbackFunction>>?
  _deactivatedPtr;

  // User callbacks
  Function(DateTime)? onTimeTick;
  Function(ButtonEvent)? onButtonEvent;
  Function()? onRenderRequest;
  Function()? onActivated;
  Function()? onDeactivated;

  WatchfacePlatform._(this._bindings);

  /// Get the singleton instance
  static WatchfacePlatform get instance {
    _instance ??= WatchfacePlatform._(_loadBindings());
    return _instance!;
  }

  /// Load the native library bindings
  static WatchfaceNativeBindings _loadBindings() {
    try {
      // Try Linux/QEMU
      return WatchfaceNativeBindings(
        ffi.DynamicLibrary.open('libpebble_watchface.so'),
      );
    } catch (e) {
      try {
        // Try macOS
        return WatchfaceNativeBindings(
          ffi.DynamicLibrary.open('libpebble_watchface.dylib'),
        );
      } catch (e) {
        // Try embedded symbols (if compiled into executable)
        return WatchfaceNativeBindings(ffi.DynamicLibrary.process());
      }
    }
  }

  /// Initialize the watchface platform
  bool initialize() {
    final result = _bindings.watchface_initialize();
    if (result == 0) {
      _registerNativeCallbacks();
      return true;
    }
    return false;
  }

  /// Shutdown the watchface platform
  void shutdown() {
    _bindings.watchface_shutdown();

    // Free callback pointers
    if (_timeTickPtr != null) calloc.free(_timeTickPtr!);
    if (_buttonEventPtr != null) calloc.free(_buttonEventPtr!);
    if (_renderRequestPtr != null) calloc.free(_renderRequestPtr!);
    if (_activatedPtr != null) calloc.free(_activatedPtr!);
    if (_deactivatedPtr != null) calloc.free(_deactivatedPtr!);
  }

  /// Get display specifications from firmware
  DisplaySpecs getDisplaySpecs() {
    final nativeSpecs = _bindings.watchface_get_display_specs();

    return DisplaySpecs(
      width: nativeSpecs.width,
      height: nativeSpecs.height,
      colorDepth: nativeSpecs.color_depth,
      type: nativeSpecs.is_color ? DisplayType.color : DisplayType.monochrome,
      dpi: nativeSpecs.dpi >= 0 ? nativeSpecs.dpi : null,
    );
  }

  /// Send a rendered frame to the firmware
  bool sendFrame(Uint8List pixels, int width, int height, int bytesPerPixel) {
    // Allocate native frame data
    final framePtr = calloc<WatchfaceFrameData>();
    final pixelsPtr = calloc<ffi.Uint8>(pixels.length);

    try {
      // Copy pixel data to native memory
      final pixelsList = pixelsPtr.asTypedList(pixels.length);
      pixelsList.setAll(0, pixels);

      // Fill frame structure
      framePtr.ref.pixels = pixelsPtr;
      framePtr.ref.pixels_length = pixels.length;
      framePtr.ref.width = width;
      framePtr.ref.height = height;
      framePtr.ref.bytes_per_pixel = bytesPerPixel;

      // Send to firmware
      final result = _bindings.watchface_send_frame(framePtr);

      return result == 0;
    } finally {
      // Cleanup
      calloc.free(pixelsPtr);
      calloc.free(framePtr);
    }
  }

  /// Register Dart callbacks with native code
  void _registerNativeCallbacks() {
    // Create native callback pointers
    _timeTickPtr =
        ffi.Pointer.fromFunction<watchface_time_tick_callbackFunction>(
          _onTimeTickNative,
        );
    _buttonEventPtr =
        ffi.Pointer.fromFunction<watchface_button_event_callbackFunction>(
          _onButtonEventNative,
        );
    _renderRequestPtr =
        ffi.Pointer.fromFunction<watchface_render_request_callbackFunction>(
          _onRenderRequestNative,
        );
    _activatedPtr =
        ffi.Pointer.fromFunction<watchface_activated_callbackFunction>(
          _onActivatedNative,
        );
    _deactivatedPtr =
        ffi.Pointer.fromFunction<watchface_deactivated_callbackFunction>(
          _onDeactivatedNative,
        );

    // Register with native code
    _bindings.watchface_register_callbacks(
      _timeTickPtr!,
      _buttonEventPtr!,
      _renderRequestPtr!,
      _activatedPtr!,
      _deactivatedPtr!,
    );
  }

  // Native callback implementations
  static void _onTimeTickNative(ffi.Pointer<WatchfaceTimeData> time) {
    final instance = WatchfacePlatform._instance;
    if (instance?.onTimeTick != null) {
      final dateTime = DateTime.fromMillisecondsSinceEpoch(
        time.ref.timestamp_ms,
      );
      instance!.onTimeTick!(dateTime);
    }
  }

  static void _onButtonEventNative(ffi.Pointer<WatchfaceButtonEvent> event) {
    final instance = WatchfacePlatform._instance;
    if (instance?.onButtonEvent != null) {
      final buttonEvent = ButtonEvent(
        button: _buttonIdToString(event.ref.button_id),
        type: _eventTypeToString(event.ref.event_type),
      );
      instance!.onButtonEvent!(buttonEvent);
    }
  }

  static void _onRenderRequestNative() {
    final instance = WatchfacePlatform._instance;
    instance?.onRenderRequest?.call();
  }

  static void _onActivatedNative() {
    final instance = WatchfacePlatform._instance;
    instance?.onActivated?.call();
  }

  static void _onDeactivatedNative() {
    final instance = WatchfacePlatform._instance;
    instance?.onDeactivated?.call();
  }

  // Helper conversions
  static String _buttonIdToString(int id) {
    switch (id) {
      case 0:
        return 'up';
      case 1:
        return 'select';
      case 2:
        return 'down';
      case 3:
        return 'back';
      default:
        return 'unknown';
    }
  }

  static String _eventTypeToString(int type) {
    switch (type) {
      case 0:
        return 'click';
      case 1:
        return 'long_click';
      case 2:
        return 'hold';
      case 3:
        return 'release';
      default:
        return 'unknown';
    }
  }
}

// ============================================================================
// Data Classes
// ============================================================================

/// Button event data
class ButtonEvent {
  final String button; // 'up', 'select', 'down', 'back'
  final String type; // 'click', 'long_click', 'hold', 'release'

  ButtonEvent({required this.button, required this.type});

  @override
  String toString() => 'ButtonEvent($button, $type)';
}
