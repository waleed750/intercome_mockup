final class DeviceIdentity {
  const DeviceIdentity({
    required this.alias,
    required this.serial,
    required this.dstAddr,
    required this.doorName,
    this.doorAddress = '192.168.100.193',
  });

  final String alias;
  final String serial;
  final String dstAddr;
  final String doorName;
  final String doorAddress;

  DeviceIdentity copyWith({
    String? alias,
    String? serial,
    String? dstAddr,
    String? doorName,
    String? doorAddress,
  }) {
    return DeviceIdentity(
      alias: alias ?? this.alias,
      serial: serial ?? this.serial,
      dstAddr: dstAddr ?? this.dstAddr,
      doorName: doorName ?? this.doorName,
      doorAddress: doorAddress ?? this.doorAddress,
    );
  }
}
