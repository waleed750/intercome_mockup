import 'package:flutter/material.dart';

final class PulsingText extends StatefulWidget {
  const PulsingText(this.text, {super.key});

  final String text;

  @override
  State<PulsingText> createState() => _PulsingTextState();
}

final class _PulsingTextState extends State<PulsingText>
    with SingleTickerProviderStateMixin {
  late final AnimationController _controller = AnimationController(
    vsync: this,
    duration: const Duration(milliseconds: 900),
  )..repeat(reverse: true);

  @override
  void dispose() {
    _controller.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return FadeTransition(
        opacity: Tween(begin: 0.45, end: 1.0).animate(_controller),
        child: Text(widget.text));
  }
}
