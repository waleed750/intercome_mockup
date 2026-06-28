import 'dart:typed_data';

final class ALawCodec {
  static const _signBit = 0x80;
  static const _quantMask = 0x0F;
  static const _segShift = 4;
  static const _segMask = 0x70;
  static const _segEnd = [0x1F, 0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF];

  static final List<int> _decodeTable = List<int>.generate(256, _decodeSample);

  static int decodeByte(int aLaw) => _decodeTable[aLaw & 0xFF];

  static Uint8List decode(List<int> aLaw, [int offset = 0, int? length]) {
    final count = length ?? aLaw.length - offset;
    final out = Uint8List(count * 2);
    var o = 0;
    for (var i = offset; i < offset + count; i++) {
      final sample = _decodeTable[aLaw[i] & 0xFF];
      out[o++] = sample & 0xFF;
      out[o++] = (sample >> 8) & 0xFF;
    }
    return out;
  }

  static Uint8List encode(List<int> pcm, [int offset = 0, int? length]) {
    final count = length ?? pcm.length - offset;
    final samples = count ~/ 2;
    final out = Uint8List(samples);
    var i = offset;
    for (var n = 0; n < samples; n++) {
      final lo = pcm[i] & 0xFF;
      final hi = pcm[i + 1];
      i += 2;
      var sample = (hi << 8) | lo;
      if ((sample & 0x8000) != 0) sample -= 0x10000;
      out[n] = encodeSample(sample);
    }
    return out;
  }

  static int encodeSample(int pcm16) {
    var pcm = pcm16 >> 3;
    final int mask;
    if (pcm >= 0) {
      mask = 0xD5;
    } else {
      mask = 0x55;
      pcm = -pcm - 1;
    }
    final seg = _search(pcm);
    final aval = seg >= 8
        ? 0x7F
        : (seg << _segShift) |
            (seg < 2 ? (pcm >> 1) & _quantMask : (pcm >> seg) & _quantMask);
    return (aval ^ mask) & 0xFF;
  }

  static int _decodeSample(int aLawByte) {
    final a = aLawByte ^ 0x55;
    var t = (a & _quantMask) << 4;
    final seg = (a & _segMask) >> _segShift;
    if (seg == 0) {
      t += 8;
    } else if (seg == 1) {
      t += 0x108;
    } else {
      t += 0x108;
      t <<= seg - 1;
    }
    return (a & _signBit) != 0 ? t : -t;
  }

  static int _search(int value) {
    for (var i = 0; i < _segEnd.length; i++) {
      if (value <= _segEnd[i]) return i;
    }
    return _segEnd.length;
  }
}
