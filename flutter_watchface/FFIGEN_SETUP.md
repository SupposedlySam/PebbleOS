# FFigen Setup Complete ✅

The Flutter Watchface SDK now uses **ffigen** to auto-generate FFI bindings from the C header file.

## What Was Done

### 1. Added ffigen Dependency

```yaml
# pubspec.yaml
dev_dependencies:
  ffigen: ^19.1.0
```

### 2. Configured ffigen

```yaml
# pubspec.yaml
ffigen:
  name: WatchfaceNativeBindings
  description: "Auto-generated bindings for Pebble watchface C API"
  output: "lib/src/platform/ffi_bindings.g.dart"
  headers:
    entry-points:
      - "../src/fw/flutter_bridge/watchface_api.h"
```

### 3. Fixed C Header

Added `#include <stddef.h>` to `watchface_api.h` to define `size_t`.

### 4. Generated Bindings

Ran `dart run ffigen` to generate `lib/src/platform/ffi_bindings.g.dart` from the C header.

### 5. Updated Platform Code

Updated `watchface_platform.dart` to use the auto-generated bindings instead of manual bindings.

### 6. Added Scripts

Added scripts to `scripts.yaml` for easy regeneration:

- `sip run flutter-setup` - Install dependencies
- `sip run flutter-gen` - Regenerate FFI bindings
- `sip run flutter-example` - Run example watchface
- `sip run flutter-clean` - Clean Flutter build
- `sip run flutter-analyze` - Analyze Flutter code
- `sip run flutter-test` - Run Flutter tests
- `sip run flutter-format` - Format Flutter code

## How to Use

### Generate Bindings

Whenever you modify the C header (`src/fw/flutter_bridge/watchface_api.h`), regenerate bindings:

```bash
# Method 1: Direct command
cd flutter_watchface
dart run ffigen

# Method 2: Using sip script
sip run flutter-gen
```

### Workflow

1. **Modify C API** - Edit `src/fw/flutter_bridge/watchface_api.h`
2. **Regenerate Bindings** - Run `dart run ffigen`
3. **Update Dart Code** - Use the generated types in `watchface_platform.dart`
4. **Test** - Verify everything compiles and works

## Benefits

✅ **Type-Safe** - Generated from actual C header  
✅ **Maintainable** - Single source of truth (C header)  
✅ **No Manual Work** - Auto-generates all boilerplate  
✅ **Standard Approach** - Uses Dart's official FFI tool  
✅ **Always in Sync** - Dart code matches C API exactly

## File Structure

```
flutter_watchface/
├── lib/src/platform/
│   ├── ffi_bindings.g.dart       # AUTO-GENERATED (don't edit!)
│   └── watchface_platform.dart   # High-level API (edit this)
├── pubspec.yaml                   # ffigen configuration
└── FFI_BRIDGE.md                  # Architecture documentation

src/fw/flutter_bridge/
├── watchface_api.h                # C API header (SOURCE OF TRUTH)
└── watchface_api.c                # C implementation
```

## Important Notes

- **Never manually edit** `ffi_bindings.g.dart` - it's auto-generated
- **Always regenerate** bindings after C header changes
- **C header is the source of truth** for the API contract
- **Linter warnings** about unused Darwin types are expected and can be ignored

## Example: Adding a New API

### 1. Add to C Header

```c
// src/fw/flutter_bridge/watchface_api.h
typedef struct {
    uint8_t battery_level;
    bool is_charging;
} WatchfaceBatteryInfo;

WatchfaceBatteryInfo watchface_get_battery_info(void);
```

### 2. Regenerate Bindings

```bash
dart run ffigen
```

### 3. Use in Dart

```dart
// Automatically available!
final battery = platform._bindings.watchface_get_battery_info();
print('Battery: ${battery.battery_level}%');
```

That's it! The binding is automatically generated and type-safe.

## Resources

- [ffigen Package](https://pub.dev/packages/ffigen)
- [Dart FFI Documentation](https://dart.dev/guides/libraries/c-interop)
- [FFI_BRIDGE.md](FFI_BRIDGE.md) - Detailed architecture
- [README.md](README.md) - SDK documentation

---

**The Flutter Watchface SDK is now fully set up with auto-generated FFI bindings! 🎉**
