/// The two stores APK-Runner takes its certificate from (ADR-0023).
public enum Store: String, Sendable, Codable, CaseIterable {
    case sideStore
    case altStore

    /// The scheme of the store's `certificate` export link. AltStore Classic registers
    /// `altstore-classic` from 2.2 on and AltStore PAL does not; `altstore` would reach both.
    public var exportScheme: String { self == .sideStore ? "sidestore" : "altstore-classic" }
    public var name: String { self == .sideStore ? "SideStore" : "AltStore" }
    /// The first version with the export link.
    public var minimumVersion: String { self == .sideStore ? "0.6.2" : "2.2.1" }
}

/// Who installed the app, read from the markers SideStore and AltStore write into the Info.plist of
/// every app they sign; Xcode writes none (docs/research/store-certificates.md, section 5).
public enum Installer: Sendable, Equatable, Codable {
    case store(Store)
    /// Xcode, or something else that leaves no marker.
    case other

    public static func detect(infoDictionary: [String: Any]) -> Installer {
        let utis = (infoDictionary["UTExportedTypeDeclarations"] as? [[String: Any]] ?? [])
            .compactMap { $0["UTTypeIdentifier"] as? String }
        let schemes = (infoDictionary["CFBundleURLTypes"] as? [Any] ?? [])
            .compactMap { ($0 as? [String: Any])?["CFBundleURLSchemes"] as? [String] }
            .flatMap { $0 }
        if utis.contains(where: { $0.hasPrefix("io.sidestore.Installed.") })
            || schemes.contains(where: { $0.hasPrefix("sidestore-") }) {
            return .store(.sideStore)
        }
        // SideStore up to 0.7.0-alpha writes ALTBundleIdentifier as well, so it counts only here.
        // AltServer installs lack the UTI but carry the scheme and the key.
        if utis.contains(where: { $0.hasPrefix("io.altstore.Installed.") })
            || schemes.contains(where: { $0.hasPrefix("altstore-") })
            || infoDictionary["ALTBundleIdentifier"] is String {
            return .store(.altStore)
        }
        return .other
    }
}
