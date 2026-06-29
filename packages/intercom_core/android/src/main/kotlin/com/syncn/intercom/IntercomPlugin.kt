package com.syncn.intercom

import io.flutter.embedding.engine.plugins.FlutterPlugin

class IntercomPlugin : FlutterPlugin {
    override fun onAttachedToEngine(binding: FlutterPlugin.FlutterPluginBinding) {
        ForegroundServiceHandler.register(binding)
        VideoDecoderHandler.register(binding)
        AudioPipelineHandler.register(binding)
    }

    override fun onDetachedFromEngine(binding: FlutterPlugin.FlutterPluginBinding) = Unit
}
