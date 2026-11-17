import APKKit
import Foundation
import Testing

@testable import APKLibrary

/// ADR-0055, step a4: which undefined symbols of a library nothing defines, and which only a stub does, binding as
/// linker64 does (breadth first through what the library needs; the runtime's rules for versions).
@Suite("Symbol binding")
struct SymbolBindingTests {
    typealias Reference = ELFSymbols.Reference

    static let table = try! ExportTable(text: """
        # minato export table 1
        @ libandroid.so
        needs libc.so libnativewindow.so
        AAsset_close@@LIBANDROID
        ASensorManager_getInstance@@LIBANDROID!
        hidden_old@LIBANDROID
        @ libc.so
        needs
        malloc
        free
        memcpy!
        versioned@@LIBC
        versioned@LIBC_OLD
        @ libnativewindow.so
        needs libc.so
        ANativeWindow_release
        @ libplain.so
        needs
        anything
        """)

    /// The library under test is "libtest.so", beside the app's other libraries.
    static func bind(_ refs: [Reference], needed: [String], own: Set<String> = [],
                     app: [String: NativeFindings.AppLibrary] = [:],
                     table: ExportTable = table) -> NativeFindings.Binding {
        var libraries = app
        libraries["libtest.so"] = NativeFindings.AppLibrary(defined: own, needed: needed)
        return NativeFindings.Binder(app: libraries, table: table).bind("libtest.so", references: refs, own: own)
    }

    /// One reference of a library that needs libc.so: its name, whether it is weak, then what is missing and what
    /// only a stub defines.
    @Test(arguments: [
        ("malloc", false, [], [], "a symbol in a needed library is bound"),
        ("nope", false, ["nope"], [], "a symbol nothing defines is missing"),
        ("memcpy", false, [], ["memcpy"], "a symbol only a stub defines is a stub"),
        ("nope", true, [], [], "a weak reference may stay unresolved"),
        ("AAsset_close", false, ["AAsset_close"], [], "a library it does not need is not searched"),
    ] as [(String, Bool, [String], [String], String)])
    func oneReferenceBindsAsLinker64Does(name: String, isWeak: Bool, missing: [String], stubs: [String], rule: String) {
        let result = Self.bind([.init(name: name, version: nil, isWeak: isWeak)], needed: ["libc.so"])

        #expect(result == NativeFindings.Binding(missing: missing, stubs: stubs), "\(rule)")
    }

    @Test func itsOwnAndTheAppsLibrariesDefineToo() {
        let app = ["libhelper.so": NativeFindings.AppLibrary(defined: ["helper_call"], needed: [])]
        let result = Self.bind(
            [.init(name: "own_symbol", version: nil, isWeak: false),
             .init(name: "helper_call", version: nil, isWeak: false)],
            needed: ["libhelper.so"], own: ["own_symbol"], app: app)

        #expect(result.missing.isEmpty)
    }

    @Test func anUnversionedReferenceBindsToTheDefaultVersionNotAHiddenOne() {
        let hidden = Self.bind([.init(name: "hidden_old", version: nil, isWeak: false)], needed: ["libandroid.so"])
        let defaulted = Self.bind([.init(name: "AAsset_close", version: nil, isWeak: false)], needed: ["libandroid.so"])

        #expect(hidden.missing == ["hidden_old"])
        #expect(defaulted.missing.isEmpty)
    }

    @Test func aVersionedReferenceBindsToThatVersionHiddenOrNot() {
        let old = Self.bind([.init(name: "versioned", version: "LIBC_OLD", isWeak: false)], needed: ["libc.so"])
        let current = Self.bind([.init(name: "versioned", version: "LIBC", isWeak: false)], needed: ["libc.so"])

        #expect(old.missing.isEmpty && current.missing.isEmpty)
    }

    @Test func aVersionTheLibraryLacksBindsOnlyToABaseVersionName() {
        // libc has versions but not LIBC_NEW: malloc is unversioned there, so it binds; versioned is not.
        let base = Self.bind([.init(name: "malloc", version: "LIBC_NEW", isWeak: false)], needed: ["libc.so"])
        let versioned = Self.bind([.init(name: "versioned", version: "LIBC_NEW", isWeak: false)], needed: ["libc.so"])

        #expect(base.missing.isEmpty)
        #expect(versioned.missing == ["versioned"])
    }

    @Test func aLibraryWithoutVersionTablesBindsEveryName() {
        let result = Self.bind([.init(name: "anything", version: "WHATEVER", isWeak: false)], needed: ["libplain.so"])

        #expect(result.missing.isEmpty)
    }

    @Test func manyLibrariesAndSymbolsAreBoundInLinearTime() {
        let names = (0..<300).map { "lib\($0).so" }
        var app: [String: NativeFindings.AppLibrary] = [:]
        for name in names {
            app[name] = NativeFindings.AppLibrary(defined: Set((0..<300).map { "\(name)_f\($0)" }), needed: names)
        }
        let refs = (0..<300).map { Reference(name: "lib299.so_f\($0)", version: nil, isWeak: false) }
        let start = ContinuousClock.now

        let result = Self.bind(refs, needed: names, app: app)

        #expect(result.missing.isEmpty)
        #expect(ContinuousClock.now - start < .seconds(20))
    }

    @Test func aSystemLibraryIsSearchedButNotWhatItNeeds() {
        // libandroid needs libnativewindow, but an app library that needs only libandroid does not see it.
        let result = Self.bind([.init(name: "ANativeWindow_release", version: nil, isWeak: false)],
                               needed: ["libandroid.so"])

        #expect(result.missing == ["ANativeWindow_release"])
    }

    @Test func aSystemLibraryAnotherAppLibraryNeedsIsInTheGroup() {
        let app = ["libhelper.so": NativeFindings.AppLibrary(defined: [], needed: ["libnativewindow.so"])]
        let result = Self.bind([.init(name: "ANativeWindow_release", version: nil, isWeak: false)],
                               needed: ["libhelper.so"], app: app)

        #expect(result.missing.isEmpty)
    }

    @Test func aGlobalLibraryBindsWithoutBeingNeeded() throws {
        let table = try ExportTable(text: "# minato export table 1\n@ libsigchain.so global\nneeds\nSigchainOnly\n")
        let result = Self.bind([.init(name: "SigchainOnly", version: nil, isWeak: false)], needed: [], table: table)

        #expect(result.missing.isEmpty)
    }

    @Test func aSymbolASiblingDefinesBindsWhenOneRootLoadsBoth() {
        // libA needs libB and libC; libB uses a symbol only libC defines. Loaded through libA, Android binds it.
        let app = [
            "libA.so": NativeFindings.AppLibrary(defined: [], needed: ["libB.so", "libC.so"]),
            "libB.so": NativeFindings.AppLibrary(defined: [], needed: []),
            "libC.so": NativeFindings.AppLibrary(defined: ["from_c"], needed: []),
        ]
        let binding = NativeFindings.Binder(app: app, table: Self.table).bind(
            "libB.so", references: [.init(name: "from_c", version: nil, isWeak: false)], own: [])

        #expect(binding.missing.isEmpty)
    }

    @Test func aSymbolNoLibraryOfTheSameLoadDefinesIsStillMissing() {
        let app = [
            "libA.so": NativeFindings.AppLibrary(defined: [], needed: ["libB.so"]),
            "libB.so": NativeFindings.AppLibrary(defined: [], needed: []),
            "libOther.so": NativeFindings.AppLibrary(defined: ["from_other"], needed: []),
        ]
        let binding = NativeFindings.Binder(app: app, table: Self.table).bind(
            "libB.so", references: [.init(name: "from_other", version: nil, isWeak: false)], own: [])

        #expect(binding.missing == ["from_other"])
    }

    @Test func aStubIsReportedOnlyWhenNoLibraryOfTheGroupHasTheRealOne() throws {
        let table = try ExportTable(text: """
            # minato export table 1
            @ libx.so
            needs
            f!
            @ liby.so
            needs
            f
            """)
        let both = Self.bind([.init(name: "f", version: nil, isWeak: false)], needed: ["libx.so", "liby.so"], table: table)
        let stubOnly = Self.bind([.init(name: "f", version: nil, isWeak: false)], needed: ["libx.so"], table: table)

        #expect(both.stubs.isEmpty && both.missing.isEmpty)
        #expect(stubOnly.stubs == ["f"])
    }

    @Test func everyLibraryOfADenseAppIsBoundInTime() {
        // 300 libraries that need each other, 3000 references each: 16.7 s with a search for each reference.
        let names = (0..<300).map { "lib\($0).so" }
        var app: [String: NativeFindings.AppLibrary] = [:]
        for name in names {
            app[name] = NativeFindings.AppLibrary(defined: Set((0..<3000).map { "\(name)_f\($0)" }), needed: names)
        }
        let refs = (0..<3000).map { Reference(name: "lib299.so_f\($0)", version: nil, isWeak: false) }
        let start = ContinuousClock.now

        let binder = NativeFindings.Binder(app: app, table: Self.table)
        for name in names { #expect(binder.bind(name, references: refs, own: []).missing.isEmpty) }

        #expect(ContinuousClock.now - start < .seconds(10))
    }

    @Test func aThousandLibrariesWithAThousandNeededNamesEachAreBoundInTime() {
        let names = (0..<1000).map { "lib\($0).so" }
        var app: [String: NativeFindings.AppLibrary] = [:]
        for name in names { app[name] = NativeFindings.AppLibrary(defined: ["\(name)_f"], needed: names) }
        let refs = (0..<1000).map { Reference(name: "lib\($0).so_f", version: nil, isWeak: false) }
        let start = ContinuousClock.now

        let binder = NativeFindings.Binder(app: app, table: Self.table)
        for name in names { #expect(binder.bind(name, references: refs, own: []).missing.isEmpty) }

        #expect(ContinuousClock.now - start < .seconds(20))
    }

    @Test func aSystemLibrarysDependencyIsOpenWhenAnyAppLibraryNeedsItDirectly() {
        // libx needs only libandroid (which needs libnativewindow); liby, of another part, needs libnativewindow
        // directly: Android binds libx's ANativeWindow_release through libandroid's dependency.
        let app = [
            "libx.so": NativeFindings.AppLibrary(defined: [], needed: ["libandroid.so"]),
            "liby.so": NativeFindings.AppLibrary(defined: [], needed: ["libnativewindow.so"]),
        ]
        let binding = NativeFindings.Binder(app: app, table: Self.table).bind(
            "libx.so", references: [.init(name: "ANativeWindow_release", version: nil, isWeak: false)], own: [])

        #expect(binding.missing.isEmpty)
    }
}
