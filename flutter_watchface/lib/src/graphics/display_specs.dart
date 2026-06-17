/// Display specifications for Pebble watches
class DisplaySpecs {
  /// Display width in pixels
  final int width;

  /// Display height in pixels
  final int height;

  /// Color depth (1 for B&W, 8 for color)
  final int colorDepth;

  /// Display type (color or monochrome)
  final DisplayType type;

  /// Dots per inch (DPI) - optional, for physical size calculations
  final int? dpi;

  const DisplaySpecs({
    required this.width,
    required this.height,
    required this.colorDepth,
    required this.type,
    this.dpi,
  });

  /// Pebble Classic (monochrome)
  static const classic = DisplaySpecs(
    width: 144,
    height: 168,
    colorDepth: 1,
    type: DisplayType.monochrome,
  );

  /// Pebble Time (color)
  static const time = DisplaySpecs(
    width: 144,
    height: 168,
    colorDepth: 8,
    type: DisplayType.color,
  );

  /// Pebble Time Round (color)
  static const timeRound = DisplaySpecs(
    width: 180,
    height: 180,
    colorDepth: 8,
    type: DisplayType.color,
  );

  /// Pebble 2 (monochrome)
  static const pebble2 = DisplaySpecs(
    width: 144,
    height: 168,
    colorDepth: 1,
    type: DisplayType.monochrome,
  );

  /// Pebble 2 Duo (monochrome, 1.26" B/W, 176 DPI)
  static const pebble2Duo = DisplaySpecs(
    width: 144,
    height: 168,
    colorDepth: 1,
    type: DisplayType.monochrome,
    dpi: 176,
  );

  /// Pebble Time 2 (64-color, 1.5", 202 DPI)
  static const time2 = DisplaySpecs(
    width: 200,
    height: 228,
    colorDepth: 8,
    type: DisplayType.color,
    dpi: 202,
  );
}

enum DisplayType { monochrome, color }
