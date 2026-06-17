# Modular Flutter Architecture - Isolated & Optional

## Philosophy

**Flutter watchfaces should be a completely separate module that doesn't interfere with the existing firmware.**

- ✅ Existing C/RockyJS watchfaces work unchanged
- ✅ Flutter watchfaces are opt-in
- ✅ Both systems can coexist
- ✅ Flutter developers don't need to touch C firmware
- ✅ Firmware developers don't need to know Flutter

## Architecture: Parallel Systems

```
┌────────────────────────────────────────────────────────┐
│                  Pebble Watch Hardware                  │
└───────────────────┬────────────────────────────────────┘
                    │
        ┌───────────┴───────────┐
        │                       │
┌───────▼────────┐    ┌────────▼──────────┐
│  Classic Path  │    │   Flutter Path    │
│  (Existing)    │    │   (New/Optional)  │
└───────┬────────┘    └────────┬──────────┘
        │                       │
        │                       │
   ┌────▼─────┐          ┌─────▼──────┐
   │ C/Rocky  │          │  Flutter   │
   │Watchfaces│          │ Watchfaces │
   └────┬─────┘          └─────┬──────┘
        │                      │
   ┌────▼─────┐          ┌─────▼──────┐
   │  C Gfx   │          │  Flutter   │
   │  Engine  │          │  Engine    │
   └────┬─────┘          └─────┬──────┘
        │                      │
        └──────────┬───────────┘
                   │
            ┌──────▼──────┐
            │  Compositor │
            │   (Shared)  │
            └──────┬──────┘
                   │
            ┌──────▼──────┐
            │   Display   │
            └─────────────┘
```

## Project Structure

```
pebble-flutter/
├── src/fw/                          # EXISTING FIRMWARE (unchanged)
│   ├── applib/
│   │   ├── graphics/               # C graphics (keep as-is)
│   │   └── rockyjs/                # RockyJS (keep as-is)
│   ├── services/
│   └── drivers/
│
├── flutter_watchface/               # NEW: Isolated Flutter module
│   ├── lib/
│   │   ├── src/
│   │   │   ├── watchface_base.dart
│   │   │   ├── pebble_canvas.dart
│   │   │   └── platform_bridge.dart
│   │   └── pebble_flutter.dart
│   ├── example/
│   │   └── simple_digital/         # Example Flutter watchface
│   ├── pigeons/
│   │   └── pigeons.dart            # Flutter ↔ C bridge
│   ├── pubspec.yaml
│   └── README.md
│
├── watchfaces/                      # NEW: Watchface projects
│   ├── flutter/                    # Flutter watchfaces
│   │   ├── analog_classic/
│   │   ├── digital_modern/
│   │   └── weather_face/
│   ├── c/                          # Traditional C watchfaces
│   └── rockyjs/                    # RockyJS watchfaces
│
└── tools/
    └── flutter_build/              # NEW: Build tools for Flutter watchfaces
        ├── compile_flutter_watchface.sh
        └── package_for_pebble.sh
```

## How It Works

### 1. Firmware Stays Unchanged

The existing firmware continues to work exactly as before:
- C watchfaces use C graphics
- RockyJS watchfaces use JerryScript
- No changes to existing code paths

### 2. Flutter Module is Standalone

```dart
// flutter_watchface/lib/pebble_flutter.dart

/// Standalone Flutter framework for Pebble watchfaces
///
/// Use this package to build watchfaces in Flutter without
/// touching the C firmware code.

library pebble_flutter;

export 'src/watchface_base.dart';
export 'src/pebble_canvas.dart';
export 'src/display_specs.dart';
export 'src/time_service.dart';
```

### 3. Clean Communication Interface

**Only one file bridges Flutter → Firmware:**

```c
// src/fw/flutter_bridge/flutter_watchface_interface.h

/**
 * Minimal interface for Flutter watchfaces
 * This is the ONLY connection between Flutter and firmware
 */

typedef struct {
    uint8_t *pixels;
    uint16_t width;
    uint16_t height;
    uint8_t bytes_per_pixel;
} FlutterFrameBuffer;

// Called by Flutter to send rendered frame
void flutter_watchface_submit_frame(FlutterFrameBuffer *frame);

// Called by firmware to notify Flutter of events
void flutter_watchface_time_tick(time_t time);
void flutter_watchface_button_event(uint8_t button_id, uint8_t event_type);
```

## Developer Workflows

### Classic Developer (C/RockyJS)
```bash
# Nothing changes for them
cd pebble-flutter
./waf configure --board=v2_0
./waf build
```

**They never interact with Flutter.**

### Flutter Developer
```bash
# Work entirely in Flutter module
cd flutter_watchface

# Develop watchface
flutter run              # Test in simulator
flutter build           # Build for watch

# Package for Pebble
../tools/flutter_build/package_for_pebble.sh my_watchface
```

**They never touch the C firmware.**

## Build System: Completely Separate

### Classic Build (Existing)
```bash
./waf configure --board=v2_0
./waf build
# Produces: build/tintin.elf (firmware)
```

### Flutter Build (New)
```bash
cd flutter_watchface
flutter build bundle
# Produces: watchface.pbw (Flutter watchface package)
```

### Combined (Optional)
```bash
# If you want to embed Flutter engine in firmware
./waf configure --board=v2_0 --with-flutter
./waf build
```

## Watchface Installation

### Classic Watchface (C/RockyJS)
```
App Store → Download → Install → Run on firmware
```

### Flutter Watchface
```
App Store → Download → Flutter Runtime detects it → Renders via Flutter
```

**OR** for a simpler approach:

### Flutter Watchface as Separate App

Treat the Flutter runtime as a special app:

```
1. Install "Flutter Runtime" app (once)
2. Install Flutter watchfaces (they require Flutter Runtime)
3. Flutter Runtime handles all Flutter watchface rendering
```

Like how web apps need a browser!

## Implementation Strategy

### Phase 1: Standalone Development

**Goal:** Build Flutter watchfaces without touching firmware

```
Flutter Watchface
    ↓
Runs in Flutter Desktop/Web
    ↓
Simulates Pebble Display (144x168)
```

**What you build:**
1. `flutter_watchface/` package
2. Example watchfaces
3. Desktop simulator

**No firmware changes needed!**

### Phase 2: Runtime Integration

**Goal:** Run Flutter watchfaces on actual Pebble

```
Flutter Watchface (.pbw)
    ↓
Flutter Runtime App (on watch)
    ↓
Pebble Display
```

**What you build:**
1. Flutter runtime as a Pebble app
2. Watchface loader
3. Frame bridge to display

**Minimal firmware changes** (just the interface above)

### Phase 3: Native Integration (Optional)

**Goal:** Embed Flutter engine directly in firmware

```
Flutter Watchface
    ↓
Embedded Flutter Engine (in firmware)
    ↓
Direct Display Access
```

**More complex, but better performance**

## Example: Creating a Flutter Watchface

### As a Flutter Developer

```bash
# 1. Create new watchface project
cd watchfaces/flutter
flutter create my_watchface --template=package
cd my_watchface

# 2. Add pebble_flutter dependency
# pubspec.yaml
dependencies:
  pebble_flutter:
    path: ../../flutter_watchface

# 3. Write your watchface
# lib/my_watchface.dart
import 'package:pebble_flutter/pebble_flutter.dart';

class MyWatchface extends PebbleWatchface {
  @override
  Widget build(BuildContext context) {
    return PebbleDisplay(
      child: StreamBuilder<DateTime>(
        stream: TimeService.tickStream,
        builder: (context, snapshot) {
          if (!snapshot.hasData) return Container();
          return Center(
            child: Text(
              DateFormat('HH:mm').format(snapshot.data!),
              style: TextStyle(fontSize: 48, color: Colors.white),
            ),
          );
        },
      ),
    );
  }
}

# 4. Test in simulator
flutter run -d macos

# 5. Package for Pebble
../../tools/flutter_build/package_for_pebble.sh
# Creates: my_watchface.pbw
```

**You never needed to:**
- Build C firmware
- Install ARM toolchain
- Touch waf build system
- Know anything about Pebble internals

## Benefits of This Approach

### For You (Flutter Developer)
✅ Work in pure Flutter/Dart
✅ Use Flutter's hot reload
✅ Test on desktop/web first
✅ Don't need to understand C firmware
✅ Fast iteration

### For Classic Developers
✅ Their workflow unchanged
✅ No risk of breaking existing code
✅ Can ignore Flutter completely
✅ Firmware stays simple

### For End Users
✅ Can use classic watchfaces OR Flutter watchfaces
✅ Choice of ecosystems
✅ Both work simultaneously

### For Project Maintenance
✅ Clear separation of concerns
✅ Each system independent
✅ Easier to maintain
✅ Lower risk of bugs

## Repository Organization

```
pebble-flutter/
├── README.md                       # "Choose your path: C or Flutter"
├── SETUP.md                        # Classic firmware setup
├── flutter_watchface/
│   └── README.md                   # "Flutter developers start here"
└── docs/
    ├── classic/                    # C/RockyJS docs
    └── flutter/                    # Flutter docs
```

## Documentation Split

### For Classic Developers
- Read: SETUP.md, classic firmware docs
- Ignore: flutter_watchface/ directory entirely

### For Flutter Developers
- Read: flutter_watchface/README.md
- Ignore: src/fw/, waf, ARM toolchain

## Next Steps to Make This Real

1. **Create `flutter_watchface/` package** (standalone Flutter package)
2. **Build desktop simulator** (test without real hardware)
3. **Create example watchfaces** (prove it works)
4. **Define minimal C interface** (just the frame submission)
5. **Build packaging tools** (convert Flutter app → .pbw)

## The Beautiful Part

**Both systems are completely independent:**

```
Classic Path:              Flutter Path:
    100% Existing              100% New
    No Changes                 No Firmware Touching
    Works Forever              Works Independently
```

They only meet at the compositor/display level, which is a clean interface.

---

**This is the best of both worlds:**
- Preserve the existing ecosystem
- Enable modern Flutter development
- Keep both completely isolated
- Let developers choose their path

Ready to build the standalone Flutter module?
