import Foundation
import Testing

@testable import APKLibrary

/// ADR-0055, step a3: which libraries cannot load, from the converter's refusals and from NEEDED names that nothing
/// gives, and which that makes unable to load in turn.
@Suite("Native findings")
struct NativeFindingsTests {
    typealias Facts = NativeFindings.Library

    static let publicLibraries: Set<String> = ["libc.so", "liblog.so", "libm.so", "libdl.so"]

    @Test func aNameNothingGivesCannotLoad() {
        let found = NativeFindings.failing(
            [Facts(name: "libgame.so", refusals: [], needed: ["libc.so", "libfmod.so"])],
            publicLibraries: Self.publicLibraries)

        #expect(found == ["libgame.so": [
            "it needs libfmod.so, which is not in the app and not one of Minato's libraries"]])
    }

    @Test func anNDKLibraryMinatoLacksIsNamedAsSuch() {
        let found = NativeFindings.failing(
            [Facts(name: "libgame.so", refusals: [], needed: ["libamidi.so"])], publicLibraries: Self.publicLibraries)

        #expect(found["libgame.so"] == ["it needs libamidi.so, an NDK library Minato does not have yet"])
    }

    @Test func libstdcxxIsOneOfThem() {
        let found = NativeFindings.failing(
            [Facts(name: "libgame.so", refusals: [], needed: ["libstdc++.so"])], publicLibraries: Self.publicLibraries)

        #expect(found["libgame.so"] == ["it needs libstdc++.so, an NDK library Minato does not have yet"])
    }

    @Test func aLibraryOfTheAppOrAPublicOneIsFound() {
        let found = NativeFindings.failing(
            [Facts(name: "libgame.so", refusals: [], needed: ["libc.so", "liblog.so", "libhelper.so"]),
             Facts(name: "libhelper.so", refusals: [], needed: ["libm.so"])], publicLibraries: Self.publicLibraries)

        #expect(found.isEmpty)
    }

    @Test func whatNeedsALibraryThatCannotLoadCannotLoadEither() {
        // libgame needs libmid, which needs libhelper, which the converter refuses.
        let found = NativeFindings.failing(
            [Facts(name: "libgame.so", refusals: [], needed: ["libmid.so"]),
             Facts(name: "libmid.so", refusals: [], needed: ["libhelper.so"]),
             Facts(name: "libhelper.so", refusals: ["text relocations"], needed: [])],
            publicLibraries: Self.publicLibraries)

        #expect(found["libhelper.so"] == ["text relocations"])
        // Each names the cause, so that one line read out says why (the request of 2026-10-30).
        #expect(found["libmid.so"] == ["it needs libhelper.so, which cannot load: text relocations"])
        #expect(found["libgame.so"] == [
            "it needs libmid.so, which cannot load: it needs libhelper.so, which cannot load: text relocations"])
    }

    @Test func theCauseIsTheFirstReasonOfTheOneItNeeds() {
        let found = NativeFindings.failing(
            [Facts(name: "libgame.so", refusals: [], needed: ["libhelper.so"]),
             Facts(name: "libhelper.so", refusals: ["text relocations", "svc"], needed: [])], publicLibraries: nil)

        #expect(found["libgame.so"] == ["it needs libhelper.so, which cannot load: text relocations"])
    }

    @Test func aCycleTerminates() {
        let found = NativeFindings.failing(
            [Facts(name: "liba.so", refusals: [], needed: ["libb.so"]),
             Facts(name: "libb.so", refusals: ["svc"], needed: ["liba.so"])], publicLibraries: Self.publicLibraries)

        #expect(Set(found.keys) == ["liba.so", "libb.so"])
    }

    @Test func withoutTheListOfPublicLibrariesOnlyTheRefusalsCount() {
        let found = NativeFindings.failing(
            [Facts(name: "libgame.so", refusals: [], needed: ["libfmod.so"]),
             Facts(name: "libhelper.so", refusals: ["svc"], needed: [])], publicLibraries: nil)

        #expect(found == ["libhelper.so": ["svc"]])
    }

    @Test func theClosureFollowsTheAppsOwnNames() {
        let closure = NativeFindings.closure(
            of: ["libgame.so"],
            over: [Facts(name: "libgame.so", refusals: [], needed: ["libmid.so", "libc.so"]),
                   Facts(name: "libmid.so", refusals: [], needed: ["libleaf.so"]),
                   Facts(name: "libleaf.so", refusals: [], needed: []),
                   Facts(name: "libother.so", refusals: [], needed: [])])

        #expect(closure == ["libgame.so", "libmid.so", "libleaf.so"])
    }

    @Test func readsTheListOfPublicLibraries() {
        let text = """
            # a comment
            libandroid.so
            libjnigraphics.so nopreload

            liblog.so
            """

        #expect(NativeFindings.publicLibraries(from: text) == ["libandroid.so", "libjnigraphics.so", "liblog.so"])
    }

    @Test func manyLibrariesThatNeedEachOtherAreFoundInLinearTime() {
        // 1000 libraries with 1000 NEEDED names each took 9 s with a scan for each pass.
        let names = (0..<1000).map { "lib\($0).so" }
        var libraries = names.map { Facts(name: $0, refusals: [], needed: names) }
        libraries[0].refusals = ["svc"]
        let start = ContinuousClock.now

        let found = NativeFindings.failing(libraries, publicLibraries: Self.publicLibraries)

        #expect(found.count == 1000)
        // A debug build under the other suites takes a few seconds; the scan for each pass took 68 s.
        #expect(ContinuousClock.now - start < .seconds(20))
    }

    @Test func aChainOfFailuresIsPassedOnInOrder() {
        // libc needs libb needs liba, which fails: libb first, then libc, each once.
        let found = NativeFindings.failing(
            [Facts(name: "libc.so", refusals: [], needed: ["libb.so"]),
             Facts(name: "libb.so", refusals: [], needed: ["liba.so", "liba.so"]),
             Facts(name: "liba.so", refusals: ["svc"], needed: [])], publicLibraries: nil)

        #expect(found["libb.so"] == ["it needs liba.so, which cannot load: svc"])
        #expect(found["libc.so"] == ["it needs libb.so, which cannot load: it needs liba.so, which cannot load: svc"])
    }
}
