import Foundation

/// Errors raised while reading an APK. Parsers never trap on malformed input; they throw one of these.
public enum APKError: Error, Equatable, Sendable {
    case notAZipArchive
    case truncated(String)
    case unsupportedCompression(entry: String, method: UInt16)
    case encryptedEntry(String)
    case entryNotFound(String)
    case entryTooLarge(String, UInt64)
    case crcMismatch(String)
    case duplicateEntry(String)
    case invalidEntryName(String)
    case inconsistentEntry(String, String)
    case invalidBinaryXML(String)
    case invalidManifest(String)
    case invalidResourceTable(String)
    case invalidSigningBlock(String)
    case invalidCertificate(String)
    case invalidELF(String)
}
