abstract interface class IncomingCallHandler {
  Future<void> initialize();

  Future<void> onIncomingCall({
    required String doorName,
    Map<String, dynamic>? data,
  });

  Future<void> onCallDismissed();

  Future<void> dispose();
}
