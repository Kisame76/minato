import APKLibrary
import APKSession
import SwiftUI
import UniformTypeIdentifiers

extension UTType {
    static let apk = UTType(importedAs: "com.android.package-archive")
    /// project.yml's imported type for bundletool's, SAI's and App Manager's .apks (ADR-0049 a3).
    static let apkSet = UTType(importedAs: "io.github.kisame76.minato.apks")
    /// project.yml's imported type for APKPure's .xapk (ADR-0049 c2).
    static let xapk = UTType(importedAs: "io.github.kisame76.minato.xapk")
    /// project.yml's imported type for APKMirror's .apkm (ADR-0049 e).
    static let apkm = UTType(importedAs: "io.github.kisame76.minato.apkm")
    /// project.yml's imported type for an app's expansion file, .obb (ADR-0049 d).
    static let obb = UTType(importedAs: "io.github.kisame76.minato.obb")
}

/// The library, laid out like a folder in the Files app: icon grid, a "…" menu with Select, +, and the search above the
/// grid (the app navigation's design, section 1).
/// Tapping an app starts it; touch and hold offers Get Info and Delete.
struct LibraryView: View {
    @Environment(LibraryModel.self) private var model
    @Environment(SigningCoordinator.self) private var signing
    @AppStorage(RootTab.storageKey) private var tab = RootTab.library
    @State private var shownEntry: LibraryEntry?
    @State private var blockedLaunch: BlockedLaunch?
    @State private var isImporting = false
    @State private var isSelecting = false
    @State private var selection: Set<LibraryItem.ID> = []
    @State private var deletion: Deletion?
    @State private var query = ""

    private let columns = [GridItem(.adaptive(minimum: 104, maximum: 160), spacing: 8, alignment: .top)]

    var body: some View {
        NavigationStack {
            content
                .navigationTitle(title)
                .navigationBarTitleDisplayMode(.inline)
                .searchable(text: $query)
                .toolbar { toolbar }
                // Select's bottom bar takes the tab bar's place, as in Files.
                .toolbar(isSelecting ? .hidden : .automatic, for: .tabBar)
                // An app's APKs, or their folder, as one pick (ADR-0048, decision 1); a set of APKs in one .apks or .zip
                // (ADR-0049 a3), in an .xapk with its OBBs (c2) or an .apkm (e), and an app's OBB on its own (d).
                .fileImporter(isPresented: $isImporting,
                              allowedContentTypes: [.apk, .apkSet, .xapk, .apkm, .obb, .zip, .folder],
                              allowsMultipleSelection: true) { result in
                    guard case .success(let urls) = result else { return }
                    Task { await model.open(urls) }
                }
                .sheet(item: $shownEntry) { entry in
                    AppDetailView(entry: entry)
                }
                .alert(launchAlertTitle, isPresented: launchAlertShown, presenting: blockedLaunch) { blocked in
                    Button("Get Info") { shownEntry = blocked.entry }
                    Button("OK", role: .cancel) {}
                } message: { blocked in
                    Text(Texts.launchProblem(of: blocked.entry, reason: blocked.reason).message)
                }
                // The restart did not work (ADR-0032, decision 2): the launch request stays, so opening
                // Minato again starts the app.
                .alert(restartAlertTitle, isPresented: restartAlertShown) {
                    Button("OK") { exit(0) }
                }
                // ADR-0034, decision 5: a file that would not run, and the question before an older version or
                // another signer replaces the installed app, for every set of a unit that asks (ADR-0048, F7).
                .alert(model.updateRejected?.title ?? "", isPresented: rejectionShown) {
                    Button("OK") {}
                } message: {
                    Text(model.updateRejected?.message ?? "")
                }
                .alert(questionTitle, isPresented: questionShown, presenting: model.questions.first) { question in
                    let texts = Texts.unitQuestion(question)
                    Button(texts.confirm, role: question.replacesAny ? .destructive : nil) {
                        Task { await model.answer(question, replace: true) }
                    }
                    Button(texts.decline, role: .cancel) { Task { await model.answer(question, replace: false) } }
                } message: { question in
                    Text(Texts.unitQuestion(question).message)
                }
                // ADR-0049 d: the app an OBB belongs to, one button per app that can take it.
                .alert(obbQuestionTitle, isPresented: obbQuestionShown, presenting: model.obbQuestions.first) { question in
                    ForEach(question.apps, id: \.id) { app in
                        Button(app.displayName) { Task { await model.assign(question, to: app) } }
                    }
                    Button(question.apps.isEmpty ? "OK" : "Don’t Import", role: .cancel) {
                        Task { await model.assign(question, to: nil) }
                    }
                } message: { question in
                    Text(Texts.obbQuestion(question).message)
                }
                .alert("Error", isPresented: errorShown) {
                    Button("OK") { model.errorMessage = nil }
                } message: {
                    Text(model.errorMessage ?? "")
                }
        }
    }

    @ViewBuilder private var content: some View {
        if model.items.isEmpty {
            emptyLibrary
        } else {
            ScrollView {
                LazyVGrid(columns: columns, spacing: 24) {
                    ForEach(visibleItems) { item in
                        tile(item)
                    }
                }
                .padding(.horizontal)
                .padding(.top, 8)
            }
            // Like Files: content scrolls under a frosted bar with a clear edge.
            .scrollEdgeEffectStyle(.hard, for: .top)
            .overlay(alignment: .bottom) {
                if let note = model.note {
                    Text(note)
                        .font(.subheadline)
                        .multilineTextAlignment(.center)
                        .padding(.horizontal, 16)
                        .padding(.vertical, 10)
                        .glassEffect()
                        .padding()
                        .transition(.move(edge: .bottom).combined(with: .opacity))
                }
            }
            .animation(.default, value: model.note)
            .overlay {
                if visibleItems.isEmpty {
                    ContentUnavailableView.search(text: query)
                }
            }
        }
    }

    private func tile(_ item: LibraryItem) -> some View {
        AppCard(item: item, showsSelection: isSelecting && item.entry != nil, isSelected: selection.contains(item.id))
            .contentShape(.rect)
            .onTapGesture { tap(item) }
            .contextMenu {
                if !isSelecting, let entry = item.entry {
                    Button("Get Info", systemImage: "info.circle") { shownEntry = entry }
                    Divider()
                    Button("Delete", systemImage: "trash", role: .destructive) {
                        deletion = Deletion(origin: .tile(item.id), entries: [entry])
                    }
                }
            }
            .deletionDialog(isPresented: deletionShown(from: .tile(item.id)), entries: deletion?.entries, perform: delete)
    }

    @ToolbarContentBuilder private var toolbar: some ToolbarContent {
        if isSelecting {
            ToolbarItem(placement: .confirmationAction) {
                Button("Done") { endSelection() }
            }
            ToolbarItem(placement: .bottomBar) {
                Button(allSelected ? "Deselect All" : "Select All") { toggleAll() }
            }
            ToolbarSpacer(placement: .bottomBar)
            ToolbarItem(placement: .bottomBar) {
                Button("Delete", systemImage: "trash", role: .destructive) {
                    deletion = Deletion(origin: .selection, entries: selectedEntries)
                }
                .disabled(selectedEntries.isEmpty)
                .deletionDialog(isPresented: deletionShown(from: .selection), entries: deletion?.entries, perform: delete)
            }
        } else {
            // Without a back button the leading side would stay empty; like Clock (Edit left, + right).
            ToolbarItem(placement: .topBarLeading) {
                Menu("More", systemImage: "ellipsis") {
                    Button("Select", systemImage: "checkmark.circle") { isSelecting = true }
                        .disabled(!model.items.contains { $0.entry != nil })
                }
            }
            ToolbarItem(placement: .topBarTrailing) {
                Button("Add APK", systemImage: "plus") { isImporting = true }
            }
        }
    }

    private var emptyLibrary: some View {
        ContentUnavailableView {
            Label("No Apps", systemImage: "square.grid.2x2")
        } description: {
            Text("Tap + to add APK files, or open an APK with Minato from another app.")
        }
    }

    // MARK: - Search and selection

    private var visibleItems: [LibraryItem] {
        let query = query.trimmingCharacters(in: .whitespaces)
        guard !query.isEmpty else { return model.items }
        return model.items.filter { item in
            item.title.localizedStandardContains(query)
                || item.entry?.report?.packageName.localizedStandardContains(query) == true
        }
    }

    private var title: String {
        guard isSelecting else { return "Library" }
        return switch selectedEntries.count {
        case 0: "Select Items"
        case 1: "1 Item Selected"
        case let count: "\(count) Items Selected"
        }
    }

    /// Pending files are still being imported and cannot be selected.
    private var selectableIDs: [LibraryItem.ID] {
        visibleItems.filter { $0.entry != nil }.map(\.id)
    }

    private var selectedEntries: [LibraryEntry] {
        model.items.filter { selection.contains($0.id) }.compactMap(\.entry)
    }

    private var allSelected: Bool {
        !selectableIDs.isEmpty && selectableIDs.allSatisfy(selection.contains)
    }

    private func tap(_ item: LibraryItem) {
        // Pending files are still being imported: nothing to start or select yet.
        guard let entry = item.entry else { return }
        guard isSelecting else {
            launch(entry)
            return
        }
        if selection.contains(item.id) {
            selection.remove(item.id)
        } else {
            selection.insert(item.id)
        }
    }

    /// Starts the app, or explains why not, or opens the signing flow when no certificate signs yet
    /// (ADR-0032, decision 3).
    private func launch(_ entry: LibraryEntry) {
        Task {
            switch await model.launch(entry) {
            case nil:
                break
            case .openSigning:
                if signing.fetchesCertificate {
                    signing.showsSetup = true
                } else {
                    tab = .settings
                }
            case .explain(let entry, let reason):
                blockedLaunch = BlockedLaunch(entry: entry, reason: reason)
            }
        }
    }

    private func toggleAll() {
        if allSelected {
            selection.removeAll()
        } else {
            selection.formUnion(selectableIDs)
        }
    }

    private func endSelection() {
        isSelecting = false
        selection.removeAll()
    }

    private func delete(_ entries: [LibraryEntry]) {
        Task {
            await model.remove(entries)
            endSelection()
        }
    }

    // MARK: - Dialogs

    /// What the user is about to delete, and where they asked for it, so the dialog points there.
    private struct Deletion {
        enum Origin: Equatable {
            case selection
            case tile(LibraryItem.ID)
        }

        let origin: Origin
        let entries: [LibraryEntry]
    }

    private func deletionShown(from origin: Deletion.Origin) -> Binding<Bool> {
        Binding(get: { deletion?.origin == origin }, set: { if !$0 { deletion = nil } })
    }

    /// An app that a tap did not start, and why.
    private struct BlockedLaunch {
        let entry: LibraryEntry
        let reason: LaunchDecision.Reason
    }

    private var launchAlertTitle: String {
        blockedLaunch.map { Texts.launchProblem(of: $0.entry, reason: $0.reason).title } ?? ""
    }

    private var launchAlertShown: Binding<Bool> {
        Binding(get: { blockedLaunch != nil }, set: { if !$0 { blockedLaunch = nil } })
    }

    private var restartAlertTitle: String {
        model.restartFailedFor.map { "Open Minato again to start “\($0.displayName)”." } ?? ""
    }

    private var restartAlertShown: Binding<Bool> {
        Binding(get: { model.restartFailedFor != nil }, set: { if !$0 { model.restartFailedFor = nil } })
    }

    private var rejectionShown: Binding<Bool> {
        Binding(get: { model.updateRejected != nil }, set: { if !$0 { model.updateRejected = nil } })
    }

    private var questionTitle: String {
        model.questions.first.map { Texts.unitQuestion($0).title } ?? ""
    }

    /// Dismissing it without a button leaves the question: the unit waits, and the next launch asks again.
    private var questionShown: Binding<Bool> {
        Binding(get: { !model.questions.isEmpty }, set: { _ in })
    }

    private var obbQuestionTitle: String {
        model.obbQuestions.first.map { Texts.obbQuestion($0).title } ?? ""
    }

    /// Dismissing it without a button leaves the question, as for an update's.
    private var obbQuestionShown: Binding<Bool> {
        Binding(get: { !model.obbQuestions.isEmpty }, set: { _ in })
    }

    private var errorShown: Binding<Bool> {
        Binding(get: { model.errorMessage != nil }, set: { if !$0 { model.errorMessage = nil } })
    }
}

private extension View {
    /// Asks before deleting: app data cannot be restored.
    func deletionDialog(isPresented: Binding<Bool>, entries: [LibraryEntry]?, perform delete: @escaping ([LibraryEntry]) -> Void) -> some View {
        let title = entries.map { $0.count == 1 ? "Delete “\($0[0].displayName)”?" : "Delete \($0.count) Items?" } ?? ""
        return confirmationDialog(title, isPresented: isPresented, titleVisibility: .visible, presenting: entries) { entries in
            Button("Delete", role: .destructive) { delete(entries) }
        } message: { entries in
            Text(entries.count == 1
                ? "Its installed files and app data will be deleted. The original APK is kept."
                : "Their installed files and app data will be deleted. The original APKs are kept.")
        }
    }
}
