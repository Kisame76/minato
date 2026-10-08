import Testing

@testable import APKSession

/// ADR-0044 d): an app that asks for ES 3.1 or more runs on ANGLE's Vulkan backend, the highest version Minato
/// offers, 3.2 and up as the closest; every other app on Metal.
@Suite("ANGLEBackend")
struct ANGLEBackendTests {
    @Test func es31AndAboveTakeVulkan() {
        #expect(ANGLEBackend(requiredGLESVersion: 0x0003_0001) == .vulkan)
        #expect(ANGLEBackend(requiredGLESVersion: 0x0003_0002) == .vulkan)
    }

    @Test func everyOtherAppTakesMetal() {
        #expect(ANGLEBackend(requiredGLESVersion: nil) == .metal)
        #expect(ANGLEBackend(requiredGLESVersion: 0x0002_0000) == .metal)
        #expect(ANGLEBackend(requiredGLESVersion: 0x0003_0000) == .metal)
    }
}
