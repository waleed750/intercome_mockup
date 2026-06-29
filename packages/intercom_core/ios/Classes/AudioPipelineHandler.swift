import Flutter
import AVFoundation

/// Two-way G.711 A-law audio at 8 kHz mono using AVAudioEngine.
/// Mirrors the Android AudioPipelineHandler.
final class AudioPipelineHandler: NSObject, FlutterMethodCallHandler, FlutterStreamHandler {

    // MARK: - Constants
    private static let methodChannel = "syncn_intercom/audio"
    private static let eventChannel  = "syncn_intercom/audio_uplink"
    private static let sampleRate: Double = 8000.0
    private static let alawFrame  = 160       // 20 ms at 8 kHz
    private static let silenceByte: UInt8 = 0xD5

    // MARK: - State
    private var engine: AVAudioEngine?
    private var playerNode: AVAudioPlayerNode?
    private var eventSink: FlutterEventSink?
    private var muted = false
    private var running = false
    private let lock = NSLock()

    /// The PCM format used for playback and capture: 8 kHz, mono, 16-bit signed int.
    private let pcmFormat = AVAudioFormat(
        commonFormat: .pcmFormatInt16,
        sampleRate: 8000.0,
        channels: 1,
        interleaved: true
    )!

    // MARK: - Registration

    static func register(with registrar: FlutterPluginRegistrar) {
        let handler = AudioPipelineHandler()
        let method = FlutterMethodChannel(
            name: methodChannel,
            binaryMessenger: registrar.messenger()
        )
        method.setMethodCallHandler(handler.handle)

        let event = FlutterEventChannel(
            name: eventChannel,
            binaryMessenger: registrar.messenger()
        )
        event.setStreamHandler(handler)
    }

    // MARK: - FlutterMethodCallHandler

    func handle(_ call: FlutterMethodCall, result: @escaping FlutterResult) {
        switch call.method {
        case "start":
            let args = call.arguments as? [String: Any]
            let captureEnabled = args?["captureEnabled"] as? Bool ?? false
            do {
                try start(captureEnabled: captureEnabled)
                result(nil)
            } catch {
                result(FlutterError(code: "audio_start_failed",
                                    message: error.localizedDescription,
                                    details: nil))
            }
        case "playDownlink":
            if let data = call.arguments as? FlutterStandardTypedData {
                playDownlink(data.data)
            }
            result(nil)
        case "setMuted":
            if let value = call.arguments as? Bool {
                muted = value
            }
            result(nil)
        case "stop":
            stop()
            result(nil)
        default:
            result(FlutterMethodNotImplemented)
        }
    }

    // MARK: - FlutterStreamHandler

    func onListen(withArguments arguments: Any?, eventSink events: @escaping FlutterEventSink) -> FlutterError? {
        self.eventSink = events
        return nil
    }

    func onCancel(withArguments arguments: Any?) -> FlutterError? {
        self.eventSink = nil
        return nil
    }

    // MARK: - Lifecycle

    private func start(captureEnabled: Bool) throws {
        if running { return }

        // Configure audio session for voice chat (built-in AEC)
        let session = AVAudioSession.sharedInstance()
        try session.setCategory(.playAndRecord, mode: .voiceChat, options: [.defaultToSpeaker])
        try session.setPreferredSampleRate(AudioPipelineHandler.sampleRate)
        try session.setPreferredIOBufferDuration(0.02) // 20 ms
        try session.setActive(true)

        let audioEngine = AVAudioEngine()
        let player = AVAudioPlayerNode()
        audioEngine.attach(player)

        // Connect player -> mainMixer at the PCM format
        audioEngine.connect(player, to: audioEngine.mainMixerNode, format: pcmFormat)

        // Install mic tap if capture is enabled
        if captureEnabled {
            installInputTap(on: audioEngine)
        }

        try audioEngine.start()
        player.play()

        engine = audioEngine
        playerNode = player
        running = true
    }

    private func stop() {
        guard running else { return }
        running = false

        engine?.inputNode.removeTap(onBus: 0)
        playerNode?.stop()
        engine?.stop()

        playerNode = nil
        engine = nil

        // Deactivate audio session
        try? AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation)
    }

    // MARK: - Uplink (mic capture)

    private func installInputTap(on engine: AVAudioEngine) {
        let inputNode = engine.inputNode
        let inputFormat = inputNode.outputFormat(forBus: 0)

        // We need to convert from the hardware sample rate to 8 kHz mono Int16
        guard let converter = AVAudioConverter(from: inputFormat, to: pcmFormat) else { return }

        let alawFrame = AudioPipelineHandler.alawFrame
        let silenceByte = AudioPipelineHandler.silenceByte

        // Tap the input at hardware format, convert in the callback
        let tapBufferSize: AVAudioFrameCount = AVAudioFrameCount(inputFormat.sampleRate * 0.02) // 20 ms
        inputNode.installTap(onBus: 0, bufferSize: tapBufferSize, format: inputFormat) {
            [weak self] (buffer, _) in
            guard let self = self, self.running else { return }

            // Convert to 8 kHz mono Int16
            let frameCapacity = AVAudioFrameCount(alawFrame) // 160 samples = 20 ms at 8 kHz
            guard let convertedBuffer = AVAudioPCMBuffer(pcmFormat: self.pcmFormat, frameCapacity: frameCapacity) else { return }

            var error: NSError?
            let inputBlock: AVAudioConverterInputBlock = { _, outStatus in
                outStatus.pointee = .haveData
                return buffer
            }
            converter.convert(to: convertedBuffer, error: &error, withInputFrom: inputBlock)

            guard error == nil, convertedBuffer.frameLength > 0 else { return }

            // Extract Int16 samples
            guard let int16Data = convertedBuffer.int16ChannelData else { return }
            let frameCount = Int(convertedBuffer.frameLength)

            let alawBytes: Data
            if self.muted {
                alawBytes = Data(repeating: silenceByte, count: frameCount)
            } else {
                // Encode PCM to A-law
                var encoded = Data(count: frameCount)
                for i in 0..<frameCount {
                    encoded[i] = ALawCodec.encodeSample(Int32(int16Data[0][i]))
                }
                alawBytes = encoded
            }

            // Send to Flutter on main thread
            DispatchQueue.main.async {
                self.eventSink?(FlutterStandardTypedData(bytes: alawBytes))
            }
        }
    }

    // MARK: - Downlink (speaker playback)

    private func playDownlink(_ alawData: Data) {
        guard running, let player = playerNode, !alawData.isEmpty else { return }

        let frameCount = alawData.count
        guard let buffer = AVAudioPCMBuffer(pcmFormat: pcmFormat, frameCapacity: AVAudioFrameCount(frameCount)) else { return }
        buffer.frameLength = AVAudioFrameCount(frameCount)

        guard let channelData = buffer.int16ChannelData else { return }

        // Decode A-law to PCM Int16
        for i in 0..<frameCount {
            channelData[0][i] = ALawCodec.decodeSample(alawData[i])
        }

        player.scheduleBuffer(buffer, completionHandler: nil)
    }
}

// MARK: - G.711 A-law Codec

/// ITU-T G.711 A-law companding. Pure Swift implementation matching the
/// Kotlin/Dart ALawCodec exactly (same constants, same algorithm).
enum ALawCodec {
    private static let signBit:   Int32 = 0x80
    private static let quantMask: Int32 = 0x0F
    private static let segShift:  Int32 = 4
    private static let segMask:   Int32 = 0x70
    private static let segEnd: [Int32] = [0x1F, 0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF]

    /// Pre-computed decode table for all 256 A-law values.
    private static let decodeTable: [Int16] = {
        var table = [Int16](repeating: 0, count: 256)
        for i in 0..<256 {
            table[i] = decodeSampleInternal(UInt8(i))
        }
        return table
    }()

    // MARK: - Public API

    /// Encode a single 16-bit PCM sample to an A-law byte.
    static func encodeSample(_ pcm16: Int32) -> UInt8 {
        var pcm = pcm16 >> 3
        let mask: Int32
        if pcm >= 0 {
            mask = 0xD5
        } else {
            mask = 0x55
            pcm = -pcm - 1
        }
        let seg = search(pcm)
        let aval: Int32
        if seg >= 8 {
            aval = 0x7F
        } else {
            let base = seg << segShift
            let quant: Int32
            if seg < 2 {
                quant = (pcm >> 1) & quantMask
            } else {
                quant = (pcm >> seg) & quantMask
            }
            aval = base | quant
        }
        return UInt8(truncatingIfNeeded: aval ^ mask)
    }

    /// Decode a single A-law byte to a 16-bit PCM sample.
    static func decodeSample(_ alaw: UInt8) -> Int16 {
        return decodeTable[Int(alaw)]
    }

    /// Decode a buffer of A-law bytes to PCM Int16 pairs (little-endian byte array).
    static func decode(_ alaw: Data) -> Data {
        var out = Data(count: alaw.count * 2)
        for i in 0..<alaw.count {
            let sample = decodeTable[Int(alaw[i])]
            let offset = i * 2
            out[offset]     = UInt8(truncatingIfNeeded: sample)
            out[offset + 1] = UInt8(truncatingIfNeeded: sample >> 8)
        }
        return out
    }

    /// Encode a PCM byte array (little-endian Int16 pairs) to A-law bytes.
    static func encode(_ pcm: Data) -> Data {
        let count = pcm.count / 2
        var out = Data(count: count)
        var i = 0
        for n in 0..<count {
            let lo = Int32(pcm[i]) & 0xFF
            let hi = Int32(Int8(bitPattern: pcm[i + 1]))
            i += 2
            out[n] = encodeSample((hi << 8) | lo)
        }
        return out
    }

    // MARK: - Internal

    private static func decodeSampleInternal(_ alawByte: UInt8) -> Int16 {
        let a = Int32(alawByte) ^ 0x55
        var t = (a & quantMask) << 4
        let seg = (a & segMask) >> segShift
        switch seg {
        case 0:
            t += 8
        case 1:
            t += 0x108
        default:
            t += 0x108
            t = t << (seg - 1)
        }
        return (a & signBit) != 0 ? Int16(t) : Int16(-t)
    }

    private static func search(_ value: Int32) -> Int32 {
        for i in 0..<segEnd.count {
            if value <= segEnd[i] { return Int32(i) }
        }
        return Int32(segEnd.count)
    }
}
