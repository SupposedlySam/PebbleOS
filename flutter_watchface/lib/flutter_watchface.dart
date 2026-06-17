/// Standalone Flutter framework for building Pebble watchfaces
///
/// This library provides everything you need to build modern Pebble
/// watchfaces using Flutter/Dart without touching the C firmware.
///
/// ## Getting Started
///
/// 1. Extend [PebbleWatchface] to create your watchface
/// 2. Use [PebbleDisplay] to constrain to display dimensions
/// 3. Subscribe to [TimeService] for time updates
///
/// ## Example
///
/// ```dart
/// import 'package:flutter_watchface/flutter_watchface.dart';
/// import 'package:flutter/material.dart';
/// import 'package:intl/intl.dart';
///
/// class SimpleDigitalWatchface extends PebbleWatchface {
///   const SimpleDigitalWatchface({super.key});
///
///   @override
///   Widget build(BuildContext context) {
///     return PebbleDisplay(
///       child: Container(
///         color: Colors.black,
///         child: Center(
///           child: StreamBuilder<DateTime>(
///             stream: TimeService().tickStream,
///             builder: (context, snapshot) {
///               if (!snapshot.hasData) {
///                 return const Text('--:--');
///               }
///               final time = snapshot.data!;
///               return Text(
///                 DateFormat('HH:mm').format(time),
///                 style: const TextStyle(
///                   fontSize: 48,
///                   fontWeight: FontWeight.bold,
///                   color: Colors.white,
///                 ),
///               );
///             },
///           ),
///         ),
///       ),
///     );
///   }
/// }
/// ```
library;

// Graphics
export 'src/graphics/display_specs.dart';

// Watchface
export 'src/watchface/pebble_watchface.dart';

// Runtime
export 'src/runtime/time_service.dart';

// Platform (FFI bridge to C firmware)
export 'src/platform/watchface_platform.dart'
    show WatchfacePlatform, ButtonEvent;

// Note: ffi_bindings is internal, not exported
