import Foundation

/// Random-access byte input. APKs can be several gigabytes, so parsers read ranges instead of whole files.
public protocol ByteSource {
    var size: UInt64 { get }
    func read(at offset: UInt64, count: Int) throws -> Data
}

/// In-memory source, used for nested content and tests.
public struct DataSource: ByteSource {
    public let data: Data

    public init(_ data: Data) {
        self.data = data
    }

    public var size: UInt64 { UInt64(data.count) }

    public func read(at offset: UInt64, count: Int) throws -> Data {
        guard count >= 0, offset <= size, UInt64(count) <= size - offset else {
            throw APKError.truncated("read of \(count) bytes at \(offset) beyond \(size)")
        }
        let start = data.startIndex + Int(offset)
        return data.subdata(in: start..<start + count)
    }
}

/// File-backed source. Not thread-safe: one instance per reading task.
public final class FileSource: ByteSource {
    private let handle: FileHandle
    public let size: UInt64

    public init(url: URL) throws {
        handle = try FileHandle(forReadingFrom: url)
        size = try handle.seekToEnd()
    }

    deinit {
        try? handle.close()
    }

    public func read(at offset: UInt64, count: Int) throws -> Data {
        guard count >= 0, offset <= size, UInt64(count) <= size - offset else {
            throw APKError.truncated("read of \(count) bytes at \(offset) beyond \(size)")
        }
        try handle.seek(toOffset: offset)
        // FileHandle hands out autoreleased buffers: without a pool per read, a loop over a large file
        // would keep all of them until its thread's pool drains, as much memory as the file.
        let data = try autoreleasepool { try handle.read(upToCount: count) ?? Data() }
        guard data.count == count else {
            throw APKError.truncated("short read at \(offset)")
        }
        return data
    }
}
