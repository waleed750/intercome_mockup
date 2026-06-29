abstract interface class ConnectionProvider {
  Future<bool> start();
  Future<void> stop();
  bool get isActive;
}
