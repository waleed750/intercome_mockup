import Flutter

final class AudioPipelineHandler: NSObject, FlutterPlugin, FlutterStreamHandler {
  static func register(with registrar: FlutterPluginRegistrar) {
    let methods = FlutterMethodChannel(name: "syncn_intercom/audio", binaryMessenger: registrar.messenger())
    let handler = AudioPipelineHandler()
    methods.setMethodCallHandler { call, result in
      switch call.method {
      case "start", "playDownlink", "setMuted", "stop":
        result(nil)
      default:
        result(FlutterMethodNotImplemented)
      }
    }
    let events = FlutterEventChannel(name: "syncn_intercom/audio_uplink", binaryMessenger: registrar.messenger())
    events.setStreamHandler(handler)
  }

  func onListen(withArguments arguments: Any?, eventSink events: @escaping FlutterEventSink) -> FlutterError? {
    return nil
  }

  func onCancel(withArguments arguments: Any?) -> FlutterError? {
    return nil
  }
}
