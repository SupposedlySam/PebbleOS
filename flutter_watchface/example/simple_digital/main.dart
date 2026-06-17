import 'package:flutter/material.dart';
import 'package:flutter_watchface/flutter_watchface.dart';
import 'package:intl/intl.dart';

void main() {
  runApp(const WatchfaceApp());
}

class WatchfaceApp extends StatelessWidget {
  const WatchfaceApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Simple Digital Watchface',
      debugShowCheckedModeBanner: false,
      theme: ThemeData.dark(),
      home: const Scaffold(body: Center(child: SimpleDigitalWatchface())),
    );
  }
}

/// A simple digital watchface showing the current time
class SimpleDigitalWatchface extends PebbleWatchface {
  const SimpleDigitalWatchface({super.key});

  @override
  Widget build(BuildContext context) {
    return PebbleDisplay(
      child: Container(
        color: Colors.black,
        child: Center(
          child: StreamBuilder<DateTime>(
            stream: TimeService().tickStream,
            builder: (context, snapshot) {
              if (!snapshot.hasData) {
                return const Text(
                  '--:--',
                  style: TextStyle(
                    fontSize: 48,
                    fontWeight: FontWeight.bold,
                    color: Colors.white,
                  ),
                );
              }

              final time = snapshot.data!;
              return Column(
                mainAxisAlignment: MainAxisAlignment.center,
                children: [
                  // Time
                  Text(
                    DateFormat('HH:mm').format(time),
                    style: const TextStyle(
                      fontSize: 48,
                      fontWeight: FontWeight.bold,
                      color: Colors.white,
                    ),
                  ),
                  const SizedBox(height: 8),
                  // Date
                  Text(
                    DateFormat('EEEE, MMM d').format(time),
                    style: const TextStyle(fontSize: 16, color: Colors.white70),
                  ),
                ],
              );
            },
          ),
        ),
      ),
    );
  }
}
