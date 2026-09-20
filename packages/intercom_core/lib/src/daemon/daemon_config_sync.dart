import 'dart:io';

/// Bridges the app's in-settings door IP into `syncn-intercomd`'s config
/// file, since the daemon has no runtime "set door address" IPC command --
/// `[door] address` is read once at process startup only (confirmed: no
/// `SYNCN_IPC_CMD_*` for it, and `config.c` never reloads -- see
/// `packaging/debian/syncn-intercom.conf`'s own header comment). Until the
/// daemon grows a real reload command, "sync the address" necessarily means
/// "rewrite the file, then restart the unit" -- there is no lighter-weight
/// option today.
///
/// This intentionally does NOT touch `[panel] alias`/`serial` -- those are
/// `DeviceConfig`'s job (the door-facing identity the panel presents over
/// the wire protocol), independent of which physical door IP the panel
/// dials. Only `[door] address` lives here.
final class DaemonConfigSync {
  const DaemonConfigSync({
    this.configPath = '/etc/syncn/intercom.conf',
    this.serviceName = 'syncn-intercomd.service',
  });

  final String configPath;
  final String serviceName;

  /// Rewrites `[door] address` in the daemon's config file to `doorIp` (blank
  /// clears it back to discovery-learn mode, matching the shipped config
  /// template's default) and restarts the daemon so it picks the change up.
  ///
  /// Best-effort: mock/dev builds and non-panel platforms have no
  /// `/etc/syncn/intercom.conf` and no `systemctl` at all, so every failure
  /// here is swallowed (logged, not thrown) -- matches this codebase's
  /// existing posture for non-critical native-side calls (see
  /// `IntercomCubit.callDoorDirectly`'s own try/catch). A door IP that fails
  /// to sync leaves the daemon on whatever it already had (its last learned
  /// address, or its previous config) rather than breaking the call flow.
  Future<void> syncDoorAddress(String doorIp) async {
    final file = File(configPath);
    if (!await file.exists()) {
      // Expected on every platform except the panel itself -- not an error.
      return;
    }

    try {
      final rewritten = _withDoorAddress(await file.readAsString(), doorIp.trim());
      await file.writeAsString(rewritten);
    } catch (e) {
      // ignore: avoid_print
      print('[DaemonConfigSync] failed to rewrite $configPath: $e');
      return;
    }

    try {
      final result = await Process.run('systemctl', ['restart', serviceName]);
      if (result.exitCode != 0) {
        // ignore: avoid_print
        print('[DaemonConfigSync] systemctl restart $serviceName failed: ${result.stderr}');
      }
    } catch (e) {
      // ignore: avoid_print
      print('[DaemonConfigSync] failed to restart $serviceName: $e');
    }
  }

  /// Replaces the `address` value inside the `[door]` section, leaving every
  /// other line (including comments and other sections) byte-for-byte
  /// unchanged. Appends a `[door]\naddress = ...` block if the file somehow
  /// has neither -- shouldn't happen against the shipped template, but a
  /// hand-edited config on-device is exactly the kind of file this should
  /// tolerate rather than corrupt.
  static String _withDoorAddress(String contents, String doorIp) {
    final lines = contents.split('\n');
    var inDoorSection = false;
    var sawDoorSection = false;
    var replaced = false;

    for (var i = 0; i < lines.length; i++) {
      final line = lines[i];
      final trimmed = line.trim();

      if (trimmed.startsWith('[')) {
        inDoorSection = trimmed == '[door]';
        if (inDoorSection) sawDoorSection = true;
        continue;
      }

      if (inDoorSection && !trimmed.startsWith('#') && trimmed.startsWith('address')) {
        lines[i] = 'address = $doorIp';
        replaced = true;
      }
    }

    if (replaced) return lines.join('\n');

    if (sawDoorSection) {
      // [door] section exists but had no `address` line (e.g. hand-edited
      // to remove it) -- insert one right after the section header.
      final headerIndex = lines.indexWhere((l) => l.trim() == '[door]');
      lines.insert(headerIndex + 1, 'address = $doorIp');
      return lines.join('\n');
    }

    // No [door] section at all -- append a fresh one.
    final buffer = StringBuffer(contents);
    if (!contents.endsWith('\n')) buffer.write('\n');
    buffer.write('\n[door]\naddress = $doorIp\n');
    return buffer.toString();
  }
}
