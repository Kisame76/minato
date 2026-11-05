import APKTestSupport
import Foundation
import Testing

@testable import APKLibrary

/// ADR-0074 decision 4: the activities an installed version declares, which a notification's cold tap may start.
@Suite struct InstalledActivitiesTests {
    @Test func theActivitiesOfBaseAndItsSplits() throws {
        let folder = FileManager.default.temporaryDirectory.appending(path: "activities-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: folder) }
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        try SampleAPK.make(package: "org.example.game", nativeActivity: true)
            .write(to: folder.appending(path: "base.apk"))
        try Data("not a zip".utf8).write(to: folder.appending(path: "split_broken.apk"))
        try Data("ignored".utf8).write(to: folder.appending(path: "notes.txt"))
        #expect(InstalledActivities.read(folder: folder) == ["android.app.NativeActivity"])
    }

    @Test func aFolderWithoutAnAPKHasNone() throws {
        let folder = FileManager.default.temporaryDirectory.appending(path: "activities-\(UUID().uuidString)")
        #expect(InstalledActivities.read(folder: folder).isEmpty)
    }
}
