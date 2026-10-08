/// The backend under a session's ANGLE (ADR-0044 decision 4, step d)): Metal, or ANGLE's Vulkan backend on MoltenVK,
/// the ES 3.1 path, once graphics/host's run-time check passed (RuntimeSessionHost).
public enum ANGLEBackend: Sendable, Equatable {
    case metal
    case vulkan

    /// The installed set's highest glEsVersion (APKAnalyzer): ES 3.1 or more takes Vulkan, where 3.2 and up get 3.1
    /// as the closest and keep their note; every other app, nil too, Metal.
    public init(requiredGLESVersion: UInt32?) {
        self = (requiredGLESVersion ?? 0) >= 0x0003_0001 ? .vulkan : .metal
    }
}
