import Flutter
import VideoToolbox
import CoreMedia
import CoreVideo

/// Decodes H.264 Annex-B NAL units via VideoToolbox and exposes frames as a
/// FlutterTexture (CVPixelBuffer). Mirrors the Android VideoDecoderHandler.
final class VideoDecoderHandler: NSObject, FlutterMethodCallHandler, FlutterTexture {

    // MARK: - Constants
    private static let channelName = "syncn_intercom/video"

    // MARK: - State
    private var textureRegistry: FlutterTextureRegistry?
    private var textureId: Int64 = -1
    private var session: VTDecompressionSession?
    private var formatDescription: CMVideoFormatDescription?

    private let lock = NSLock()
    private var latestPixelBuffer: CVPixelBuffer?

    private var currentSPS: Data?
    private var currentPPS: Data?
    private var running = false

    // MARK: - Registration
    static func register(with registrar: FlutterPluginRegistrar) {
        let channel = FlutterMethodChannel(
            name: channelName,
            binaryMessenger: registrar.messenger()
        )
        let handler = VideoDecoderHandler()
        handler.textureRegistry = registrar.textures()
        channel.setMethodCallHandler(handler.handle)
    }

    // MARK: - FlutterMethodCallHandler
    func handle(_ call: FlutterMethodCall, result: @escaping FlutterResult) {
        switch call.method {
        case "start":
            do {
                let id = try start()
                result(id)
            } catch {
                result(FlutterError(code: "video_start_failed",
                                    message: error.localizedDescription,
                                    details: nil))
            }
        case "submit":
            if let data = call.arguments as? FlutterStandardTypedData {
                submit(data.data)
            }
            result(nil)
        case "stop":
            stop()
            result(nil)
        default:
            result(FlutterMethodNotImplemented)
        }
    }

    // MARK: - FlutterTexture
    func copyPixelBuffer() -> Unmanaged<CVPixelBuffer>? {
        lock.lock()
        defer { lock.unlock() }
        guard let pb = latestPixelBuffer else { return nil }
        return Unmanaged.passRetained(pb)
    }

    // MARK: - Lifecycle

    private func start() throws -> Int64 {
        if running { return textureId }
        guard let registry = textureRegistry else { throw DecoderError.noTextureRegistry }

        textureId = registry.register(self)
        running = true
        currentSPS = nil
        currentPPS = nil
        formatDescription = nil
        session = nil
        return textureId
    }

    private func stop() {
        running = false
        if let s = session {
            VTDecompressionSessionInvalidate(s)
            session = nil
        }
        formatDescription = nil
        currentSPS = nil
        currentPPS = nil

        lock.lock()
        latestPixelBuffer = nil
        lock.unlock()

        if textureId >= 0 {
            textureRegistry?.unregisterTexture(textureId)
            textureId = -1
        }
    }

    // MARK: - NAL unit submission

    private func submit(_ annexB: Data) {
        guard running, !annexB.isEmpty else { return }
        let nalUnits = parseAnnexB(annexB)
        for nal in nalUnits {
            processNALUnit(nal)
        }
    }

    /// Parse Annex-B byte stream into individual NAL units (without start codes).
    private func parseAnnexB(_ data: Data) -> [Data] {
        var units = [Data]()
        let bytes = [UInt8](data)
        let count = bytes.count
        var i = 0

        // Find first start code
        func findStartCode(from pos: Int) -> (offset: Int, length: Int)? {
            var j = pos
            while j < count - 2 {
                if bytes[j] == 0x00 && bytes[j + 1] == 0x00 {
                    if j + 2 < count && bytes[j + 2] == 0x01 {
                        return (j, 3)
                    }
                    if j + 3 < count && bytes[j + 2] == 0x00 && bytes[j + 3] == 0x01 {
                        return (j, 4)
                    }
                }
                j += 1
            }
            return nil
        }

        guard let first = findStartCode(from: 0) else {
            // No start codes — treat entire data as single NAL
            if !data.isEmpty { units.append(data) }
            return units
        }

        i = first.offset + first.length

        while i < count {
            if let next = findStartCode(from: i) {
                let nalData = Data(bytes[i..<next.offset])
                if !nalData.isEmpty { units.append(nalData) }
                i = next.offset + next.length
            } else {
                // Last NAL: from current position to end
                let nalData = Data(bytes[i..<count])
                if !nalData.isEmpty { units.append(nalData) }
                break
            }
        }
        return units
    }

    private func processNALUnit(_ nal: Data) {
        guard !nal.isEmpty else { return }
        let nalType = nal[0] & 0x1F

        switch nalType {
        case 7: // SPS
            if currentSPS != nal {
                currentSPS = nal
                rebuildFormatDescription()
            }
        case 8: // PPS
            if currentPPS != nal {
                currentPPS = nal
                rebuildFormatDescription()
            }
        case 1, 5: // Non-IDR slice, IDR slice
            decodeNALUnit(nal)
        default:
            // Other NAL types (SEI, AUD, etc.) — attempt decode
            decodeNALUnit(nal)
        }
    }

    // MARK: - Format description & session

    private func rebuildFormatDescription() {
        guard let sps = currentSPS, let pps = currentPPS else { return }

        // Invalidate old session
        if let s = session {
            VTDecompressionSessionInvalidate(s)
            session = nil
        }
        formatDescription = nil

        let spsBytes = [UInt8](sps)
        let ppsBytes = [UInt8](pps)
        let parameterSets: [UnsafePointer<UInt8>] = [
            spsBytes.withUnsafeBufferPointer { $0.baseAddress! },
            ppsBytes.withUnsafeBufferPointer { $0.baseAddress! },
        ]
        // We need stable pointers for the C call
        var desc: CMFormatDescription?
        let status = spsBytes.withUnsafeBufferPointer { spsBuf in
            ppsBytes.withUnsafeBufferPointer { ppsBuf in
                var ptrs: [UnsafePointer<UInt8>] = [spsBuf.baseAddress!, ppsBuf.baseAddress!]
                var sizes: [Int] = [spsBytes.count, ppsBytes.count]
                return CMVideoFormatDescriptionCreateFromH264ParameterSets(
                    allocator: kCFAllocatorDefault,
                    parameterSetCount: 2,
                    parameterSetPointers: &ptrs,
                    parameterSetSizes: &sizes,
                    nalUnitHeaderLength: 4,
                    formatDescriptionOut: &desc
                )
            }
        }

        guard status == noErr, let newDesc = desc else { return }
        formatDescription = newDesc
        createDecompressionSession(formatDescription: newDesc)
    }

    private func createDecompressionSession(formatDescription: CMVideoFormatDescription) {
        let attrs: [NSString: Any] = [
            kCVPixelBufferPixelFormatTypeKey: kCVPixelFormatType_32BGRA,
            kCVPixelBufferIOSurfacePropertiesKey: [:] as NSDictionary,
        ]

        var callbackRecord = VTDecompressionOutputCallbackRecord(
            decompressionOutputCallback: decompressionCallback,
            decompressionOutputRefCon: Unmanaged.passUnretained(self).toOpaque()
        )

        var newSession: VTDecompressionSession?
        let status = VTDecompressionSessionCreate(
            allocator: kCFAllocatorDefault,
            formatDescription: formatDescription,
            decoderSpecification: nil,
            imageBufferAttributes: attrs as CFDictionary,
            outputCallback: &callbackRecord,
            decompressionSessionOut: &newSession
        )

        if status == noErr {
            session = newSession
        }
    }

    // MARK: - Decoding

    private func decodeNALUnit(_ nal: Data) {
        guard let currentSession = session, let _ = formatDescription else { return }

        // Convert Annex-B NAL to AVCC format (4-byte length prefix)
        var nalBytes = [UInt8](nal)
        let nalLength = UInt32(nalBytes.count)
        var lengthBE = nalLength.bigEndian

        var blockBuffer: CMBlockBuffer?
        let totalLength = 4 + nalBytes.count
        var status = CMBlockBufferCreateWithMemoryBlock(
            allocator: kCFAllocatorDefault,
            memoryBlock: nil,
            blockLength: totalLength,
            blockAllocator: kCFAllocatorDefault,
            customBlockSource: nil,
            offsetToData: 0,
            dataLength: totalLength,
            flags: 0,
            blockBufferOut: &blockBuffer
        )
        guard status == kCMBlockBufferNoErr, let buffer = blockBuffer else { return }

        // Copy length prefix
        status = CMBlockBufferReplaceDataBytes(
            with: &lengthBE,
            blockBuffer: buffer,
            offsetIntoDestination: 0,
            dataLength: 4
        )
        guard status == kCMBlockBufferNoErr else { return }

        // Copy NAL data
        status = nalBytes.withUnsafeMutableBufferPointer { ptr in
            CMBlockBufferReplaceDataBytes(
                with: ptr.baseAddress!,
                blockBuffer: buffer,
                offsetIntoDestination: 4,
                dataLength: nalBytes.count
            )
        }
        guard status == kCMBlockBufferNoErr else { return }

        var sampleBuffer: CMSampleBuffer?
        var sampleSize = totalLength
        status = CMSampleBufferCreateReady(
            allocator: kCFAllocatorDefault,
            dataBuffer: buffer,
            formatDescription: formatDescription,
            sampleCount: 1,
            sampleTimingEntryCount: 0,
            sampleTimingArray: nil,
            sampleSizeEntryCount: 1,
            sampleSizeArray: &sampleSize,
            sampleBufferOut: &sampleBuffer
        )
        guard status == noErr, let sample = sampleBuffer else { return }

        let decodeFlags: VTDecodeFrameFlags = [._EnableAsynchronousDecompression]
        var infoFlags = VTDecodeInfoFlags()
        VTDecompressionSessionDecodeFrame(
            currentSession,
            sampleBuffer: sample,
            flags: decodeFlags,
            frameRefcon: nil,
            infoFlagsOut: &infoFlags
        )
    }

    // MARK: - Decompression callback

    private func storePixelBuffer(_ pixelBuffer: CVPixelBuffer) {
        lock.lock()
        latestPixelBuffer = pixelBuffer
        lock.unlock()
        if let registry = textureRegistry, textureId >= 0 {
            registry.textureFrameAvailable(textureId)
        }
    }

    // MARK: - Error type
    private enum DecoderError: Error {
        case noTextureRegistry
    }
}

// MARK: - VTDecompressionOutputCallback (C function pointer)

private func decompressionCallback(
    decompressionOutputRefCon: UnsafeMutableRawPointer?,
    sourceFrameRefCon: UnsafeMutableRawPointer?,
    status: OSStatus,
    infoFlags: VTDecodeInfoFlags,
    imageBuffer: CVImageBuffer?,
    presentationTimeStamp: CMTime,
    presentationDuration: CMTime
) {
    guard status == noErr,
          let refCon = decompressionOutputRefCon,
          let pixelBuffer = imageBuffer else { return }

    let handler = Unmanaged<VideoDecoderHandler>.fromOpaque(refCon).takeUnretainedValue()
    handler.storePixelBuffer(pixelBuffer)
}
