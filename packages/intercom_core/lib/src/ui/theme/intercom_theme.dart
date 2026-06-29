import 'package:flutter/material.dart';

final class IntercomTheme {
  static ThemeData dark() {
    final scheme = ColorScheme.fromSeed(
      seedColor: const Color(0xFF1DAF8A),
      brightness: Brightness.dark,
      surface: const Color(0xFF101417),
    );
    return ThemeData(
      colorScheme: scheme,
      useMaterial3: true,
      scaffoldBackgroundColor: const Color(0xFF0B0F12),
    );
  }
}
