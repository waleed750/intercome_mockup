import 'package:flutter_test/flutter_test.dart';
import 'package:intercom/src/protocol/frame.dart';
import 'package:intercom/src/protocol/frame_parser.dart';

void main() {
  List<int> payload(List<int> bytes) => bytes;

  FrameParser parser(List<(Channel, List<int>)> sink,
      {int maxPayload = 1 << 20}) {
    return FrameParser(
      maxPayloadLength: maxPayload,
      onFrame: (channel, payload) => sink.add((channel, payload)),
    );
  }

  test('parses single whole frame', () {
    final frames = <(Channel, List<int>)>[];
    final p = payload([1, 2, 3, 4, 5]);
    parser(frames).offer(Frame.encode(Channel.control, p));
    expect(frames, hasLength(1));
    expect(frames.first.$1, Channel.control);
    expect(frames.first.$2, p);
  });

  test('parses merged frames in one offer', () {
    final frames = <(Channel, List<int>)>[];
    final wire = [
      ...Frame.encode(Channel.control, payload([10, 11])),
      ...Frame.encode(Channel.video, payload([20, 21, 22])),
      ...Frame.encode(Channel.audio, payload([30])),
    ];
    parser(frames).offer(wire);
    expect(frames.map((f) => f.$1),
        [Channel.control, Channel.video, Channel.audio]);
    expect(frames[1].$2, [20, 21, 22]);
  });

  test('reassembles frame fed byte by byte', () {
    final frames = <(Channel, List<int>)>[];
    final p = payload([7, 6, 5, 4, 3, 2]);
    final wire = Frame.encode(Channel.video, p);
    final pser = parser(frames);
    for (final byte in wire) {
      pser.offer([byte]);
    }
    expect(frames, hasLength(1));
    expect(frames.first.$2, p);
  });

  test('emits complete frames and buffers partial tail', () {
    final frames = <(Channel, List<int>)>[];
    final a = Frame.encode(Channel.control, payload([1]));
    final b = Frame.encode(Channel.video, payload([2, 3, 4, 5]));
    final pser = parser(frames);
    pser.offer([...a, ...b.sublist(0, 3)]);
    expect(frames, hasLength(1));
    pser.offer(b, 3, b.length - 3);
    expect(frames, hasLength(2));
    expect(frames[1].$2, [2, 3, 4, 5]);
  });

  test('skips garbage and resyncs past implausible length', () {
    final frames = <(Channel, List<int>)>[];
    final bogus = [0xAA, 0xAA, 0xAA, 0xAA, 0xFF, 0xFF, 0xFF, 0x7F];
    final good = Frame.encode(Channel.audio, payload([1, 2, 3]));
    parser(frames, maxPayload: 1024).offer([...bogus, ...good]);
    expect(frames, hasLength(1));
    expect(frames.first.$1, Channel.audio);
    expect(frames.first.$2, [1, 2, 3]);
  });

  test('reset drops buffered bytes', () {
    final frames = <(Channel, List<int>)>[];
    final pser = parser(frames);
    final partial = Frame.encode(Channel.control, payload([1, 2, 3, 4]));
    pser.offer(partial, 0, 6);
    pser.reset();
    pser.offer(Frame.encode(Channel.audio, payload([5])));
    expect(frames, hasLength(1));
    expect(frames.first.$1, Channel.audio);
  });
}
