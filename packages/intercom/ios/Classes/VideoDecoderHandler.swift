import Flutter

final class VideoDecoderHandler: NSObject, FlutterPlugin {
  static func register(with registrar: FlutterPluginRegistrar) {
    let channel = FlutterMethodChannel(name: "syncn_intercom/video", binaryMessenger: registrar.messenger())
    channel.setMethodCallHandler { call, result in
      switch call.method {
      case "start":
        result(-1)
      case "submit", "stop":
        result(nil)
      default:
        result(FlutterMethodNotImplemented)
      }
    }
  }
}
