import 'package:flutter/material.dart';

final class VideoSurface extends StatelessWidget {
  const VideoSurface({super.key, required this.textureId, this.placeholder});

  final int? textureId;
  final Widget? placeholder;

  @override
  Widget build(BuildContext context) {
    final id = textureId;
    if (id == null || id < 0) {
      return ColoredBox(
        color: Colors.black,
        child: Center(
            child: placeholder ??
                const Icon(Icons.videocam_off_outlined, size: 48)),
      );
    }
    return Texture(textureId: id);
  }
}
