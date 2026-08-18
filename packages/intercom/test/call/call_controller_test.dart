import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:flutter_local_notifications_platform_interface/flutter_local_notifications_platform_interface.dart';
import 'package:intercom/src/call/call_controller.dart';
import 'package:intercom/src/config/device_config.dart';
import 'package:intercom/src/intercom_mode.dart';
import 'package:intercom/src/protocol/commands.dart';
import 'package:intercom/src/protocol/frame.dart';
import 'package:shared_preferences/shared_preferences.dart';

final class _FakeNotificationsPlatform extends FlutterLocalNotificationsPlatform {
  @override
  Future<void> cancel(int id, {String? tag}) async {}

  @override
  Future<void> cancelAll() async {}

  @override
  Future<List<PendingNotificationRequest>>
      pendingNotificationRequests() async => [];
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  FlutterLocalNotificationsPlatform.instance = _FakeNotificationsPlatform();

  final methodChannels = <String, MethodChannel>{};

  void registerFakeChannel(String name) {
    final channel = MethodChannel(name);
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, (MethodCall methodCall) async {
      methodChannels[name] = channel;
      if (name == 'syncn_intercom/audio_uplink') {
        return Stream<Uint8List>.empty();
      }
      if (name == 'flutter.baseflow.com/permissions/methods') {
        return 1; // PermissionStatus.granted
      }
      return null;
    });
  }

  setUp(() {
    registerFakeChannel('syncn_intercom/audio');
    registerFakeChannel('syncn_intercom/video');
    registerFakeChannel('syncn_intercom/ringer');
    registerFakeChannel('syncn_intercom/audio_uplink');
    registerFakeChannel('flutter.baseflow.com/permissions/methods');
    registerFakeChannel('vibration');
  });

  tearDown(() {
    for (final entry in methodChannels.entries) {
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
          .setMockMethodCallHandler(entry.value, null);
    }
    methodChannels.clear();
  });

  Future<DeviceConfig> makeConfig() async {
    SharedPreferences.setMockInitialValues({});
    final prefs = await SharedPreferences.getInstance();
    final config = DeviceConfig(prefs);
    await config.ensureDefaults();
    await config.save(config.identity.copyWith(
      doorAddress: '127.0.0.1',
    ));
    return config;
  }

  group('viewDoor', () {
    test('sends exactly 3 answer frames in order and no audio', () async {
      final server = await ServerSocket.bind(InternetAddress.loopbackIPv4, 0);
      final port = server.port;
      Socket? client;

      server.listen((socket) {
        client = socket;
      });

      final config = await makeConfig();
      final controller = CallController(
        mode: IntercomMode.panel,
        deviceConfig: config,
      );

      final received = <(Channel, Uint8List)>[];
      final buffer = <int>[];
      final done = Completer<void>();

      await controller.viewDoor(host: '127.0.0.1', port: port);
      // Wait for the server side to accept the connection
      await Future.delayed(const Duration(milliseconds: 50));
      expect(client, isNotNull);

      client!.listen(
        (data) {
          buffer.addAll(data);
          while (buffer.length >= 8) {
            final magic = Uint8List.fromList(buffer.sublist(0, 4));
            final lenBytes = Uint8List.fromList(buffer.sublist(4, 8));
            final len =
                lenBytes.buffer.asByteData().getInt32(0, Endian.little);
            if (buffer.length < 8 + len) break;
            final channel = switch (magic) {
              [0xAA, 0xAA, 0xAA, 0xAA] => Channel.control,
              [0xBB, 0xBB, 0xBB, 0xBB] => Channel.video,
              [0xCC, 0xCC, 0xCC, 0xCC] => Channel.audio,
              _ => throw StateError('unknown magic'),
            };
            final payload = Uint8List.fromList(buffer.sublist(8, 8 + len));
            received.add((channel, payload));
            buffer.removeRange(0, 8 + len);
          }
        },
        onDone: () {
          if (!done.isCompleted) done.complete();
        },
      );

      // Give some time for frames to arrive
      await Future.delayed(const Duration(milliseconds: 200));

      expect(received, hasLength(3));
      expect(
          received.map((f) => f.$1), [Channel.control, Channel.control, Channel.control]);

      final expected = Commands.answerSequence;
      for (var i = 0; i < 3; i++) {
        final decoded = jsonDecode(utf8.decode(received[i].$2));
        expect(decoded['command'], jsonDecode(expected[i])['command']);
        if (jsonDecode(expected[i]).containsKey('OtherAnswer')) {
          expect(decoded['OtherAnswer'],
              jsonDecode(expected[i])['OtherAnswer']);
        }
      }

      // No audio frames
      expect(received.where((f) => f.$1 == Channel.audio), isEmpty);

      await controller.endCall();
      client?.destroy();
      await server.close();
    });
  });

  group('talk', () {
    test('sends StartTalk after talk() is called', () async {
      final server = await ServerSocket.bind(InternetAddress.loopbackIPv4, 0);
      final port = server.port;
      Socket? client;

      server.listen((socket) {
        client = socket;
      });

      final config = await makeConfig();
      final controller = CallController(
        mode: IntercomMode.panel,
        deviceConfig: config,
      );

      final received = <(Channel, Uint8List)>[];
      final buffer = <int>[];

      await controller.viewDoor(host: '127.0.0.1', port: port);
      await Future.delayed(const Duration(milliseconds: 50));
      expect(client, isNotNull);

      client!.listen(
        (data) {
          buffer.addAll(data);
          while (buffer.length >= 8) {
            final magic = Uint8List.fromList(buffer.sublist(0, 4));
            final lenBytes = Uint8List.fromList(buffer.sublist(4, 8));
            final len =
                lenBytes.buffer.asByteData().getInt32(0, Endian.little);
            if (buffer.length < 8 + len) break;
            final channel = switch (magic) {
              [0xAA, 0xAA, 0xAA, 0xAA] => Channel.control,
              [0xBB, 0xBB, 0xBB, 0xBB] => Channel.video,
              [0xCC, 0xCC, 0xCC, 0xCC] => Channel.audio,
              _ => throw StateError('unknown magic'),
            };
            final payload = Uint8List.fromList(buffer.sublist(8, 8 + len));
            received.add((channel, payload));
            buffer.removeRange(0, 8 + len);
          }
        },
      );

      // Wait for answer frames
      await Future.delayed(const Duration(milliseconds: 200));
      final answerCount =
          received.where((f) => f.$1 == Channel.control).length;
      expect(answerCount, 3);

      // Call talk
      controller.talk();
      // Wait for 1s delay + frames
      await Future.delayed(const Duration(milliseconds: 1500));

      // Should now have 4 control frames: 3 answers + 1 StartTalk
      final controlFrames = received.where((f) => f.$1 == Channel.control).toList();
      expect(controlFrames, hasLength(4));

      final lastControl = jsonDecode(utf8.decode(controlFrames.last.$2));
      expect(lastControl['command'], 'StartTalk');

      await controller.endCall();
      client?.destroy();
      await server.close();
    });
  });
}
