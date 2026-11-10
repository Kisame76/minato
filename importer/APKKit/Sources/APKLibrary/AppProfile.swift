import Foundation

/// The user's choices for one app, all off by default (ADR-0065 decision 1, ADR-0066 decision 2): what the session
/// hands the runtime before the program starts. Kept in the app's data folder (`LibraryPaths.profileFile`); a key this
/// version does not know is passed over and a missing one is the default, so a later field needs no migration.
public struct AppProfile: Sendable, Equatable, Codable {
    /// The phone whose names the app sees (`apkrt_set_device_profile`).
    public enum DeviceProfile: String, Sendable, Codable, CaseIterable {
        case pixel, galaxy
    }

    /// The FPS unlock (`apkrt_set_frame_rate_unlock`): only divisors of 120 Hz (ADR-0066).
    public enum FrameRate: Int, Sendable, Codable, CaseIterable {
        case fps60 = 60
        case fps120 = 120
    }

    /// nil: the host's own names.
    public var deviceProfile: DeviceProfile?
    /// nil: as the app asks.
    public var frameRate: FrameRate?
    /// Whether the app may install apps (REQUEST_INSTALL_PACKAGES, `apkrt_set_install_sources`), which the user allows
    /// when the app asks; nil: not allowed, Android's default.
    public var installSources: Bool?

    public init(deviceProfile: DeviceProfile? = nil, frameRate: FrameRate? = nil, installSources: Bool? = nil) {
        self.deviceProfile = deviceProfile
        self.frameRate = frameRate
        self.installSources = installSources
    }

    public var isDefault: Bool { self == AppProfile() }

    /// The app's profile; the default where there is no file or one that does not decode, which the next save
    /// replaces.
    public static func load(for package: String, in paths: LibraryPaths) -> AppProfile {
        guard let data = try? Data(contentsOf: paths.profileFile(for: package)),
              let profile = try? JSONDecoder().decode(AppProfile.self, from: data)
        else { return AppProfile() }
        return profile
    }

    /// Writes the profile, or deletes the file for the default.
    public func save(for package: String, in paths: LibraryPaths) throws {
        let file = paths.profileFile(for: package)
        if isDefault {
            do {
                try FileManager.default.removeItem(at: file)
            } catch CocoaError.fileNoSuchFile {
            }
            return
        }
        try FileManager.default.createDirectory(at: file.deletingLastPathComponent(), withIntermediateDirectories: true)
        let encoder = JSONEncoder()
        encoder.outputFormatting = .sortedKeys
        try encoder.encode(self).write(to: file, options: .atomic)
    }
}
