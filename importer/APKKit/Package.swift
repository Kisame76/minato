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
        // Game mode's pure logic (ADR-0032).
        .library(name: "APKSession", targets: ["APKSession"]),
        // The parity benchmark's run plan, CSV and validity (ADR-0037).
        .library(name: "APKBench", targets: ["APKBench"]),
        // Split sets and containers (M5, ADR-0048 and ADR-0049): import units, Android's checks, the split choice.
        .library(name: "APKFormats", targets: ["APKFormats"]),
    ],
    targets: [
        .target(name: "APKKit"),
        .target(name: "APKLibrary", dependencies: ["APKKit", "APKSigning", "APKFormats"]),
        .testTarget(name: "APKLibraryTests", dependencies: ["APKLibrary", "APKKit", "APKSigning", "APKFormats", "APKTestSupport"]),
        .target(name: "APKSigning"),
        .testTarget(name: "APKSigningTests", dependencies: ["APKSigning"]),
        // Game mode's pure logic (ADR-0032), tested on the Mac: the launch request and decision, the
        // lifecycle machine and the orientation of a session.
        .target(name: "APKSession", dependencies: ["APKLibrary"]),
        .testTarget(name: "APKSessionTests", dependencies: ["APKSession", "APKLibrary", "APKKit", "APKTestSupport"]),
        // The parity benchmark's pure logic (ADR-0037, step d): the run plan, the CSV and the validity.
        .target(name: "APKBench"),
        .testTarget(name: "APKBenchTests", dependencies: ["APKBench"]),
        // Development tool: tells tools/benchmark.sh the next run from the results so far (ADR-0037).
        .executableTarget(name: "bench-plan", dependencies: ["APKBench"]),
        // Development tool: prints what APKKit reads from a real APK, and what SetChecks answers to a set of them.
        .executableTarget(name: "apk-inspect", dependencies: ["APKKit", "APKFormats"]),
        // Writes APKs for our test apps (make-apk, ADR-0031): ZIP, binary XML and string pools,
        // encoded independently of the production parsers, plus the manifest compiler and the layout.
        .target(name: "APKPack", dependencies: ["APKKit"]),
        .testTarget(name: "APKPackTests", dependencies: ["APKPack", "APKKit", "APKTestSupport"]),
        // Development tool: packs our test apps (ADR-0031); tools/make-apk.sh builds and runs it.
        .executableTarget(name: "make-apk", dependencies: ["APKPack"]),
        // Test-only builders for resource tables, ELF headers, signing blocks and whole sample APKs;
        // re-exports APKPack's writers.
        .target(name: "APKTestSupport", dependencies: ["APKPack"], path: "Tests/APKTestSupport"),
        .testTarget(name: "APKKitTests", dependencies: ["APKKit", "APKTestSupport"]),
        // Split sets and containers (M5, ADR-0048 and ADR-0049), pure logic tested on the Mac.
        .target(name: "APKFormats", dependencies: ["APKKit"]),
        .testTarget(name: "APKFormatsTests", dependencies: ["APKFormats", "APKKit", "APKTestSupport"]),
    ]
)
