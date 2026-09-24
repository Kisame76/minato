import Testing

@testable import APKSigning

@Suite("Re-sign planning")
struct ResignPlanTests {
    @Test func onlyEntriesWithConvertedCodeSignedByAnotherCertificate() {
        let candidates = [
            ResignCandidate(id: "old", signedWith: "bb", containerCount: 2),
            ResignCandidate(id: "current", signedWith: "aa", containerCount: 5),
            ResignCandidate(id: "unrecorded", signedWith: nil, containerCount: 1),
            ResignCandidate(id: "nothing converted", signedWith: "bb", containerCount: 0),
        ]
        #expect(entriesToResign(candidates, current: "aa") == ["old", "unrecorded"])
    }
}
