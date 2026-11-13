import Foundation
import Testing

@testable import APKLibrary

/// The guest's export table (ADR-0055, step a4; the runtime's format): per library its NEEDED names and its exported
/// symbols with their versions, and which are stubs.
@Suite("Export table")
struct ExportTableTests {
    static let text = """
        # minato export table 1
        @ libandroid.so
        needs libc.so libnativewindow.so
        AAsset_close@@LIBANDROID
        ASensorManager_getInstance@@LIBANDROID!
        oldname@LIBANDROID
        @ libc.so
        needs
        malloc
        free
        memcpy!
        @ libnativewindow.so
        needs libc.so
        ANativeWindow_release
        """

    @Test func readsTheBlocksTheirNeedsAndTheirSymbols() throws {
        let table = try ExportTable(text: Self.text)

        #expect(table.libraries.keys.sorted() == ["libandroid.so", "libc.so", "libnativewindow.so"])
        #expect(table.libraries["libandroid.so"]?.needs == ["libc.so", "libnativewindow.so"])
        #expect(table.libraries["libc.so"]?.needs == [])
        #expect(table.libraries["libandroid.so"]?.symbols["AAsset_close"] == [
            .init(version: "LIBANDROID", isDefault: true, isStub: false)])
        #expect(table.libraries["libandroid.so"]?.symbols["ASensorManager_getInstance"] == [
            .init(version: "LIBANDROID", isDefault: true, isStub: true)])
        #expect(table.libraries["libandroid.so"]?.symbols["oldname"] == [
            .init(version: "LIBANDROID", isDefault: false, isStub: false)])
        #expect(table.libraries["libc.so"]?.symbols["memcpy"] == [.init(version: nil, isDefault: true, isStub: true)])
    }

    @Test(arguments: [
        "",
        "# minato export table 2\n@ libc.so\nneeds\n",
        "@ libc.so\nneeds\nmalloc\n",
        "# minato export table 1\nmalloc\n",
        "# minato export table 1\n@ libc.so\nneeds\nbad name\n",
        "# minato export table 1\n@ libc.so\nneeds\nbad\u{7}name\n",
        "# minato export table 1\n@ libc.so\nneeds\nname@@\n",
        "# minato export table 1\n@ libc.so\n@ libc.so\nneeds\n",
    ])
    func refusesWhatIsNotTheFormat(_ text: String) {
        #expect(throws: ExportTable.FormatError.self) { try ExportTable(text: text) }
    }

    @Test func aSymbolInTwoVersionsHasTwoDefinitions() throws {
        let table = try ExportTable(text: "# minato export table 1\n@ libx.so\nneeds\nf@@V2\nf@V1\n")

        #expect(table.libraries["libx.so"]?.symbols["f"]?.count == 2)
    }

    @Test func aGlobalLibraryIsMarkedOnItsHeaderLine() throws {
        let table = try ExportTable(text: "# minato export table 1\n@ libsigchain.so global\nneeds\nf\n@ libc.so\nneeds\ng\n")

        #expect(table.libraries["libsigchain.so"]?.isGlobal == true)
        #expect(table.libraries["libc.so"]?.isGlobal == false)
    }

    @Test func anythingElseOnAHeaderLineIsRefused() {
        #expect(throws: ExportTable.FormatError.self) {
            try ExportTable(text: "# minato export table 1\n@ libx.so globall\nneeds\n")
        }
        #expect(throws: ExportTable.FormatError.self) {
            try ExportTable(text: "# minato export table 1\n@ libx.so global extra\nneeds\n")
        }
    }
}
