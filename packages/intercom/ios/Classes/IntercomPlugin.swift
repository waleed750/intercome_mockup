import Flutter
import UIKit

public class IntercomPlugin: NSObject, FlutterPlugin {
  public static func register(with registrar: FlutterPluginRegistrar) {
    VideoDecoderHandler.register(with: registrar)
    AudioPipelineHandler.register(with: registrar)
  }
}
