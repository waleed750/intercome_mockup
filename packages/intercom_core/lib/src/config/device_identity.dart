final class DeviceIdentity {
  const DeviceIdentity({
    required this.alias,
    required this.serial,
    required this.dstAddr,
    required this.doorName,
  });

  final String alias;
  final String serial;
  final String dstAddr;
  final String doorName;

  DeviceIdentity copyWith({
    String? alias,
    String? serial,
    String? dstAddr,
    String? doorName,
  }) {
    return DeviceIdentity(
      alias: alias ?? this.alias,
      serial: serial ?? this.serial,
      dstAddr: dstAddr ?? this.dstAddr,
      doorName: doorName ?? this.doorName,
    );
  }
}
