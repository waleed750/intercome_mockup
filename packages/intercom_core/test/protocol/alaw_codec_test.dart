import 'package:flutter_test/flutter_test.dart';
import 'package:intercom_core/intercom_core.dart';

void main() {
  test('encode of decode is identity for all 256 codes', () {
    for (var code = 0; code <= 255; code++) {
      final pcm = ALawCodec.decodeByte(code);
      final reencoded = ALawCodec.encodeSample(pcm);
      expect(reencoded, code, reason: 'round-trip failed for code $code');
    }
  });

  test('encode buffer round trips through decode buffer', () {
    final codes = List<int>.generate(256, (i) => i);
    final pcm = ALawCodec.decode(codes);
    final reencoded = ALawCodec.encode(pcm);
    expect(reencoded, codes);
  });

  test('decode buffer emits little endian pcm16 per sample', () {
    final codes = [0x00, 0x55, 0xD5, 0x2A, 0x7F, 0xFF];
    final pcm = ALawCodec.decode(codes);
    expect(pcm, hasLength(codes.length * 2));
    for (var i = 0; i < codes.length; i++) {
      final expected = ALawCodec.decodeByte(codes[i]);
      var sample = (pcm[i * 2 + 1] << 8) | pcm[i * 2];
      if ((sample & 0x8000) != 0) sample -= 0x10000;
      expect(sample, expected);
    }
  });

  test('signed zero codes decode to opposite small magnitudes', () {
    expect(ALawCodec.decodeByte(0xD5), 8);
    expect(ALawCodec.decodeByte(0x55), -8);
    expect(ALawCodec.encodeSample(8), 0xD5);
    expect(ALawCodec.encodeSample(-8), 0x55);
  });

  test('encodes full scale samples to expected codes', () {
    expect(ALawCodec.encodeSample(32767), 0xAA);
    expect(ALawCodec.encodeSample(-32768), 0x2A);
  });
}
