// swift-tools-version: 6.2
import PackageDescription

let package = Package(
    name: "APKKit",
    platforms: [.iOS("26.0"), .macOS("26.0")],
    products: [
        // Parsing of APK files: ZIP, binary XML, resources, signatures, native libraries.
        .library(name: "APKKit", targets: ["APKKit"]),
        // The on-device library: finds dropped APKs, installs them, keeps the index.
        .library(name: "APKLibrary", targets: ["APKLibrary"]),
        // What Minato decides about its signing certificate (ADR-0023): pure logic, no
        // Security.framework, so the decisions are tested on the Mac.
        .library(name: "APKSigning", targets: ["APKSigning"]),
    ],
    targets: [
        .target(name: "APKKit"),
        .target(name: "APKLibrary", dependencies: ["APKKit", "APKSigning"]),
        .testTarget(name: "APKLibraryTests", dependencies: ["APKLibrary", "APKKit", "APKSigning", "APKTestSupport"]),
        .target(name: "APKSigning"),
        .testTarget(name: "APKSigningTests", dependencies: ["APKSigning"]),
        // Development tool: prints what APKKit reads from a real APK.
        .executableTarget(name: "apk-inspect", dependencies: ["APKKit"]),
        // Test-only builders that encode ZIP, binary XML and resource tables independently
        // of the production parsers.
        .target(name: "APKTestSupport", path: "Tests/APKTestSupport"),
        .testTarget(name: "APKKitTests", dependencies: ["APKKit", "APKTestSupport"]),
    ]
)
