import APKRuntime
import AVFAudio
import Synchronization

/// The input node's tap for the guest's input streams (ADR-0064, decision 4): each buffer converted to every stream's
/// rate and channels, interleaved float32, into the runtime's ring with apkrt_audio_input_write, which never blocks
/// and takes nothing from a stream the guest has not started. Mono is put on both sides of a stereo stream, and a
/// stereo input is mixed down for a mono one. A converter is made for each stream once its first buffer comes, and
/// again when the input's format changes (a new route). The tap runs on a thread of AVAudioEngine's, the streams' table
/// is shared with the host's queue under a lock.
final class MicrophoneTap: Sendable {
    private final class Stream: @unchecked Sendable {
        let format: AVAudioFormat
        var converter: AVAudioConverter?

        init(format: AVAudioFormat) {
            self.format = format
        }
    }

    private let streams = Mutex<[UInt64: Stream]>([:])

    func has(_ stream: UInt64) -> Bool {
        streams.withLock { $0[stream] != nil }
    }

    func add(_ stream: UInt64, format: AVAudioFormat) {
        streams.withLock { $0[stream] = Stream(format: format) }
    }

    /// Whether `stream` was there.
    func remove(_ stream: UInt64) -> Bool {
        streams.withLock { $0.removeValue(forKey: stream) != nil }
    }

    /// The tap's buffer, whose first frame is at `hostTime`.
    func captured(_ buffer: AVAudioPCMBuffer, hostTime: UInt64) {
        let open = streams.withLock { Array($0) }
        for (id, stream) in open {
            guard let converted = Self.convert(buffer, for: stream), let data = converted.floatChannelData
            else { continue }
            _ = apkrt_audio_input_write(id, data[0], Int64(converted.frameLength), hostTime)
        }
    }

    private static func convert(_ buffer: AVAudioPCMBuffer, for stream: Stream) -> AVAudioPCMBuffer? {
        let input = buffer.format, output = stream.format
        if stream.converter?.inputFormat != input {
            let converter = AVAudioConverter(from: input, to: output)
            if input.channelCount == 1, output.channelCount == 2 { converter?.channelMap = [0, 0] }
            converter?.downmix = input.channelCount > output.channelCount
            stream.converter = converter
        }
        guard let converter = stream.converter else { return nil }
        let frames = AVAudioFrameCount((Double(buffer.frameLength) * output.sampleRate / input.sampleRate).rounded(.up))
        guard let converted = AVAudioPCMBuffer(pcmFormat: output, frameCapacity: frames + 32) else { return nil }
        var given = false
        var error: NSError?
        let status = converter.convert(to: converted, error: &error) { _, inputStatus in
            if given {
                inputStatus.pointee = .noDataNow
                return nil
            }
            given = true
            inputStatus.pointee = .haveData
            return buffer
        }
        return status == .error || converted.frameLength == 0 ? nil : converted
    }
}
