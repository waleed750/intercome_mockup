import 'dart:math';

import 'package:shared_preferences/shared_preferences.dart';

import 'device_identity.dart';

final class DeviceConfig {
  DeviceConfig(this._prefs);

  static const _aliasKey = 'intercom.alias';
  static const _serialKey = 'intercom.serial';
  static const _dstAddrKey = 'intercom.dst_addr';
  static const _doorNameKey = 'intercom.door_name';

  final SharedPreferences _prefs;

  static Future<DeviceConfig> load() async {
    final prefs = await SharedPreferences.getInstance();
    final config = DeviceConfig(prefs);
    await config.ensureDefaults();
    return config;
  }

  DeviceIdentity get identity => DeviceIdentity(
        alias: _prefs.getString(_aliasKey) ?? _generateDefaults().alias,
        serial: _prefs.getString(_serialKey) ?? _generateDefaults().serial,
        dstAddr: _prefs.getString(_dstAddrKey) ?? _generateDefaults().dstAddr,
        doorName:
            _prefs.getString(_doorNameKey) ?? _generateDefaults().doorName,
      );

  Future<void> ensureDefaults() async {
    final defaults = _generateDefaults();
    await _prefs.setString(
        _aliasKey, _prefs.getString(_aliasKey) ?? defaults.alias);
    await _prefs.setString(
        _serialKey, _prefs.getString(_serialKey) ?? defaults.serial);
    await _prefs.setString(
        _dstAddrKey, _prefs.getString(_dstAddrKey) ?? defaults.dstAddr);
    await _prefs.setString(
        _doorNameKey, _prefs.getString(_doorNameKey) ?? defaults.doorName);
  }

  Future<void> save(DeviceIdentity identity) async {
    final defaults = _generateDefaults();
    await _prefs.setString(_aliasKey,
        identity.alias.trim().isEmpty ? defaults.alias : identity.alias.trim());
    await _prefs.setString(
        _serialKey,
        identity.serial.trim().isEmpty
            ? defaults.serial
            : identity.serial.trim());
    await _prefs.setString(
        _dstAddrKey,
        identity.dstAddr.trim().isEmpty
            ? defaults.dstAddr
            : identity.dstAddr.trim());
    await _prefs.setString(
        _doorNameKey,
        identity.doorName.trim().isEmpty
            ? defaults.doorName
            : identity.doorName.trim());
  }

  static DeviceIdentity _generateDefaults() {
    final suffix = 1000 + Random.secure().nextInt(9000);
    final serial = List<int>.generate(12, (_) => Random.secure().nextInt(16))
        .map((n) => n.toRadixString(16))
        .join();
    return DeviceIdentity(
      alias: 'Indoor $suffix',
      serial: serial,
      dstAddr: 'android-$suffix',
      doorName: 'Front Door',
    );
  }
}
