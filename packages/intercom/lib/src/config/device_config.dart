import 'dart:math';

import 'package:flutter/foundation.dart';
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
    final stopwatch = Stopwatch()..start();
    debugPrint('Intercom config load started: shared_preferences.getInstance');
    final prefs = await SharedPreferences.getInstance();
    debugPrint(
        'Intercom config load finished: shared_preferences.getInstance (${stopwatch.elapsedMilliseconds}ms)');
    final config = DeviceConfig(prefs);
    stopwatch
      ..reset()
      ..start();
    debugPrint('Intercom config load started: ensureDefaults');
    await config.ensureDefaults();
    debugPrint(
        'Intercom config load finished: ensureDefaults (${stopwatch.elapsedMilliseconds}ms)');
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
    final address = _prefs.getString(_dstAddrKey);
    await _prefs.setString(_dstAddrKey,
        _isValidAddress(address ?? '') ? address! : defaults.dstAddr);
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
        _isValidAddress(identity.dstAddr.trim())
            ? identity.dstAddr.trim()
            : _isValidAddress(defaults.dstAddr)
                ? defaults.dstAddr
                : '1');
    await _prefs.setString(
        _doorNameKey,
        identity.doorName.trim().isEmpty
            ? defaults.doorName
            : identity.doorName.trim());
  }

  static DeviceIdentity _generateDefaults() {
    final address = 1 + Random.secure().nextInt(999);
    final suffix = 1000 + Random.secure().nextInt(9000);
    final serial = List<int>.generate(12, (_) => Random.secure().nextInt(16))
        .map((n) => n.toRadixString(16))
        .join();
    return DeviceIdentity(
      alias: 'Indoor $suffix',
      serial: serial,
      dstAddr: '$address',
      doorName: 'Front Door',
    );
  }

  static bool _isValidAddress(String value) {
    final address = int.tryParse(value);
    return address != null && address >= 1 && address <= 999;
  }
}
