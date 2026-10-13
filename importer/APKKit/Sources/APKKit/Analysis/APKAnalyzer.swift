import Foundation

/// Reads an APK into an `APKReport`. Only the manifest is mandatory; everything else degrades
/// gracefully (missing resources give the package name as label, unreadable libraries are noted).
public enum APKAnalyzer {
    static let arm64 = "arm64-v8a"
    private static let thirtyTwoBitABIs: Set<String> = ["armeabi", "armeabi-v7a"]

    public static func analyze(_ archive: ZipArchive, preferredLanguages: [String]) throws -> APKReport {
        try analyze(base: archive, splits: [], preferredLanguages: preferredLanguages).report
    }

    /// A split set's report (ADR-0048, decision 5): the base's manifest, with the native code, the ELF probe,
    /// "32-bit only" and "needs Java" judged over the base and the splits Minato installs, and the label, version
    /// name and icon resolved over all their resource tables, the base's IDs with the best configuration in any of
    /// them. Its OpenGL ES version is the highest of any APK of the set, a split's own uses-feature too, which
    /// Android's PackageManager ignores (it parses only a split's <application>): Minato's choice of the session's
    /// path (ADR-0044 d)), so that a split's code gets the version it asks for. `otherABIs` are the ABIs of APKs
    /// Minato skips, so a set whose ABI splits are 32-bit only says so; `needsSplits`, the set's own verdict,
    /// replaces the base's. `icon` is the APK that holds the chosen icon.
    public static func analyze(base: ZipArchive, splits: [ZipArchive], otherABIs: [String] = [], needsSplits: Bool? = nil,
                               preferredLanguages: [String]) throws -> (report: APKReport, icon: ZipArchive?) {
        func readManifest(_ archive: ZipArchive) throws -> AndroidManifest {
            try AndroidManifest(xml: BinaryXML.parse(archive.data(named: "AndroidManifest.xml", maxSize: 16 << 20)))
        }
        let manifest = try readManifest(base)
        // The highest OpenGL ES version any APK of the set asks for, a split's own uses-feature too: Minato's choice
        // (ADR-0044 d)), since Android's PackageManager reads the base's alone.
        var requiredGLESVersion = manifest.requiredGLESVersion
        let apks: [(split: String?, archive: ZipArchive, hasCode: Bool)] = try [(nil, base, manifest.hasCode)]
            + splits.map { split in
                let splitManifest = try readManifest(split)
                if let version = splitManifest.requiredGLESVersion {
                    requiredGLESVersion = max(requiredGLESVersion ?? 0, version)
                }
                return (splitManifest.splitName, split, splitManifest.hasCode)
            }
        let tables = apks.compactMap { apk in
            (try? ResourceTable(data: apk.archive.data(named: "resources.arsc"))).map { (table: $0, archive: apk.archive) }
        }

        func text(_ value: ResourceValue?) -> String? {
            switch value {
            case .string(let string): return string
            case .reference(let id): return string(for: id, in: tables.map(\.table), preferredLanguages: preferredLanguages)
            default: return nil
            }
        }

        let libraryEntries = apks.flatMap { apk in
            apk.archive.entries.filter { entry in
                let parts = entry.name.split(separator: "/", omittingEmptySubsequences: false)
                return parts.count == 3 && parts[0] == "lib" && entry.name.hasSuffix(".so")
            }.map { (split: apk.split, archive: apk.archive, entry: $0) }
        }
        let abis = Set(libraryEntries.map { String($0.entry.name.split(separator: "/")[1]) }).union(otherABIs).sorted()
        let selectedABI = abis.contains(arm64) ? arm64 : nil
        // One library in several APKs: Android installs one copy (ADR-0048, decision 8). It extracts the base and
        // then the splits by name into one lib/, a later copy replacing an earlier one (NativeLibraryHelper.java:
        // 196-205, NativeLibraryHelper.cpp:211-218), or, where the base's extractNativeLibs is false, loads from the
        // first APK on its search path (LoadedApk.java:485-489, 565-566; PackageLite.java:141). `splits` come by name.
        let arm64Libraries = libraryEntries.filter { $0.entry.name.hasPrefix("lib/\(arm64)/") }
        let extracts = manifest.extractNativeLibs ?? true
        var installed: [String: Int] = [:]
        for (index, library) in arm64Libraries.enumerated() where extracts || installed[library.entry.name] == nil {
            installed[library.entry.name] = index
        }
        let nativeLibraries = arm64Libraries.enumerated()
            .filter { installed[$0.element.entry.name] == $0.offset }
            .map { _, library in
                var inspected = inspectLibrary(library.entry, in: library.archive)
                inspected.split = library.split
                return inspected
            }
        let allLibraryNames = Set(libraryEntries.map { String($0.entry.name.split(separator: "/")[2]) })
        let icon = chooseIcon(manifest, tables: tables, archives: apks.map(\.archive))

        let runtime: APKReport.Runtime = manifest.usesNativeActivity
            ? .nativeActivity(library: manifest.nativeLibraryName ?? "main")
            : .art

        var report = APKReport(
            packageName: manifest.packageName,
            label: text(manifest.label).flatMap { $0.isEmpty ? nil : $0 } ?? manifest.packageName,
            versionCode: manifest.versionCode,
            versionName: text(manifest.versionName),
            minSdk: manifest.minSdk,
            targetSdk: manifest.targetSdk,
            launcherActivity: manifest.launcherActivity,
            screenOrientation: manifest.screenOrientation,
            configChanges: manifest.configChanges,
            runtime: runtime,
            nativeFunction: manifest.nativeFunctionName,
            abis: abis,
            selectedABI: selectedABI,
            nativeLibraries: nativeLibraries,
            extractNativeLibs: manifest.extractNativeLibs,
            requiredGLESVersion: requiredGLESVersion,
            requiredVulkanVersion: manifest.requiredVulkanVersion,
            requiredVulkanLevel: manifest.requiredVulkanLevel,
            requiresAndroidExtensionPack: manifest.requiresAndroidExtensionPack,
            splitName: manifest.splitName,
            requiresSplits: needsSplits ?? manifest.requiresSplits,
            playDependencies: playDependencies(manifest),
            engine: engine(allLibraryNames),
            iconPath: icon?.path,
            signature: signature(of: base, targetSdk: manifest.targetSdk),
            compatibility: Compatibility(status: .ready, issues: []))
        report.usesInternet = manifest.requestsInternet
        report.compatibility = compatibility(of: report, manifest: manifest, hasCode: apks.contains(where: \.hasCode))
        return (report, icon?.archive)
    }

    /// A string resource over a set's resource tables, following references: the variants of every table compete,
    /// as AssetManager2 resolves across a set's APKs (`AssetManager2.cpp:596-625`).
    private static func string(for id: UInt32, in tables: [ResourceTable], preferredLanguages: [String]) -> String? {
        var visited: Set<UInt32> = []
        var current = id
        while visited.insert(current).inserted, visited.count <= 16 {
            guard let chosen = ResourceTable.pickLanguage(tables.flatMap { $0.values(for: current) }, preferredLanguages)
            else { return nil }
            switch chosen.value {
            case .string(let text): return text
            case .reference(let target): current = target
            default: return nil
            }
        }
        return nil
    }

    /// The certificates and the signer as the file names them, checked cryptographically (ADR-0034,
    /// step d): once verified, the signer is the one the check proved, and its proof is kept for signers.json.
    static func signature(of archive: ZipArchive, targetSdk: Int) -> SignatureInfo {
        var signature = APKSignatures.read(archive)
        let verification = SignatureVerifier.verify(archive, targetSdk: targetSdk)
        signature.check = verification.check
        signature.proof = verification.proof
        if let signer = verification.signer { signature.signer = signer }
        return signature
    }

    public static func iconData(for report: APKReport, in archive: ZipArchive) throws -> Data? {
        guard let path = report.iconPath else { return nil }
        return try archive.data(named: path, maxSize: 16 << 20)
    }

    // MARK: - Native libraries

    private static func inspectLibrary(_ entry: ZipEntry, in archive: ZipArchive) -> NativeLibrary {
        var library = NativeLibrary(
            name: String(entry.name.split(separator: "/").last ?? ""),
            path: entry.name,
            size: entry.uncompressedSize,
            isCompressed: entry.method != .stored,
            isZipAligned16K: nil,
            elf: nil,
            problem: nil)
        if entry.method == .stored {
            library.isZipAligned16K = (try? archive.dataOffset(of: entry)).map { $0 % 16_384 == 0 }
        }
        do {
            do {
                library.elf = try ELFProbe.probe(archive.prefix(of: entry, count: ELFProbe.typicalHeaderSize))
            } catch APKError.truncated {
                // Program headers beyond the first page: read further.
                library.elf = try ELFProbe.probe(archive.prefix(of: entry, count: 64 * 1024))
            }
        } catch {
            library.problem = "\(error)"
        }
        return library
    }

    // MARK: - Compatibility

    /// `hasCode`: whether any of the APKs has Java code.
    private static func compatibility(of report: APKReport, manifest: AndroidManifest, hasCode: Bool) -> Compatibility {
        var issues: [Compatibility.Issue] = []
        var blocked = false

        // In Android's order: the installer's parse checks the signature itself, then refuses for uses-sdk; the
        // package parser's v1 rule needs its targetSdk (ApkLiteParseUtils.java:368-377, 569-625).
        let signatureFailure: SignatureFailure? = if case .failed(let failure) = report.signature.check { failure } else { nil }
        if let signatureFailure, signatureFailure.kind != .noV2Signature {
            issues.append(.signatureInvalid(signatureFailure))
            blocked = true
        }
        if let refusal = manifest.sdkRefusal {
            issues.append(.olderSDK(refusal))
            blocked = true
        }
        if let attribute = manifest.sdkReference {
            issues.append(.sdkReference(attribute))
            blocked = true
        }
        if let signatureFailure, signatureFailure.kind == .noV2Signature {
            issues.append(.signatureInvalid(signatureFailure))
            blocked = true
        }
        if let split = report.splitName {
            issues.append(.isSplitAPK(split))
            blocked = true
        }
        if !report.abis.isEmpty && report.selectedABI == nil {
            let thirtyTwoBit = report.abis.filter(thirtyTwoBitABIs.contains)
            issues.append(thirtyTwoBit.isEmpty ? .noARM64(report.abis) : .only32Bit(thirtyTwoBit))
            blocked = true
        }
        for library in report.nativeLibraries {
            if let elf = library.elf, !elf.isAArch64 {
                issues.append(.wrongArchitecture(library.name))
                blocked = true
            } else if library.elf == nil {
                issues.append(.unreadableLibrary(library.name))
            }
        }

        if hasCode { issues.append(.needsART) }
        // ES 3.1 runs on ANGLE's Vulkan backend (ADR-0044 d)); a higher version gets 3.1 and this note.
        if let gles = report.requiredGLESVersion, gles > 0x0003_0001 { issues.append(.requiresGLES(gles)) }
        let fourK = report.nativeLibraries.filter { ($0.elf?.pageAlignment ?? 16_384) < 16_384 }.map(\.name)
        if !fourK.isEmpty { issues.append(.pageAligned4K(fourK)) }
        if report.playDependencies.contains(.licenseCheck) { issues.append(.usesLicenseCheck) }

        let status: Compatibility.Status = blocked ? .incompatible : report.requiresSplits ? .needsSplits : .ready
        return Compatibility(status: status, issues: issues)
    }

    // MARK: - Dependencies and engine

    private static func playDependencies(_ manifest: AndroidManifest) -> [APKReport.PlayDependency] {
        let components = manifest.components
        var found: Set<APKReport.PlayDependency> = []
        if manifest.metaData["com.google.android.gms.version"] != nil
            || components.contains(where: { $0.hasPrefix("com.google.android.gms.") }) {
            found.insert(.playServices)
        }
        if components.contains(where: { $0.hasPrefix("com.google.firebase.") })
            || manifest.metaData.keys.contains(where: { $0.hasPrefix("firebase_") }) {
            found.insert(.firebase)
        }
        if manifest.permissions.contains("com.android.vending.BILLING") { found.insert(.billing) }
        if manifest.permissions.contains("com.android.vending.CHECK_LICENSE") { found.insert(.licenseCheck) }
        if manifest.metaData["com.google.android.gms.games.APP_ID"] != nil
            || components.contains(where: { $0.hasPrefix("com.google.android.gms.games.") || $0.hasPrefix("com.google.games.") }) {
            found.insert(.playGames)
        }
        return APKReport.PlayDependency.allCases.filter(found.contains)
    }

    private static func engine(_ libraries: Set<String>) -> String? {
        let known: [(test: (Set<String>) -> Bool, name: String)] = [
            ({ $0.contains("libil2cpp.so") }, "Unity (IL2CPP)"),
            ({ $0.contains("libunity.so") }, "Unity"),
            ({ $0.contains("libUE4.so") || $0.contains("libUnreal.so") }, "Unreal Engine"),
            ({ $0.contains("libgodot_android.so") }, "Godot"),
            ({ $0.contains { $0.hasPrefix("libcocos") } }, "Cocos2d-x"),
            ({ $0.contains("libgdx.so") }, "libGDX"),
            ({ $0.contains("libyoyo.so") }, "GameMaker"),
            ({ $0.contains("libflutter.so") }, "Flutter"),
            ({ $0.contains("libreactnativejni.so") }, "React Native"),
            ({ $0.contains("libmonodroid.so") || $0.contains("libxamarin-app.so") }, ".NET (Xamarin/MAUI)"),
            ({ $0.contains("libSDL2.so") || $0.contains("libSDL3.so") }, "SDL"),
        ]
        return known.first { $0.test(libraries) }?.name
    }

    // MARK: - Icon

    private static let rasterExtensions = [".png", ".webp", ".jpg", ".jpeg"]

    /// The icon's image and the APK that holds it, over a set's resource tables: the best raster of the icon or the round
    /// icon in any of them, else an adaptive icon's foreground.
    static func chooseIcon(_ manifest: AndroidManifest, tables: [(table: ResourceTable, archive: ZipArchive)],
                           archives: [ZipArchive]) -> (path: String, archive: ZipArchive)? {
        let references: [UInt32] = [manifest.icon, manifest.roundIcon].compactMap {
            if case .reference(let id) = $0 { return id }
            return nil
        }
        func candidates(for id: UInt32) -> [(file: ResourceFile, archive: ZipArchive)] {
            tables.flatMap { table in table.table.files(for: id).map { (file: $0, archive: table.archive) } }
        }
        for id in references {
            let files = candidates(for: id)
            if let raster = bestRaster(files) { return raster }
            for file in files where file.file.path.hasSuffix(".xml") {
                if let foreground = adaptiveForeground(file.file.path, in: file.archive),
                   let raster = bestRaster(candidates(for: foreground)) {
                    return raster
                }
            }
        }
        // Vector or unresolvable icon: the largest launcher-named image, then any mipmap image.
        let names = references.compactMap { id in
            tables.lazy.compactMap { $0.table.name(of: id) }.first?.split(separator: "/").last.map(String.init)
        }
        return largestImage(in: archives) { path in
            let file = path.split(separator: "/").last.map(String.init) ?? ""
            return names.contains { file.hasPrefix($0) }
        } ?? largestImage(in: archives) { $0.hasPrefix("res/mipmap") }
    }

    private static func isRaster(_ path: String) -> Bool {
        let lower = path.lowercased()
        return rasterExtensions.contains { lower.hasSuffix($0) }
    }

    private static func bestRaster(_ files: [(file: ResourceFile, archive: ZipArchive)]) -> (path: String, archive: ZipArchive)? {
        func score(_ config: ResourceConfig) -> Int {
            switch config.density {
            case 0, ResourceConfig.densityNone: return 160
            case ResourceConfig.densityAny: return 0
            default: return Int(config.density)
            }
        }
        return files
            .filter { isRaster($0.file.path) && $0.archive.entry(named: $0.file.path) != nil }
            .max { score($0.file.config) < score($1.file.config) }
            .map { ($0.file.path, $0.archive) }
    }

    private static func adaptiveForeground(_ path: String, in archive: ZipArchive) -> UInt32? {
        guard let data = try? archive.data(named: path, maxSize: 1 << 20),
              let root = try? BinaryXML.parse(data), root.name == "adaptive-icon",
              case .reference(let id)? = root.children(named: "foreground").first?.android(.drawable)?.value
        else { return nil }
        return id
    }

    private static func largestImage(in archives: [ZipArchive], where matches: (String) -> Bool)
        -> (path: String, archive: ZipArchive)? {
        archives
            .flatMap { archive in archive.entries.map { (entry: $0, archive: archive) } }
            .filter { $0.entry.name.hasPrefix("res/") && isRaster($0.entry.name) && matches($0.entry.name) }
            .max { $0.entry.uncompressedSize < $1.entry.uncompressedSize }
            .map { ($0.entry.name, $0.archive) }
    }
}
