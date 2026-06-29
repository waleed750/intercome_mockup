import 'dart:typed_data';

enum Channel {
  control(0xAA),
  video(0xBB),
  audio(0xCC);

  const Channel(this.marker);

  final int marker;

  static Channel? forMarker(int marker) {
    for (final channel in values) {
      if (channel.marker == marker) return channel;
    }
    return null;
  }
}

final class Frame {
  static const magicSize = 4;
  static const lengthSize = 4;
  static const headerSize = magicSize + lengthSize;

  static Uint8List encode(Channel channel, List<int> payload) {
    final out = Uint8List(headerSize + payload.length);
    out[0] = channel.marker;
    out[1] = channel.marker;
    out[2] = channel.marker;
    out[3] = channel.marker;
    final length = payload.length;
    out[4] = length & 0xFF;
    out[5] = (length >> 8) & 0xFF;
    out[6] = (length >> 16) & 0xFF;
    out[7] = (length >> 24) & 0xFF;
    out.setRange(headerSize, out.length, payload);
    return out;
  }
}
