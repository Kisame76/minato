import Foundation
import Testing

@testable import APKSession

/// ADR-0063 decision 2 and ADR-0064 decision 2: the host's side of APKRT_REQUEST_PERMISSION. Minato's own alert per
/// app first, iOS's prompt only after a yes there, the stored answer per app, Android's second denial final, and the
/// foreground's re-read of iOS's answer.
@Suite struct PermissionDeskTests {
    private typealias Step = PermissionDesk.Step

    private func desk(_ records: PermissionRecords = PermissionRecords()) -> PermissionDesk {
        PermissionDesk(records: records)
    }

    @Test func theRequestTextNamesTheGroupAndTheDetail() {
        #expect(PermissionAsk(text: "2\t0") == PermissionAsk(group: .microphone, detail: 0))
        #expect(PermissionAsk(text: "1\t2") == PermissionAsk(group: .location, detail: 2))
        #expect(PermissionAsk(text: "4\t0") == nil)
        #expect(PermissionAsk(text: "2") == nil)
        #expect(PermissionAsk(text: "2\t0\t1") == nil)
    }

    @Test func locationAsksMinatosAlertThenIOSAndAFineRequestIsGrantedFine() {
        var desk = desk()
        #expect(desk.requested(id: 5, text: "1\t1", ios: .notDetermined) == [.ask(id: 5, group: .location)])
        #expect(desk.allowed(id: 5, ios: .notDetermined) == [.askIOS(id: 5, group: .location)])
        #expect(desk.iosAnswered(id: 5, state: .granted) == [.answer(id: 5, group: 1, outcome: 1)])
        #expect(desk.records.location == PermissionRecord(outcome: 1, denials: 0))
        #expect(desk.records.microphone == PermissionRecord())
    }

    @Test func aCoarseOnlyRequestIsGrantedCoarseEvenWhenIOSGivesFullAccuracy() {
        var desk = desk()
        _ = desk.requested(id: 5, text: "1\t2", ios: .granted)
        #expect(desk.allowed(id: 5, ios: .granted) == [.answer(id: 5, group: 1, outcome: 2)])
        #expect(desk.records.location == PermissionRecord(outcome: 2, denials: 0))
    }

    @Test func aFineRequestUnderIOSsReducedAccuracyIsGrantedCoarse() {
        var desk = desk()
        _ = desk.requested(id: 5, text: "1\t1", ios: .notDetermined)
        _ = desk.allowed(id: 5, ios: .notDetermined)
        #expect(desk.iosAnswered(id: 5, state: .grantedReduced) == [.answer(id: 5, group: 1, outcome: 2)])
    }

    @Test func eachGroupKeepsItsOwnRecordAndDenials() {
        var desk = desk()
        _ = desk.requested(id: 1, text: "1\t1", ios: .granted)
        _ = desk.denied(id: 1)
        _ = desk.requested(id: 2, text: "1\t1", ios: .granted)
        _ = desk.denied(id: 2)
        #expect(desk.records.location == PermissionRecord(outcome: 0, denials: 2))
        #expect(desk.requested(id: 3, text: "2\t0", ios: .granted) == [.ask(id: 3, group: .microphone)])
        #expect(desk.requested(id: 4, text: "1\t1", ios: .granted).isEmpty)
        #expect(desk.allowed(id: 3, ios: .granted) == [.answer(id: 3, group: 2, outcome: 1),
                                                       .answer(id: 4, group: 1, outcome: 0)])
    }

    @Test func aStoredCoarseGrantAsksAgainForFine() {
        var desk = desk(PermissionRecords(location: PermissionRecord(outcome: 2, denials: 0)))
        #expect(desk.requested(id: 1, text: "1\t2", ios: .granted) == [.answer(id: 1, group: 1, outcome: 2)])
        #expect(desk.requested(id: 2, text: "1\t1", ios: .granted) == [.ask(id: 2, group: .location)])
        #expect(desk.allowed(id: 2, ios: .granted) == [.answer(id: 2, group: 1, outcome: 1)])
    }

    @Test func theStartSeedsEachGroupByItsOwnIOSAnswer() {
        let records = PermissionRecords(microphone: PermissionRecord(outcome: 1, denials: 0),
                                        location: PermissionRecord(outcome: 1, denials: 0))
        let both = desk(records).seeds { $0 == .microphone ? .granted : .grantedReduced }
        #expect(both == [.answer(id: 0, group: 2, outcome: 1), .answer(id: 0, group: 1, outcome: 2)])
        #expect(desk(records).seeds { $0 == .microphone ? .denied : .granted }
                == [.answer(id: 0, group: 1, outcome: 1)])
    }

    @Test func theForegroundFollowsIOSsLocationAccuracyAndRevoke() {
        var desk = desk(PermissionRecords(location: PermissionRecord(outcome: 1, denials: 0)))
        #expect(desk.foreground(ios: .granted).isEmpty)
        #expect(desk.foreground(ios: .grantedReduced) == [.answer(id: 0, group: 1, outcome: 2)])
        #expect(desk.records.location == PermissionRecord(outcome: 2, denials: 0))
        #expect(desk.foreground(ios: .granted).isEmpty)
        #expect(desk.foreground(ios: .denied) == [.answer(id: 0, group: 1, outcome: 0)])
        #expect(desk.records.location == PermissionRecord(outcome: 0, denials: 0))
    }

    @Test func theCameraIsAGroupOfItsOwnWithMinatosAlertAndIOSsPrompt() {
        var desk = desk()
        #expect(PermissionAsk(text: "3\t0") == PermissionAsk(group: .camera, detail: 0))
        #expect(desk.requested(id: 9, text: "3\t0", ios: .notDetermined) == [.ask(id: 9, group: .camera)])
        #expect(desk.allowed(id: 9, ios: .notDetermined) == [.askIOS(id: 9, group: .camera)])
        #expect(desk.iosAnswered(id: 9, granted: true) == [.answer(id: 9, group: 3, outcome: 1)])
        #expect(desk.records.camera == PermissionRecord(outcome: 1, denials: 0))
        #expect(desk.records.microphone == PermissionRecord() && desk.records.location == PermissionRecord())
    }

    @Test func theCameraIsSeededAndRevokedByItsOwnIOSAnswer() {
        var desk = desk(PermissionRecords(camera: PermissionRecord(outcome: 1, denials: 0)))
        #expect(desk.seeds { $0 == .camera ? .granted : .denied } == [.answer(id: 0, group: 3, outcome: 1)])
        #expect(desk.foreground { $0 == .camera ? .denied : .granted } == [.answer(id: 0, group: 3, outcome: 0)])
    }

    @Test func aFileWithoutALocationRecordKeepsItsMicrophoneRecord() throws {
        let folder = FileManager.default.temporaryDirectory.appending(path: "permission-desk-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: folder) }
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        let file = folder.appending(path: "permissions.json")
        try Data(#"{"microphone":{"outcome":1,"denials":0}}"#.utf8).write(to: file)
        #expect(PermissionRecords.load(from: file)
                == PermissionRecords(microphone: PermissionRecord(outcome: 1, denials: 0)))
    }

    @Test func aTextTheHostCannotReadIsDenied() {
        var desk = desk()
        #expect(desk.requested(id: 5, text: "x", ios: .granted) == [.answer(id: 5, group: 0, outcome: 0)])
    }

    @Test func theMicrophoneAsksMinatosAlertFirst() {
        var desk = desk()
        #expect(desk.requested(id: 7, text: "2\t0", ios: .notDetermined) == [.ask(id: 7, group: .microphone)])
    }

    @Test func aYesAsksIOSWhenIOSHasNotAnsweredAndItsAnswerIsTheResult() {
        var desk = desk()
        _ = desk.requested(id: 7, text: "2\t0", ios: .notDetermined)
        #expect(desk.allowed(id: 7, ios: .notDetermined) == [.askIOS(id: 7, group: .microphone)])
        #expect(desk.iosAnswered(id: 7, granted: true) == [.answer(id: 7, group: 2, outcome: 1)])
        #expect(desk.records.microphone == PermissionRecord(outcome: 1, denials: 0))
    }

    @Test func aYesWhenIOSHasGrantedGrantsAtOnce() {
        var desk = desk()
        _ = desk.requested(id: 7, text: "2\t0", ios: .granted)
        #expect(desk.allowed(id: 7, ios: .granted) == [.answer(id: 7, group: 2, outcome: 1)])
    }

    @Test func aNoAtIOSsPromptIsADenial() {
        var desk = desk()
        _ = desk.requested(id: 7, text: "2\t0", ios: .notDetermined)
        _ = desk.allowed(id: 7, ios: .notDetermined)
        #expect(desk.iosAnswered(id: 7, granted: false) == [.answer(id: 7, group: 2, outcome: 0)])
        #expect(desk.records.microphone == PermissionRecord(outcome: 0, denials: 1))
    }

    @Test func aNoInMinatosAlertNeverReachesIOSAndTheSecondDenialIsFinal() {
        var desk = desk()
        _ = desk.requested(id: 1, text: "2\t0", ios: .notDetermined)
        #expect(desk.denied(id: 1) == [.answer(id: 1, group: 2, outcome: 0)])
        #expect(desk.requested(id: 2, text: "2\t0", ios: .notDetermined) == [.ask(id: 2, group: .microphone)])
        #expect(desk.denied(id: 2) == [.answer(id: 2, group: 2, outcome: 0)])
        #expect(desk.records.microphone == PermissionRecord(outcome: 0, denials: 2))
        #expect(desk.requested(id: 3, text: "2\t0", ios: .notDetermined) == [.answer(id: 3, group: 2, outcome: 0)])
    }

    @Test func aGrantClearsTheDenials() {
        var desk = desk()
        _ = desk.requested(id: 1, text: "2\t0", ios: .granted)
        _ = desk.denied(id: 1)
        _ = desk.requested(id: 2, text: "2\t0", ios: .granted)
        _ = desk.allowed(id: 2, ios: .granted)
        #expect(desk.records.microphone == PermissionRecord(outcome: 1, denials: 0))
    }

    @Test func whenIOSHasDeniedMinatoAnswersNoWithoutItsAlert() {
        var desk = desk()
        #expect(desk.requested(id: 4, text: "2\t0", ios: .denied) == [.answer(id: 4, group: 2, outcome: 0)])
        #expect(desk.records.microphone == PermissionRecord(outcome: 0, denials: 1))
    }

    @Test func aStoredGrantIsAnsweredAtOnceWhileIOSStillGrants() {
        var desk = desk(PermissionRecords(microphone: PermissionRecord(outcome: 1, denials: 0)))
        #expect(desk.requested(id: 4, text: "2\t0", ios: .granted) == [.answer(id: 4, group: 2, outcome: 1)])
        #expect(desk.requested(id: 5, text: "2\t0", ios: .notDetermined) == [.ask(id: 5, group: .microphone)])
    }

    @Test func oneRequestAtATimeInTheirOrder() {
        var desk = desk()
        #expect(desk.requested(id: 1, text: "2\t0", ios: .granted) == [.ask(id: 1, group: .microphone)])
        #expect(desk.requested(id: 2, text: "2\t0", ios: .granted).isEmpty)
        #expect(desk.requested(id: 1, text: "2\t0", ios: .granted).isEmpty)
        #expect(desk.denied(id: 2).isEmpty)
        #expect(desk.denied(id: 1) == [.answer(id: 1, group: 2, outcome: 0), .ask(id: 2, group: .microphone)])
    }

    @Test func aWaitingRequestAskedAgainAfterItsTurnTakesTheStoredAnswer() {
        var desk = desk()
        _ = desk.requested(id: 1, text: "2\t0", ios: .granted)
        _ = desk.requested(id: 2, text: "2\t0", ios: .granted)
        #expect(desk.allowed(id: 1, ios: .granted) == [.answer(id: 1, group: 2, outcome: 1),
                                                       .answer(id: 2, group: 2, outcome: 1)])
    }

    @Test func aCancelledRequestIsDismissedOrDroppedAndNeverAnswered() {
        var desk = desk()
        _ = desk.requested(id: 1, text: "2\t0", ios: .granted)
        _ = desk.requested(id: 2, text: "2\t0", ios: .granted)
        _ = desk.requested(id: 3, text: "2\t0", ios: .granted)
        #expect(desk.cancelled(id: 2).isEmpty)
        #expect(desk.cancelled(id: 1) == [.dismiss(id: 1), .ask(id: 3, group: .microphone)])
        #expect(desk.allowed(id: 1, ios: .granted).isEmpty)
        #expect(desk.records == PermissionRecords())
    }

    @Test func aCancelWhileIOSAsksDropsIOSsLaterAnswer() {
        var desk = desk()
        _ = desk.requested(id: 1, text: "2\t0", ios: .notDetermined)
        _ = desk.allowed(id: 1, ios: .notDetermined)
        #expect(desk.cancelled(id: 1) == [.dismiss(id: 1)])
        #expect(desk.iosAnswered(id: 1, granted: true).isEmpty)
    }

    @Test func theStartSeedsAStoredGrantThatIOSStillGives() {
        let granted = PermissionRecords(microphone: PermissionRecord(outcome: 1, denials: 0))
        #expect(desk(granted).seeds(ios: .granted) == [.answer(id: 0, group: 2, outcome: 1)])
        #expect(desk(granted).seeds(ios: .denied).isEmpty)
        #expect(desk().seeds(ios: .granted).isEmpty)
    }

    @Test func aRevokeInIOSSettingsIsADenialAtTheForegroundWithoutACount() {
        var desk = desk(PermissionRecords(microphone: PermissionRecord(outcome: 1, denials: 0)))
        #expect(desk.foreground(ios: .granted).isEmpty)
        #expect(desk.foreground(ios: .denied) == [.answer(id: 0, group: 2, outcome: 0)])
        #expect(desk.records.microphone == PermissionRecord(outcome: 0, denials: 0))
        #expect(desk.foreground(ios: .denied).isEmpty)
    }

    @Test func theRecordsAreKeptInAFileAndAMissingOneIsEmpty() throws {
        let folder = FileManager.default.temporaryDirectory.appending(path: "permission-desk-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: folder) }
        let file = folder.appending(path: "permissions.json")
        #expect(PermissionRecords.load(from: file) == PermissionRecords())
        let records = PermissionRecords(microphone: PermissionRecord(outcome: 0, denials: 2))
        try records.save(to: file)
        #expect(PermissionRecords.load(from: file) == records)
        try Data("not json".utf8).write(to: file)
        #expect(PermissionRecords.load(from: file) == PermissionRecords())
    }
}
