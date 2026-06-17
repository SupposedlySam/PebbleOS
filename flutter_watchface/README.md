# Flutter Watchface SDK

Build modern Pebble watchfaces using Flutter/Dart that run directly on Pebble firmware.

## Architecture

Flutter watchfaces communicate with Pebble firmware via **Dart FFI** (Foreign Function Interface):

```
Flutter Watchface (Dart)
    ↓ FFI
C Firmware Bridge
    ↓
Pebble Display/Hardware
```

See **[FFI_BRIDGE.md](FFI_BRIDGE.md)** for detailed architecture documentation.

## Quick Start

### 1. Setup and Generate FFI Bindings

```bash
# Install dependencies
cd flutter_watchface
flutter pub get

# Generate FFI bindings from C header
dart run ffigen
# Or use the script: sip run flutter-gen
```

This auto-generates `lib/src/platform/ffi_bindings.g.dart` from the C header file `../src/fw/flutter_bridge/watchface_api.h`.

**Note:** Regenerate bindings whenever you modify the C API header.

### 2. Add Dependency

```yaml
# pubspec.yaml
dependencies:
  flutter_watchface:
    path: ../flutter_watchface
```

### 2. Create a Watchface

```dart
import 'package:flutter/material.dart';
import 'package:flutter_watchface/flutter_watchface.dart';
import 'package:intl/intl.dart';

class SimpleDigitalWatchface extends StatefulWidget {
  const SimpleDigitalWatchface({super.key});

  @override
  State<SimpleDigitalWatchface> createState() => _SimpleDigitalWatchfaceState();
}

class _SimpleDigitalWatchfaceState extends State<SimpleDigitalWatchface> {
  DateTime _currentTime = DateTime.now();
  late WatchfacePlatform _platform;

  @override
  void initState() {
    super.initState();

    // Initialize platform bridge
    _platform = WatchfacePlatform.instance;

    if (!_platform.initialize()) {
      print('Failed to initialize watchface platform');
      return;
    }

    // Register for time ticks
    _platform.onTimeTick = (time) {
      setState(() {
        _currentTime = time;
      });
    };

    // Get display specs from firmware
    final specs = _platform.getDisplaySpecs();
    print('Display: ${specs.width}x${specs.height}, ${specs.dpi} DPI');
  }

  @override
  void dispose() {
    _platform.shutdown();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Container(
      color: Colors.black,
      child: Center(
        child: Text(
          DateFormat('HH:mm').format(_currentTime),
          style: const TextStyle(
            fontSize: 48,
            fontWeight: FontWeight.bold,
            color: Colors.white,
          ),
        ),
      ),
    );
  }
}

void main() {
  runApp(MaterialApp(
    home: SimpleDigitalWatchface(),
  ));
}
```

### 3. Build and Run

```bash
# Build Flutter app
flutter build bundle

# Compile with firmware
cd ..
./waf build --with-flutter

# Run in QEMU
./qemu_run.sh
```

## API Reference

### WatchfacePlatform

Main interface for communicating with Pebble firmware.

#### Methods

**`initialize()`** - Initialize the platform

```dart
final platform = WatchfacePlatform.instance;
if (platform.initialize()) {
  print('Platform ready');
}
```

**`getDisplaySpecs()`** - Get display specifications from firmware

```dart
final specs = platform.getDisplaySpecs();
print('${specs.width}x${specs.height}');
```

**`sendFrame()`** - Send rendered frame to firmware

```dart
Uint8List pixels = renderFrame();
platform.sendFrame(pixels, 144, 168, 1);
```

**`shutdown()`** - Clean up resources

```dart
platform.shutdown();
```

#### Callbacks

**`onTimeTick`** - Called when time updates

```dart
platform.onTimeTick = (DateTime time) {
  print('Time: ${time.hour}:${time.minute}');
};
```

**`onButtonEvent`** - Called on button press

```dart
platform.onButtonEvent = (ButtonEvent event) {
  print('Button ${event.button} ${event.type}');
};
```

**`onRenderRequest`** - Called when firmware requests render

```dart
platform.onRenderRequest = () {
  // Render and send frame
};
```

**`onActivated`** / **`onDeactivated`** - Lifecycle events

```dart
platform.onActivated = () => print('Watchface active');
platform.onDeactivated = () => print('Watchface inactive');
```

### DisplaySpecs

Display configuration for different Pebble models.

```dart
// Pre-defined displays
DisplaySpecs.classic      // Pebble Classic (144x168, B&W)
DisplaySpecs.time         // Pebble Time (144x168, color)
DisplaySpecs.timeRound    // Pebble Time Round (180x180, color)
DisplaySpecs.pebble2      // Pebble 2 (144x168, B&W)
DisplaySpecs.pebble2Duo   // Pebble 2 Duo (144x168, B&W, 176 DPI)
DisplaySpecs.time2        // Pebble Time 2 (200x228, color, 202 DPI)

// Custom display
DisplaySpecs(
  width: 144,
  height: 168,
  colorDepth: 1,
  type: DisplayType.monochrome,
  dpi: 176,
)
```

### TimeService

Time update service with configurable intervals.

```dart
// Update every minute (default)
TimeService().tickStream

// Update every second
TimeService(tickInterval: Duration(seconds: 1)).tickStream

// Update 10 times per second
TimeService(tickInterval: Duration(milliseconds: 100)).tickStream
```

## Project Structure

```
flutter_watchface/
├── lib/
│   ├── flutter_watchface.dart          # Main library exports
│   └── src/
│       ├── graphics/
│       │   └── display_specs.dart      # Display configurations
│       ├── platform/
│       │   ├── ffi_bindings.dart       # Low-level FFI bindings
│       │   └── watchface_platform.dart # High-level Dart API
│       ├── runtime/
│       │   └── time_service.dart       # Time update service
│       └── watchface/
│           └── pebble_watchface.dart   # Base watchface widget
├── example/
│   └── simple_digital/                 # Example watchface
└── FFI_BRIDGE.md                       # Architecture documentation
```

## C Firmware Integration

The C side of the bridge is in the main firmware repository:

```
src/fw/flutter_bridge/
├── watchface_api.h   # C API header
└── watchface_api.c   # C API implementation
```

### Implementing Firmware Integration

1. **Display Integration** - Connect `watchface_send_frame()` to compositor
2. **Tick Service** - Call `watchface_trigger_time_tick()` on timer events
3. **Button Handler** - Call `watchface_trigger_button_event()` on button press
4. **Display Specs** - Return actual hardware specs in `watchface_get_display_specs()`

See `src/fw/flutter_bridge/watchface_api.c` for implementation stubs.

## Development Workflow

### Testing Without Hardware

While firmware integration is in progress, you can test watchface logic:

```dart
// Mock the platform for testing
class TestWatchface extends StatefulWidget {
  @override
  Widget build(BuildContext context) {
    return StreamBuilder<DateTime>(
      stream: TimeService().tickStream,
      builder: (context, snapshot) {
        // Your watchface UI
      },
    );
  }
}
```

### With QEMU Emulator

```bash
# Configure a build with the Flutter bridge enabled (off by default).
# asterix is the Pebble 2 Duo board; see docs/development/options.md.
./waf configure --board=asterix -DCONFIG_FLUTTER_WATCHFACE=y
./waf build

# Run in QEMU (see docs/development/qemu.md)
```

### On Real Hardware

```bash
# Flash to a connected Pebble 2 Duo (asterix)
./waf configure --board=asterix -DCONFIG_FLUTTER_WATCHFACE=y
./waf build
./waf flash
```

## Performance

### Frame Rate

- Target: 30 FPS (33ms per frame)
- Flutter rendering: ~10-15ms
- FFI overhead: ~2-5ms
- Display refresh: ~5-10ms

### Optimization Tips

1. **Minimize FFI calls** - batch updates when possible
2. **Reuse buffers** - avoid allocations in render loop
3. **Partial updates** - only render changed regions
4. **Cache layouts** - compute once, render many times

## Examples

See `example/simple_digital/` for a complete working example.

## Building Blocks

### Digital Clock

```dart
StreamBuilder<DateTime>(
  stream: TimeService(tickInterval: Duration(seconds: 1)).tickStream,
  builder: (context, snapshot) {
    if (!snapshot.hasData) return Container();
    return Text(DateFormat('HH:mm:ss').format(snapshot.data!));
  },
)
```

### Analog Clock

```dart
CustomPaint(
  painter: ClockPainter(
    hour: time.hour,
    minute: time.minute,
    second: time.second,
  ),
)
```

### Button Interaction

```dart
@override
void initState() {
  super.initState();
  platform.onButtonEvent = (event) {
    if (event.button == 'select' && event.type == 'click') {
      setState(() {
        _showSeconds = !_showSeconds;
      });
    }
  };
}
```

## Resources

- **FFI Architecture**: [FFI_BRIDGE.md](FFI_BRIDGE.md)
- **Dart FFI Guide**: https://dart.dev/guides/libraries/c-interop
- **Flutter Engine**: https://github.com/flutter/engine
- **Pebble Firmware**: ../README.md

## Troubleshooting

### "Failed to load dynamic library"

The Flutter app can't find the C firmware library. Ensure:

1. Firmware is compiled with Flutter bridge support
2. Library path is correct in `ffi_bindings.dart`
3. Running on actual hardware or QEMU (not desktop)

### "Symbol not found"

C functions aren't exported properly. Check:

1. Functions use `extern "C"` linkage
2. Functions are not `static`
3. Library is linked correctly in waf build

### Memory issues

Check pointer management in FFI calls. See [FFI_BRIDGE.md](FFI_BRIDGE.md) for memory safety guidelines.

## Contributing

This is an experimental SDK for running Flutter watchfaces on Pebble hardware. Contributions welcome!

Areas that need work:

- [ ] Display driver integration
- [ ] Performance optimization
- [ ] More example watchfaces
- [ ] Testing infrastructure
- [ ] Documentation improvements

## License

See main repository LICENSE file.
