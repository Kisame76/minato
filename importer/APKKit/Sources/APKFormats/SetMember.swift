import APKKit
import Foundation

/// One APK of an import unit, as Android 15's installer reads it for a session (ADR-0048, decision 2):
/// `ApkLiteParseUtils.parseApkLite` at android-15.0.0_r1, which finds each attribute by namespace and name.
public struct SetMember: Sendable, Equatable {
    /// The name the APK came with, for the reasons.
    public var fileName: String
    public var packageName: String
    /// `versionCodeMajor` in the upper 32 bits, as the session compares it: the installer's reading by name, which
    /// `AndroidManifest` refuses where the package parser's by resource ID differs. Where obfuscation renamed one, the
    /// resource ID's reading stands, where Android's installer reads 0 (a known gap; an older version still asks).
    public var versionCode: Int64
    /// `<manifest android:versionName>` when it is text, nil for none or a reference: bundletool writes the app's
    /// versionCode there on the slices of fast-follow and on-demand packs, which carry no versionCode
    /// (`AssetSlicesGenerator.java:56-76`, 1.18.3; ADR-0049 f's fix).
    public var versionName: String? = nil
    /// nil for the base.
    public var splitName: String?
    public var configForSplit: String?
    public var isFeatureSplit: Bool
    /// `<manifest android:isolatedSplits>`, which Android takes from the base alone (`PackageLite.java:142`).
    public var isolatedSplits: Bool
    /// The first `<uses-split>`; Android ignores the others.
    public var usesSplit: String?
    public var requiredSplitTypes: Set<String>
    public var splitTypes: Set<String>
    /// `<manifest android:isSplitRequired>`, which Android 15 reads.
    public var splitRequired: Bool
    /// What bundletool wrote and Android 15 ignores: `<application android:isSplitRequired>` or the meta-data
    /// `com.android.vending.splits.required`. Minato reads them as "Needs split APKs".
    public var stricterSplitSignal: Bool
    public var targetSdk: Int
    /// The base's check uses its targetSdk, a split's none, so a split may be signed with v1 alone. Android checks
    /// the minimum scheme on the session's signer, which comes from the first APK it stages, in no fixed order
    /// (`PackageInstallerSession.java:1541, 3501-3503`); Minato's rule is that order with the base first.
    public var signature: SignatureVerification
    /// The SHA-256 of each signer's certificate: as the check proved them, in the APK's order and as many as its
    /// signers, when verified; as the APK names them, unproven but as many, when Minato cannot verify it
    /// (`APKSignatures.signerCertificates`); none when unsigned.
    public var certificates: [String]
    public var sha256: String
    /// The ABIs with a `lib/<abi>/lib*.so`, sorted.
    public var abis: [String]

    public var isBase: Bool { splitName == nil }

    public var role: SplitRole {
        guard let name = splitName else { return .base }
        if isFeatureSplit { return .feature(name) }
        if let module = configForSplit {
            let prefix = module.isEmpty ? "config." : module + ".config."
            return .config(module: module.isEmpty ? nil : module,
                           suffix: name.hasPrefix(prefix) ? String(name.dropFirst(prefix.count)) : name)
        }
        if name.hasPrefix("config.") { return .config(module: nil, suffix: String(name.dropFirst("config.".count))) }
        if let separator = name.range(of: ".config.") {
            return .config(module: String(name[..<separator.lowerBound]), suffix: String(name[separator.upperBound...]))
        }
        return .other(name)
    }

    /// Reads the APK of `archive` through `AndroidManifest`, which reads it as Android's installer does and refuses what
    /// Android's parser refuses, with its message, in its order (`ApkLiteParseUtils.java:406-640, 685-773`), and what
    /// it reads two ways (ADR-0048 b2).
    public static func read(_ archive: ZipArchive, fileName: String, sha256: String) throws -> SetMember {
        func refusal(_ reason: String) -> SetMemberError { SetMemberError(fileName: fileName, reason: reason) }
        let manifest: AndroidManifest
        do {
            // Within the analyzer's limit: a manifest's string pool is decoded whole.
            let xml = try archive.data(named: "AndroidManifest.xml", maxSize: 16 << 20)
            manifest = try AndroidManifest(xml: BinaryXML.parse(xml))
        } catch APKError.invalidManifest(let reason) {
            throw refusal(reason)
        } catch {
            throw refusal("Not a valid APK: \(error)")
        }
        // A split's levels only refuse it (:569-625); a base's are the analyzer's, as for one APK (ADR-0039).
        if manifest.splitName != nil, let sdkRefusal = manifest.sdkRefusal {
            throw refusal(sdkRefusal)
        }

        let abis = Set(archive.entries.compactMap { entry -> String? in
            let parts = entry.name.split(separator: "/", omittingEmptySubsequences: false)
            guard parts.count == 3, parts[0] == "lib", !parts[1].isEmpty,
                  parts[2].hasPrefix("lib"), parts[2].hasSuffix(".so") else { return nil }
            return String(parts[1])
        })

        let signature = SignatureVerifier.verify(archive, targetSdk: manifest.splitName == nil ? manifest.targetSdk : 0)
        let certificates: [String] = switch signature.check {
        case .verified: signature.proof?.certificates.map(SigningCertificate.sha256(of:)) ?? []
        case .unverified: APKSignatures.signerCertificates(archive)
        default: []
        }
        return SetMember(
            fileName: fileName, packageName: manifest.packageName, versionCode: manifest.versionCode,
            versionName: manifest.versionName.flatMap { value -> String? in
                if case .string(let name) = value { return name }
                return nil
            },
            splitName: manifest.splitName, configForSplit: manifest.configForSplit, isFeatureSplit: manifest.isFeatureSplit,
            isolatedSplits: manifest.isolatedSplits, usesSplit: manifest.usesSplit, requiredSplitTypes: manifest.requiredSplitTypes, splitTypes: manifest.splitTypes,
            splitRequired: manifest.splitRequired, stricterSplitSignal: manifest.stricterSplitSignal,
            targetSdk: manifest.targetSdk, signature: signature, certificates: certificates, sha256: sha256,
            abis: abis.sorted())
    }
}

/// How an APK takes part in its set (ADR-0048, decision 2), from its manifest, never its file name.
public enum SplitRole: Sendable, Equatable {
    case base
    case feature(String)
    /// A configuration split of the base (`module` nil) or of a feature module; `suffix` names its configuration.
    case config(module: String?, suffix: String)
    /// A split that is neither: Android loads it like any other.
    case other(String)
}

/// An APK that Android's parser refuses, with its message.
public struct SetMemberError: Error, Sendable, Equatable {
    public var fileName: String
    public var reason: String

    public init(fileName: String, reason: String) {
        self.fileName = fileName
        self.reason = reason
    }
}

/// A unit's APKs grouped by package and version code, in the order of each group's first APK (ADR-0048, decision 2).
/// Android's session takes one package and version; a unit may hold several apps or versions, each a set of its own.
public enum SetGrouping {
    public static func byPackageAndVersion(_ members: [SetMember]) -> [[SetMember]] {
        struct Key: Hashable {
            var packageName: String
            var versionCode: Int64
        }
        var groups: [[SetMember]] = []
        var index: [Key: Int] = [:]
        for member in members {
            let key = Key(packageName: member.packageName, versionCode: member.versionCode)
            if let position = index[key] {
                groups[position].append(member)
            } else {
                index[key] = groups.count
                groups.append([member])
            }
        }
        return groups
    }
}
