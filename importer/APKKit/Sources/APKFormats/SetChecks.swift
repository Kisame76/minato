import APKKit
import Foundation

/// Who signed a set (ADR-0048, decision 3).
public enum SetSigning: Sendable, Equatable {
    /// Every APK verified, each split exactly by its base's signer.
    case verified(SignerIdentity)
    /// No APK signed: Minato asks, as for one unsigned APK (ADR-0034), where Android refuses.
    case unsigned
    /// Every APK of an algorithm Minato cannot check: the set is treated as one such APK (ADR-0034).
    case unverified(String)
    /// Some APKs verified, the others not: Minato cannot check those files' content, so the set asks even on a fresh
    /// install and names them (the lead's decision); the signer is the verified APKs'.
    case partlyVerified(SignerIdentity, unverifiable: [UnverifiableFile])
}

/// An APK of a set whose signature Minato cannot check, and why.
public struct UnverifiableFile: Sendable, Equatable {
    public var fileName: String
    public var reason: String

    public init(fileName: String, reason: String) {
        self.fileName = fileName
        self.reason = reason
    }
}

/// A set that passed Android's checks.
public struct CheckedSet: Sendable, Equatable {
    /// nil when the APKs are splits only: they may only join the installed version (ADR-0048, decision 7).
    public var base: SetMember?
    /// In split-name order, as Android's `PackageLite` keeps them (`ApkLiteParseUtils.java:287-288`).
    public var splits: [SetMember]
    public var signing: SetSigning
    /// Android 15 would refuse the set with INSTALL_FAILED_MISSING_SPLIT; only with a base.
    public var missingSplit: Bool
    /// "Needs split APKs": a missing split, or bundletool's older signals with no split at all (F1).
    public var needsSplits: Bool
}

/// Why a set cannot be installed, in Android's words where Android has them.
public struct SetRefusal: Error, Sendable, Equatable {
    public var reason: String
}

/// Android 15's checks of one install session (`PackageInstallerSession.validateApkInstallLocked`,
/// android-15.0.0_r1), over the APKs of one package (ADR-0048, decision 3).
public enum SetChecks {
    public static func check(_ members: [SetMember]) -> Result<CheckedSet, SetRefusal> {
        guard !members.isEmpty else { return .failure(SetRefusal(reason: "No packages staged")) }
        // Every APK's certificates are collected first; a damaged or forged one ends the session (:1604-1622).
        for member in members {
            if case .failed(let failure) = member.signature.check {
                return .failure(SetRefusal(reason: "\(member.fileName): \(failure.detail)"))
            }
        }
        let splits = members.filter { !$0.isBase }.sorted { $0.splitName! < $1.splitName! }
        // The first APK defines the package, version and signer the others must match (:3481-3513); the base
        // comes first, so the reasons name the splits.
        let ordered = members.filter(\.isBase) + splits
        let first = ordered[0]
        var seen: Set<String?> = []
        // APFS tells file names apart without their case, Android's ext4 with it (the lead's decision).
        var folded: [String: String] = [:]
        for member in ordered {
            if !seen.insert(member.splitName).inserted {
                return .failure(SetRefusal(reason: "Split \(member.splitName ?? "base") was defined multiple times"))
            }
            if let name = member.splitName {
                if let other = folded[name.lowercased()] {
                    return .failure(SetRefusal(reason: "Splits \(other) and \(name) differ only in case"))
                }
                folded[name.lowercased()] = name
            }
            if member.packageName != first.packageName {
                return .failure(SetRefusal(
                    reason: "\(member.fileName) package \(member.packageName) inconsistent with \(first.packageName)"))
            }
            if member.versionCode != first.versionCode {
                return .failure(SetRefusal(
                    reason: "\(member.fileName) version code \(member.versionCode) inconsistent with \(first.versionCode)"))
            }
            if !sameSigner(member, first) {
                return .failure(SetRefusal(reason: "\(member.fileName) signatures are inconsistent"))
            }
            let installedName = SetDigest.installedName(of: member.splitName)
            if !InstallerReading.isValidExtFilename(installedName) {
                return .failure(SetRefusal(reason: "Invalid filename: \(installedName)"))
            }
        }

        let signing = signing(of: ordered)
        guard first.isBase else {
            return .success(CheckedSet(base: nil, splits: splits, signing: signing, missingSplit: false, needsSplits: false))
        }
        let missingSplit = missesSplit(base: first, splits: splits)
        // The session refuses a missing split before the package parser reads the dependencies.
        if !missingSplit, first.isolatedSplits, let error = dependencyError(splits: splits) {
            return .failure(SetRefusal(reason: error))
        }
        return .success(CheckedSet(base: first, splits: splits, signing: signing, missingSplit: missingSplit,
                                   needsSplits: needsSplits(base: first, splits: splits)))
    }

    /// INSTALL_PARSE_FAILED_BAD_MANIFEST of a base with isolatedSplits, whose splits' dependency tree Android builds
    /// (`ParsingPackageUtils.java:358-364`, `SplitDependencyLoader.createDependenciesFromPackage`, :161-249): a feature
    /// split's `<uses-split>` names a split of the set, a configuration split's `configForSplit` a feature split of it,
    /// and following each split's `<uses-split>` ends at the base. `splits` in split-name order, as Android goes
    /// through them; its words, or nil.
    static func dependencyError(splits: [SetMember]) -> String? {
        let byName = Dictionary(uniqueKeysWithValues: splits.map { ($0.splitName!, $0) })
        // Each feature split's dependency: another split, or the base (nil).
        var dependencies: [String: String?] = [:]
        for split in splits where split.isFeatureSplit {
            if let dependency = split.usesSplit, byName[dependency] == nil {
                return "Split '\(split.splitName!)' requires split '\(dependency)', which is missing."
            }
            dependencies[split.splitName!] = .some(split.usesSplit)
        }
        for split in splits where !split.isFeatureSplit {
            guard let target = split.configForSplit else { continue }
            guard let configured = byName[target] else {
                return "Split '\(split.splitName!)' targets split '\(target)', which is missing."
            }
            if !configured.isFeatureSplit {
                return "Split '\(split.splitName!)' declares itself as configuration split for a non-feature split '\(target)'"
            }
        }
        // A configuration split, which has no dependency of its own, ends the walk as the base does.
        for start in dependencies.keys {
            var seen: Set<String> = []
            var next: String? = start
            while let name = next {
                guard seen.insert(name).inserted else { return "Cycle detected in split dependencies." }
                next = dependencies[name] ?? nil
            }
        }
        return nil
    }

    /// Splits without their base on the installed version, as Android's inherit mode stages them
    /// (`PackageInstallerSession.java:3433-3437, 3678-3718`; ADR-0048, decision 7): its base and splits with each of
    /// `splits` added, or in place of the one of its name, to be checked as a set again. nil where they cannot join it:
    /// no installed base, or a split of another package, version code or signer than its base's ("Missing existing base
    /// package", "Existing … inconsistent with …", "Existing signatures are inconsistent").
    public static func joining(_ splits: [SetMember], installed: [SetMember]) -> [SetMember]? {
        guard !splits.isEmpty, let base = installed.first(where: \.isBase), splits.allSatisfy({ split in
            split.packageName == base.packageName && split.versionCode == base.versionCode && sameSigner(split, base)
        }) else { return nil }
        let names = Set(splits.map(\.splitName))
        return [base] + installed.filter { !$0.isBase && !names.contains($0.splitName) } + splits
    }

    /// "Needs split APKs": a missing split, or bundletool's older signals with no split at all (F1).
    static func needsSplits(base: SetMember, splits: [SetMember]) -> Bool {
        missesSplit(base: base, splits: splits) || base.stricterSplitSignal && splits.isEmpty
    }

    /// INSTALL_FAILED_MISSING_SPLIT (:3540-3542, 3652-3656): when the base requires splits, at least one split, and
    /// the required split types of every APK given by their split types.
    static func missesSplit(base: SetMember, splits: [SetMember]) -> Bool {
        let requiresSplits = base.splitRequired || !base.requiredSplitTypes.isEmpty
        return requiresSplits && (splits.isEmpty || !missingSplitTypes(base: base, splits: splits).isEmpty)
    }

    /// The required split types of the APKs that none of them gives, sorted.
    static func missingSplitTypes(base: SetMember, splits: [SetMember]) -> [String] {
        let all = [base] + splits
        let required = all.reduce(into: Set<String>()) { $0.formUnion($1.requiredSplitTypes) }
        let provided = all.reduce(into: Set<String>()) { $0.formUnion($1.splitTypes) }
        return required.subtracting(provided).sorted()
    }

    /// `SigningDetails.signaturesMatchExactly` (`Signature.java:314-334`): as many certificates, the same ones in any
    /// order, a lineage not counted. An APK Minato cannot verify is compared by the certificates it names, and the set
    /// then asks; signed never matches unsigned.
    static func sameSigner(_ member: SetMember, _ reference: SetMember) -> Bool {
        (member.signature.check == .unsigned) == (reference.signature.check == .unsigned)
            && member.certificates.count == reference.certificates.count
            && Set(member.certificates) == Set(reference.certificates)
    }

    /// The weakest member's state: unsigned only when all are, as a mix is rejected above.
    static func signing(of members: [SetMember]) -> SetSigning {
        let unverifiable = members.compactMap { member -> UnverifiableFile? in
            if case .unverified(let reason) = member.signature.check { UnverifiableFile(fileName: member.fileName, reason: reason) } else { nil }
        }
        guard let verified = members.first(where: { $0.signature.check == .verified }) else {
            return unverifiable.first.map { .unverified($0.reason) } ?? .unsigned
        }
        let signer = verified.signature.signer ?? SignerIdentity(current: [])
        return unverifiable.isEmpty ? .verified(signer) : .partlyVerified(signer, unverifiable: unverifiable)
    }
}
