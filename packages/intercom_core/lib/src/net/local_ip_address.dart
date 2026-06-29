import 'dart:io';

Future<String?> resolveLocalIpv4Address() async {
  final interfaces = await NetworkInterface.list(
    type: InternetAddressType.IPv4,
    includeLinkLocal: false,
    includeLoopback: false,
  );

  for (final interface in interfaces) {
    for (final address in interface.addresses) {
      final value = address.address;
      if (_isUsableIpv4(value)) return value;
    }
  }
  return null;
}

bool _isUsableIpv4(String value) {
  if (value.startsWith('127.') || value.startsWith('169.254.')) return false;
  return InternetAddress.tryParse(value)?.type == InternetAddressType.IPv4;
}
