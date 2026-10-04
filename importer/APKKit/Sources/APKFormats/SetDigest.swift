import CryptoKit
import Foundation

/// A set's identity (ADR-0048, decision 6): its version folder and guest code path are named after it.
public enum SetDigest {
    /// The name Android gives an installed APK (`ApkLiteParseUtils.java:318-322`): the split name passed
    /// Android's name check, so it is a safe file name.
    public static func installedName(of splitName: String?) -> String {
        splitName.map { "split_\($0).apk" } ?? "base.apk"
    }

    /// The SHA-256, in hex, over the lines `<installed name> NUL <SHA-256 in hex> LF` of a set's APKs, sorted by their
    /// bytes. A NUL or LF inside a field would let two sets give the same lines, so it is refused.
    public static func of(_ members: [(splitName: String?, sha256: String)]) throws -> String {
        let lines = try members.map { member -> [UInt8] in
            let name = Array(installedName(of: member.splitName).utf8), sha256 = Array(member.sha256.utf8)
            guard !(name + sha256).contains(where: { $0 == 0 || $0 == 0x0A }) else { throw SetDigestError.separatorInField }
            return name + [0] + sha256 + [0x0A]
        }
        let text = Data(lines.sorted { $0.lexicographicallyPrecedes($1) }.joined())
        return SHA256.hash(data: text).map { String(format: "%02x", $0) }.joined()
    }
}

/// A set digest's field that holds one of its separators.
public enum SetDigestError: Error, Sendable, Equatable {
    case separatorInField
}
