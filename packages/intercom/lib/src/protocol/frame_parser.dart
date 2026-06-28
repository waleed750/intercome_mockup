import 'dart:typed_data';

import 'frame.dart';

typedef FrameSink = void Function(Channel channel, Uint8List payload);

final class FrameParser {
  FrameParser({
    this.maxPayloadLength = defaultMaxPayload,
    required this.onFrame,
  });

  static const initialCapacity = 64 * 1024;
  static const defaultMaxPayload = 4 * 1024 * 1024;

  final int maxPayloadLength;
  final FrameSink onFrame;

  Uint8List _buffer = Uint8List(initialCapacity);
  int _size = 0;

  void offer(List<int> data, [int offset = 0, int? length]) {
    final count = length ?? data.length - offset;
    RangeError.checkValidRange(offset, offset + count, data.length);
    _ensureCapacity(_size + count);
    _buffer.setRange(_size, _size + count, data, offset);
    _size += count;
    _drain();
  }

  void reset() {
    _size = 0;
  }

  void _drain() {
    var pos = 0;
    while (true) {
      if (_size - pos < Frame.magicSize) break;

      final channel = _magicAt(pos);
      if (channel == null) {
        pos++;
        continue;
      }
      if (_size - pos < Frame.headerSize) break;

      final len = _readLengthLE(pos + Frame.magicSize);
      if (len < 0 || len > maxPayloadLength) {
        pos++;
        continue;
      }

      final frameEnd = pos + Frame.headerSize + len;
      if (frameEnd > _size) break;

      onFrame(
          channel,
          Uint8List.fromList(
              _buffer.sublist(pos + Frame.headerSize, frameEnd)));
      pos = frameEnd;
    }
    _compact(pos);
  }

  Channel? _magicAt(int index) {
    final marker = _buffer[index];
    final channel = Channel.forMarker(marker);
    if (channel == null) return null;
    return _buffer[index + 1] == marker &&
            _buffer[index + 2] == marker &&
            _buffer[index + 3] == marker
        ? channel
        : null;
  }

  int _readLengthLE(int offset) {
    return _buffer[offset] |
        (_buffer[offset + 1] << 8) |
        (_buffer[offset + 2] << 16) |
        (_buffer[offset + 3] << 24);
  }

  void _compact(int consumed) {
    if (consumed == 0) return;
    final remaining = _size - consumed;
    if (remaining > 0) {
      _buffer.setRange(0, remaining, _buffer, consumed);
    }
    _size = remaining;
  }

  void _ensureCapacity(int needed) {
    if (needed <= _buffer.length) return;
    var newCapacity = _buffer.length;
    while (newCapacity < needed) {
      newCapacity <<= 1;
    }
    final next = Uint8List(newCapacity);
    next.setRange(0, _size, _buffer);
    _buffer = next;
  }
}
