import 'dart:async';

/// Service that provides time updates for watchfaces
///
/// This service emits time updates at configurable intervals to trigger
/// watchface updates when the displayed time changes.
///
/// ## Usage
///
/// ```dart
/// // Update every minute (default)
/// TimeService().tickStream
///
/// // Update every second
/// TimeService(tickInterval: Duration(seconds: 1)).tickStream
///
/// // Update every 5 seconds
/// TimeService(tickInterval: Duration(seconds: 5)).tickStream
///
/// // Update 10 times per second (100ms)
/// TimeService(tickInterval: Duration(milliseconds: 100)).tickStream
///
/// // Update at high frequency
/// TimeService(tickInterval: Duration(microseconds: 16667)).tickStream // ~60 FPS
/// ```
///
/// ## Implementation Note
///
/// Dart's DateTime class doesn't have built-in streams or reactive capabilities.
/// We create our own stream using Timer to emit updates at the specified interval.
///
/// For intervals >= 1 second that divide evenly into 60 seconds (1s, 2s, 3s, 5s,
/// 10s, 15s, 20s, 30s, 60s), the timer aligns to those boundaries to prevent drift.
/// For sub-second intervals or other intervals, updates occur periodically without
/// boundary alignment.
class TimeService {
  final Duration tickInterval;

  static final Map<Duration, TimeService> _instances = {};

  factory TimeService({Duration tickInterval = const Duration(minutes: 1)}) {
    return _instances.putIfAbsent(
      tickInterval,
      () => TimeService._internal(tickInterval),
    );
  }

  TimeService._internal(this.tickInterval);

  StreamController<DateTime>? _controller;
  Timer? _timer;

  /// Stream of time updates
  ///
  /// Emits the current time at intervals specified by [tickInterval].
  /// Supports any Duration value from microseconds to hours.
  ///
  /// For intervals >= 1 second that divide evenly into 60 seconds,
  /// updates are aligned to those boundaries to prevent drift.
  ///
  /// Example:
  /// ```dart
  /// StreamBuilder<DateTime>(
  ///   stream: TimeService(tickInterval: Duration(seconds: 1)).tickStream,
  ///   builder: (context, snapshot) {
  ///     if (!snapshot.hasData) return Container();
  ///     final time = snapshot.data!;
  ///     return Text(DateFormat('HH:mm:ss').format(time));
  ///   },
  /// )
  /// ```
  Stream<DateTime> get tickStream {
    _controller ??= StreamController<DateTime>.broadcast();
    _startTimer();
    return _controller!.stream;
  }

  void _startTimer() {
    if (_timer != null) return;

    // Emit current time immediately
    _controller?.add(DateTime.now());

    // Schedule first update at next tick interval
    _scheduleNextUpdate();
  }

  void _scheduleNextUpdate() {
    final now = DateTime.now();
    final intervalMicroseconds = tickInterval.inMicroseconds;

    Duration delay;

    // For intervals of 1 second or more that divide evenly into 60 seconds,
    // align to those boundaries to prevent drift
    if (intervalMicroseconds >= Duration.microsecondsPerSecond &&
        intervalMicroseconds % Duration.microsecondsPerSecond == 0) {
      final intervalSeconds = tickInterval.inSeconds;

      if (intervalSeconds <= 60 && 60 % intervalSeconds == 0) {
        // Calculate time until next aligned boundary
        final currentSecond = now.second;
        final nextAlignedSecond =
            ((currentSecond ~/ intervalSeconds) + 1) * intervalSeconds;

        // If we've crossed into the next minute, adjust
        if (nextAlignedSecond >= 60) {
          final nextMinute = DateTime(
            now.year,
            now.month,
            now.day,
            now.hour,
            now.minute + 1,
          );
          delay = nextMinute.difference(now);
        } else {
          // Calculate delay to next aligned second within this minute
          delay = Duration(
            seconds: nextAlignedSecond - currentSecond,
            microseconds: -now.microsecond,
          );
        }
      } else {
        // For intervals > 60 seconds or that don't divide evenly, use interval as-is
        delay = tickInterval;
      }
    } else {
      // For sub-second intervals, just use the interval as-is
      delay = tickInterval;
    }

    // Schedule update at that exact time
    _timer = Timer(delay, () {
      _controller?.add(DateTime.now());

      // Recalculate for next update to prevent drift
      _scheduleNextUpdate();
    });
  }

  /// Stop the timer (cleanup)
  void dispose() {
    _timer?.cancel();
    _timer = null;
    _controller?.close();
    _controller = null;
  }

  /// Get current time (for one-off access)
  static DateTime get now => DateTime.now();
}
