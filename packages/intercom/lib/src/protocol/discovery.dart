import 'dart:convert';
import 'dart:typed_data';

final class ScreenInfo {
  const ScreenInfo({
    this.command = 'cmd_reply_get_device_info',
    this.appid = '7551000',
    required this.alias,
    this.groupIp = '239.255.74.199',
    required this.serial,
    this.dstType = 3,
    required this.dstAddr,
    this.verify = 0,
    this.deviceBusy = 0,
    this.cameraEn = 0,
    this.relay0Delay = 1,
  });

  final String command;
  final String appid;
  final String alias;
  final String groupIp;
  final String serial;
  final int dstType;
  final String dstAddr;
  final int verify;
  final int deviceBusy;
  final int cameraEn;
  final int relay0Delay;

  ScreenInfo copyWith({int? dstType, String? dstAddr}) => ScreenInfo(
        command: command,
        appid: appid,
        alias: alias,
        groupIp: groupIp,
        serial: serial,
        dstType: dstType ?? this.dstType,
        dstAddr: dstAddr ?? this.dstAddr,
        verify: verify,
        deviceBusy: deviceBusy,
        cameraEn: cameraEn,
        relay0Delay: relay0Delay,
      );

  Map<String, Object?> toJson() => {
        'command': command,
        'appid': appid,
        'alias': alias,
        'group_ip': groupIp,
        'serial': serial,
        'dstType': dstType,
        'dstAddr': dstAddr,
        'verify': verify,
        'deviceBusy': deviceBusy,
        'camera_en': cameraEn,
        'relay0_delay': relay0Delay,
      };
}

final class DoorAddr {
  const DoorAddr(this.addr, this.type);

  final String addr;
  final int type;
}

final class Discovery {
  static const port = 8089;
  static const _requestTokens = [
    'cmd_send_get_call_device',
    'cmd_send_get_device_info'
  ];

  static bool isDiscoveryRequest(List<int> payload, [int? length]) {
    final text = utf8.decode(payload.take(length ?? payload.length).toList(),
        allowMalformed: true);
    return _requestTokens.any(text.contains);
  }

  static DoorAddr? doorAddrFrom(List<int> payload, [int? length]) {
    try {
      final text = utf8.decode(payload.take(length ?? payload.length).toList(),
          allowMalformed: true);
      final decoded = jsonDecode(text);
      if (decoded is! Map<String, Object?>) return null;
      final addr = decoded['localAddr'] as String?;
      if (addr == null) return null;
      return DoorAddr(addr, decoded['localType'] as int? ?? 3);
    } catch (_) {
      return null;
    }
  }

  static Uint8List buildReply(ScreenInfo info, [DoorAddr? door]) {
    final reply = door == null
        ? info
        : info.copyWith(dstAddr: door.addr, dstType: door.type);
    return Uint8List.fromList(utf8.encode(jsonEncode(reply.toJson())));
  }
}
