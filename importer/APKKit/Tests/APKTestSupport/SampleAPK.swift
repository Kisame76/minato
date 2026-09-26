import Foundation

/// Complete synthetic APKs for import tests.
public enum SampleAPK {
    /// With `nativeActivity` the app starts android.app.NativeActivity with its libgame.so and has no
    /// Java code; otherwise it declares no activity, as a Java app that needs ART.
    public static func make(package: String = "org.example.game", versionCode: Int32 = 1, label: String = "Sample Game",
                            abis: [String] = ["arm64-v8a"], splitRequired: Bool = false, nativeActivity: Bool = false,
                            screenOrientation: Int32? = nil) -> Data {
        var resources = ARSCBuilder()
        resources.packageName = package
        let icon = resources.add("mipmap", "ic_launcher", [
            (ARSCBuilder.Config(density: 480, sdkVersion: 4), .string("res/mipmap-xxhdpi-v4/ic_launcher.png")),
        ])

        var applicationAttributes: [AXMLBuilder.Attribute] = [
            .android("label", 0x0101_0001, .string(label)),
            .android("icon", 0x0101_0002, .reference(icon)),
        ]
        if splitRequired { applicationAttributes.append(.android("isSplitRequired", 0x0101_0591, .bool(true))) }
        var activities: [AXMLBuilder.Element] = []
        if nativeActivity {
            applicationAttributes.append(.android("hasCode", 0x0101_000C, .bool(false)))
            var activityAttributes: [AXMLBuilder.Attribute] = [.android("name", 0x0101_0003, .string("android.app.NativeActivity"))]
            if let screenOrientation {
                activityAttributes.append(.android("screenOrientation", 0x0101_001E, .int(screenOrientation)))
            }
            activities.append(AXMLBuilder.Element("activity", activityAttributes, [
                AXMLBuilder.Element("meta-data", [
                    .android("name", 0x0101_0003, .string("android.app.lib_name")),
                    .android("value", 0x0101_0024, .string("game")),
                ]),
                AXMLBuilder.Element("intent-filter", [], [
                    AXMLBuilder.Element("action", [.android("name", 0x0101_0003, .string("android.intent.action.MAIN"))]),
                    AXMLBuilder.Element("category", [.android("name", 0x0101_0003, .string("android.intent.category.LAUNCHER"))]),
                ]),
            ]))
        }
        let manifest = AXMLBuilder.Element("manifest", [
            .plain("package", .string(package)),
            .android("versionCode", 0x0101_021B, .int(versionCode)),
            .android("versionName", 0x0101_021C, .string("\(versionCode).0")),
        ], [AXMLBuilder.Element("application", applicationAttributes, activities)])

        var zip = ZipBuilder()
        zip.add("AndroidManifest.xml", AXMLBuilder().build(manifest))
        zip.add("resources.arsc", resources.build(), method: .stored)
        zip.add("res/mipmap-xxhdpi-v4/ic_launcher.png", pngSignature + Data("icon of \(package)".utf8), method: .stored)
        for abi in abis {
            let elf = ELFBuilder(segments: [.load(align: 0x4000), .load(align: 0x4000)])
            zip.add("lib/\(abi)/libgame.so", elf.build() + Data(repeating: 0x11, count: 2_000))
        }
        return zip.build().data
    }

    public static let pngSignature = Data([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A])
}
