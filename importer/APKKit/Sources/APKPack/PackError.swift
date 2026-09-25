import Foundation

/// Why make-apk cannot pack: the file it concerns and, in the manifest, the line (ADR-0031).
public struct PackError: Error, Equatable, Sendable, CustomStringConvertible {
    public var file: String
    public var line: Int?
    public var message: String

    public init(file: String, line: Int? = nil, message: String) {
        self.file = file
        self.line = line
        self.message = message
    }

    public var description: String {
        line.map { "\(file):\($0): \(message)" } ?? "\(file): \(message)"
    }
}
