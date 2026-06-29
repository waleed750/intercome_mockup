import Flutter
import UIKit

public class IntercomPlugin: NSObject, FlutterPlugin {
    public static func register(with registrar: FlutterPluginRegistrar) {
        VideoDecoderHandler.register(with: registrar)
        AudioPipelineHandler.register(with: registrar)

        // Foreground service — iOS no-op shim
        let fgChannel = FlutterMethodChannel(
            name: "syncn_intercom/foreground_service",
            binaryMessenger: registrar.messenger()
        )
        let instance = IntercomPlugin()
        fgChannel.setMethodCallHandler(instance.handleForegroundService)
    }

    private func handleForegroundService(call: FlutterMethodCall, result: @escaping FlutterResult) {
        switch call.method {
        case "startPanelService", "stopPanelService":
            // No foreground service concept on iOS — succeed silently
            result(nil)
        case "takePendingIncomingCall":
            // iOS uses PushKit / CallKit instead — always return false
            result(false)
        default:
            result(FlutterMethodNotImplemented)
        }
    }
}
