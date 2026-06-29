import 'dart:convert';
import 'dart:typed_data';

import 'frame.dart';

enum InboundCommand { call, getCallInfo, hangUp, unknown }

final class ControlMessage {
  const ControlMessage({this.command, this.cmd});

  final String? command;
  final String? cmd;

  String? get name => command ?? cmd;

  InboundCommand classify() {
    switch (name?.trim().toLowerCase()) {
      case 'call':
        return InboundCommand.call;
      case 'getcallinfo':
        return InboundCommand.getCallInfo;
      case 'hangup':
        return InboundCommand.hangUp;
      default:
        return InboundCommand.unknown;
    }
  }
}

final class Commands {
  static ControlMessage? parse(List<int> payload) {
    try {
      final decoded = jsonDecode(utf8.decode(payload));
      if (decoded is! Map<String, Object?>) return null;
      return ControlMessage(
        command: decoded['command'] as String?,
        cmd: decoded['cmd'] as String?,
      );
    } catch (_) {
      return null;
    }
  }

  static Uint8List openDoor() => _control('{"command":"OpenDoor"}');
  static Uint8List startTalk() => _control('{"command":"StartTalk"}');
  static Uint8List hangUp() => _control('{"command":"HangUp","OtherAnswer":0}');
  static Uint8List deviceBusy() => _control('{"command":"deviceBusy"}');

  static const answerSequence = [
    '{"command":"Answer"}',
    '{"command":"Answer","OtherAnswer":1}',
    '{"command":"Answer","OtherAnswer":true}',
  ];

  static List<Uint8List> answerFrames() =>
      answerSequence.map(_control).toList();

  static Uint8List _control(String jsonText) {
    return Frame.encode(Channel.control, utf8.encode(jsonText));
  }
}
