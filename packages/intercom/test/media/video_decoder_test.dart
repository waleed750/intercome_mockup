import 'dart:async';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:intercom/src/media/video_decoder.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  test('frames submitted before start() are queued and flushed on start',
      () async {
    const channel = MethodChannel('syncn_intercom/video');
    final submitted = <Uint8List>[];
    final startCompleter = Completer<void>();

    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, (call) async {
      switch (call.method) {
        case 'start':
          await startCompleter.future;
          return 42;
        case 'submit':
          submitted.add(call.arguments as Uint8List);
          return null;
        default:
          return null;
      }
    });
    addTearDown(() => TestDefaultBinaryMessengerBinding
        .instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, null));

    final decoder = VideoDecoder(channel: channel);

    // No start() call yet: submissions must be queued, not dropped.
    final early1 = Uint8List.fromList([1, 2, 3]);
    final early2 = Uint8List.fromList([4, 5, 6]);
    await decoder.submit(early1);
    await decoder.submit(early2);
    expect(submitted, isEmpty);
    expect(decoder.textureId, isNull);

    final startFuture = decoder.start();
    startCompleter.complete();
    final textureId = await startFuture;

    expect(textureId, 42);
    expect(submitted, [early1, early2]);
  });
}
