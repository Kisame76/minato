import Foundation

/// Reads an APK into an `APKReport`. Only the manifest is mandatory; everything else degrades
/// gracefully (missing resources give the package name as label, unreadable libraries are noted).
public enum APKAnalyzer {
    static let arm64 = "arm64-v8a"
    private static let thirtyTwoBitABIs: Set<String> = ["armeabi", "armeabi-v7a"]

    public static func analyze(_ archive: ZipArchive, preferredLanguages: [String]) throws -> APKReport {
        let manifest = try AndroidManifest(xml: BinaryXML.parse(archive.data(named: "AndroidManifest.xml", maxSize: 16 << 20)))
        let resources = try? ResourceTable(data: archive.data(named: "resources.arsc"))

        func text(_ value: ResourceValue?) -> String? {
            switch value {
            case .string(let string): return string
            case .reference(let id): return resources?.string(for: id, preferredLanguages: preferredLanguages)
            default: return nil
            }
        }

        let libraryEntries = archive.entries.filter { entry in
            let parts = entry.name.split(separator: "/", omittingEmptySubsequences: false)
            return parts.count == 3 && parts[0] == "lib" && entry.name.hasSuffix(".so")
        }
        let abis = Set(libraryEntries.map { String($0.name.split(separator: "/")[1]) }).sorted()
        let selectedABI = abis.contains(arm64) ? arm64 : nil
        let nativeLibraries = libraryEntries
            .filter { $0.name.hasPrefix("lib/\(arm64)/") }
            .map { inspectLibrary($0, in: archive) }
        let allLibraryNames = Set(libraryEntries.map { String($0.name.split(separator: "/")[2]) })

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
            requiredGLESVersion: manifest.requiredGLESVersion,
            requiredVulkanVersion: manifest.requiredVulkanVersion,
            requiredVulkanLevel: manifest.requiredVulkanLevel,
            requiresAndroidExtensionPack: manifest.requiresAndroidExtensionPack,
            splitName: manifest.splitName,
            requiresSplits: manifest.requiresSplits,
            playDependencies: playDependencies(manifest),
            engine: engine(allLibraryNames),
            iconPath: chooseIcon(manifest, resources: resources, archive: archive),
            signature: signature(of: archive),
            compatibility: Compatibility(status: .ready, issues: []))
        report.compatibility = compatibility(of: report, manifest: manifest)
        return report
    }

    /// The certificates and the signer as the file names them, checked cryptographically (ADR-0034,
    /// step d): once verified, the signer is the one the check proved.
    static func signature(of archive: ZipArchive) -> SignatureInfo {
        var signature = APKSignatures.read(archive)
        let verification = SignatureVerifier.verify(archive)
        signature.check = verification.check
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

    private static func compatibility(of report: APKReport, manifest: AndroidManifest) -> Compatibility {
        var issues: [Compatibility.Issue] = []
        var blocked = false

        if case .failed(let failure) = report.signature.check {
            issues.append(.signatureInvalid(failure))
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

        if manifest.hasCode { issues.append(.needsART) }
        if let gles = report.requiredGLESVersion, gles > 0x0003_0000 { issues.append(.requiresGLES(gles)) }
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

    static func chooseIcon(_ manifest: AndroidManifest, resources: ResourceTable?, archive: ZipArchive) -> String? {
        let references: [UInt32] = [manifest.icon, manifest.roundIcon].compactMap {
            if case .reference(let id) = $0 { return id }
            return nil
        }
        if let resources {
            for id in references {
                let files = resources.files(for: id)
                if let raster = bestRaster(files, in: archive) { return raster }
                for file in files where file.path.hasSuffix(".xml") {
                    if let foreground = adaptiveForeground(file.path, in: archive),
                       let raster = bestRaster(resources.files(for: foreground), in: archive) {
                        return raster
                    }
                }
            }
        }
        // Vector or unresolvable icon: the largest launcher-named image, then any mipmap image.
        let names = references.compactMap { resources?.name(of: $0)?.split(separator: "/").last.map(String.init) }
        return largestImage(in: archive) { path in
            let file = path.split(separator: "/").last.map(String.init) ?? ""
            return names.contains { file.hasPrefix($0) }
        } ?? largestImage(in: archive) { $0.hasPrefix("res/mipmap") }
    }

    private static func isRaster(_ path: String) -> Bool {
        let lower = path.lowercased()
        return rasterExtensions.contains { lower.hasSuffix($0) }
    }

    private static func bestRaster(_ files: [ResourceFile], in archive: ZipArchive) -> String? {
        func score(_ config: ResourceConfig) -> Int {
            switch config.density {
            case 0, ResourceConfig.densityNone: return 160
            case ResourceConfig.densityAny: return 0
            default: return Int(config.density)
            }
        }
        return files
            .filter { isRaster($0.path) && archive.entry(named: $0.path) != nil }
            .max { score($0.config) < score($1.config) }?
            .path
    }

    private static func adaptiveForeground(_ path: String, in archive: ZipArchive) -> UInt32? {
        guard let data = try? archive.data(named: path, maxSize: 1 << 20),
              let root = try? BinaryXML.parse(data), root.name == "adaptive-icon",
              case .reference(let id)? = root.children(named: "foreground").first?.android(.drawable)?.value
        else { return nil }
        return id
    }

    private static func largestImage(in archive: ZipArchive, where matches: (String) -> Bool) -> String? {
        archive.entries
            .filter { $0.name.hasPrefix("res/") && isRaster($0.name) && matches($0.name) }
            .max { $0.uncompressedSize < $1.uncompressedSize }?
            .name
    }
}
