import 'package:flutter/widgets.dart';
import '../graphics/display_specs.dart';

/// Base class for all Pebble watchfaces built with Flutter
///
/// Extend this class to create your own watchface. Your watchface will
/// be constrained to the Pebble display dimensions and will receive
/// time tick events.
///
/// Example:
/// ```dart
/// class MyWatchface extends PebbleWatchface {
///   const MyWatchface({super.key});
///
///   @override
///   Widget build(BuildContext context) {
///     return PebbleDisplay(
///       child: Center(
///         child: Text('12:34'),
///       ),
///     );
///   }
/// }
/// ```
abstract class PebbleWatchface extends StatefulWidget {
  const PebbleWatchface({super.key});

  /// The display specifications for this watchface
  /// Default is Pebble Time (144x168, color)
  DisplaySpecs get displaySpecs => DisplaySpecs.time;

  @override
  State<PebbleWatchface> createState() => PebbleWatchfaceState();

  /// Build the watchface UI
  ///
  /// This method is called whenever the watchface needs to be rendered.
  /// Use [PebbleDisplay] widget to ensure your content is properly
  /// constrained to the display dimensions.
  Widget build(BuildContext context);
}

/// State for PebbleWatchface
class PebbleWatchfaceState extends State<PebbleWatchface> {
  @override
  Widget build(BuildContext context) {
    return widget.build(context);
  }
}

/// Widget that constrains its child to Pebble display dimensions
///
/// Use this as the root widget in your watchface to ensure it's
/// properly sized for the Pebble display.
class PebbleDisplay extends StatelessWidget {
  final Widget child;
  final DisplaySpecs displaySpecs;

  const PebbleDisplay({
    super.key,
    required this.child,
    this.displaySpecs = DisplaySpecs.time,
  });

  @override
  Widget build(BuildContext context) {
    return SizedBox(
      width: displaySpecs.width.toDouble(),
      height: displaySpecs.height.toDouble(),
      child: child,
    );
  }
}
