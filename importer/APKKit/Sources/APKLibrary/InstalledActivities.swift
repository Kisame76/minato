import APKKit
import Foundation

/// The activities and aliases an installed version declares in base.apk and its split APKs (ADR-0074 decision 4): a
/// notification's cold tap starts only one of these. An APK Minato cannot read counts as declaring none.
public enum InstalledActivities {
    public static func read(folder: URL) -> Set<String> {
        let files = (try? FileManager.default.contentsOfDirectory(at: folder, includingPropertiesForKeys: nil)) ?? []
        var activities: Set<String> = []
        for file in files where file.pathExtension == "apk" {
            guard let archive = try? ZipArchive(url: file),
                  let xml = try? archive.data(named: "AndroidManifest.xml", maxSize: 16 << 20),
                  let manifest = try? AndroidManifest(xml: BinaryXML.parse(xml)) else { continue }
            activities.formUnion(manifest.activities)
        }
        return activities
    }
}
