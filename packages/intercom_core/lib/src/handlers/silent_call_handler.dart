import 'dart:async';

import '../call/incoming_call_handler.dart';

/// Implements [IncomingCallHandler] as a no-op.
///
/// Use this for embedded panels where the UI is always visible and no
/// notification or ringtone is needed.
final class SilentCallHandler implements IncomingCallHandler {
  @override
  Future<void> initialize() async {}

  @override
  Future<void> onIncomingCall({
    required String doorName,
    Map<String, dynamic>? data,
  }) async {}

  @override
  Future<void> onCallDismissed() async {}

  @override
  Future<void> dispose() async {}
}
