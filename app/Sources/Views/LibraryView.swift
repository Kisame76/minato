import APKLibrary
import APKSession
import SwiftUI
import UniformTypeIdentifiers
#if DEBUG
import os
#endif

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

/// The library's two views, as a folder's in Files.
enum LibraryLayout: String {
    case icons, list
}

/// The library, laid out like a folder in the Files app: icon grid or list, a "…" menu with Select, the view and the
/// sort order, +, and the search above the grid (the app navigation's design, sections 1 and 3). View and sort order
/// are the device's preferences. Tapping an app starts it; touch and hold offers Get Info, the favorite and Delete
/// (AppTile), and in the manual order moving the finger drags the app into another place.
struct LibraryView: View {
    @Environment(LibraryModel.self) private var model
    @Environment(TabBarVisibility.self) private var tabBar
    @State private var actions = TileActions()
    @State private var isImporting = false
    @State private var isSelecting = false
    @State private var selection: Set<LibraryItem.ID> = []
    /// The selected apps the bottom bar's Delete asks about.
    @State private var selectionDeletion: [LibraryEntry]?
    @State private var query = ""
    @State private var width: CGFloat = 0
    @AppStorage("libraryLayout") private var layout = LibraryLayout.icons
    @AppStorage("librarySort") private var sort = LibrarySort.manual
    @AppStorage("librarySortAscending") private var ascending = true
    @AppStorage("libraryFilter") private var filter = LibraryFilter.all

    var body: some View {
        NavigationStack {
            content
                .navigationTitle(title)
                .navigationBarTitleDisplayMode(.inline)
                .searchable(text: $query)
                .toolbar { toolbar }
                // Select's bottom bar takes the tab bar's place, as in Files (RootView's controller hides it).
                .onChange(of: isSelecting) { _, selecting in tabBar.hidden = selecting }
                // An app's APKs, or their folder, as one pick (ADR-0048, decision 1); a set of APKs in one .apks or .zip
                // (ADR-0049 a3), in an .xapk with its OBBs (c2) or an .apkm (e), and an app's OBB on its own (d).
                .fileImporter(isPresented: $isImporting,
                              allowedContentTypes: [.apk, .apkSet, .xapk, .apkm, .obb, .zip, .folder],
                              allowsMultipleSelection: true) { result in
                    guard case .success(let urls) = result else { return }
                    Task { await model.open(urls) }
                }
                .tileActions(actions)
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
        if model.entries.isEmpty && model.pending.isEmpty {
            emptyLibrary
        } else {
            // Built once per change: each Select tap renders the library again.
            let items = visibleItems
            Group {
                switch layout {
                case .icons:
                    ScrollView {
                        // 4 apps side by side with Home Screen sized icons, as on Home (the navigation design,
                        // section 3).
                        TileGrid(items: items, width: width,
                                 move: movesApps ? { move($0, before: $1) } : nil) { item, iconSize in
                            tile(item, iconSize: iconSize)
                        }
                        .padding(.top, 8)
                    }
                case .list:
                    List {
                        ForEach(items) { tile($0, asRow: true) }
                            .reorderableItems()
                    }
                    .listStyle(.plain)
                    .reordering(LibraryItem.self, move: movesApps ? { move($0, before: $1) } : nil)
                }
            }
            .onGeometryChange(for: CGFloat.self) { $0.size.width } action: { width = $0 }
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
                if items.isEmpty {
                    noMatches
                }
            }
        }
    }

    /// A search finds nothing, or the filter shows no app.
    @ViewBuilder private var noMatches: some View {
        if !trimmedQuery.isEmpty {
            ContentUnavailableView.search(text: query)
        } else {
            let empty = Texts.filterEmpty(filter)
            ContentUnavailableView(empty.title, systemImage: filterSymbol, description: Text(empty.description))
        }
    }

    private func tile(_ item: LibraryItem, iconSize: CGFloat = 60, asRow: Bool = false) -> some View {
        AppTile(item: item, actions: actions, asRow: asRow, iconSize: iconSize, isSelecting: isSelecting,
                isSelected: selection.contains(item.id)) {
            #if DEBUG
            SelectTiming.tap()
            #endif
            if selection.contains(item.id) {
                selection.remove(item.id)
            } else {
                selection.insert(item.id)
            }
        }
        .accessibilityActions { moveActions(item) }
    }

    /// VoiceOver's way to the manual order, where dragging is: the steps this app can take.
    @ViewBuilder private func moveActions(_ item: LibraryItem) -> some View {
        if movesApps, item.entry != nil {
            let ids = model.entries.map(\.id)
            ForEach(LibraryMoveStep.allCases, id: \.self) { step in
                if let destination = step.move(item.id, in: ids) {
                    Button(Texts.moveName(step)) {
                        move([item.id], before: destination.before)
                        AccessibilityNotification.Announcement(
                            Texts.moved(position: destination.position, of: ids.count)).post()
                    }
                }
            }
        }
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
                // For everything selected; when all are favorites already, it removes them instead.
                let candidates = favoriteCandidates
                let adds = candidates.isEmpty
                    || Library.favoriteAdds(candidates.map(\.id), favorites: model.favorites)
                Button(adds ? "Favorite" : "Unfavorite", systemImage: adds ? "star" : "star.slash") {
                    Task {
                        await model.setFavorite(candidates, adds)
                        endSelection()
                    }
                }
                .disabled(candidates.isEmpty)
            }
            ToolbarItem(placement: .bottomBar) {
                Button("Delete", systemImage: "trash", role: .destructive) {
                    selectionDeletion = selectedEntries
                }
                .disabled(selectedEntries.isEmpty)
                .deletionDialog(isPresented: selectionDeletionShown, entries: selectionDeletion, perform: delete)
            }
        } else {
            // Without a back button the leading side would stay empty; like Clock (Edit left, + right).
            ToolbarItem(placement: .topBarLeading) {
                Menu("More", systemImage: "ellipsis") {
                    Button("Select", systemImage: "checkmark.circle") { isSelecting = true }
                        .disabled(model.entries.isEmpty)
                    // As Files' menu: the view as rows with the checkmark, the symbol and the name.
                    Section {
                        Picker("View", selection: $layout) {
                            Label("Icons", systemImage: "square.grid.2x2").tag(LibraryLayout.icons)
                            Label("List", systemImage: "list.bullet").tag(LibraryLayout.list)
                        }
                        .pickerStyle(.inline)
                    }
                    Section {
                        ForEach(LibrarySort.allCases, id: \.self) { order in
                            Toggle(isOn: chosen(order)) {
                                Text(Texts.sortName(order))
                                // The active order's direction under its name, as in Files; under Manual how apps
                                // move, or why not now.
                                if order == sort, let direction = arrangement.direction {
                                    Text(Texts.sortDirection(direction))
                                } else if order == sort,
                                          let note = arrangement.manualNote(searching: !trimmedQuery.isEmpty,
                                                                            filter: filter) {
                                    Text(Texts.manualNote(note))
                                }
                            }
                        }
                    }
                    // A submenu at the bottom with the choice under its name, as Files' View Options.
                    Section {
                        Picker(selection: $filter) {
                            ForEach(LibraryFilter.allCases, id: \.self) { Text(Texts.filterName($0)) }
                        } label: {
                            Text("Filter")
                            Text(Texts.filterName(filter))
                        }
                        .pickerStyle(.menu)
                    }
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

    // MARK: - Order, search and selection

    private var arrangement: LibraryArrangement { LibraryArrangement(sort: sort, ascending: ascending) }

    /// A tap on a sort order: the active one flips its direction.
    private func chosen(_ order: LibrarySort) -> Binding<Bool> {
        Binding(get: { sort == order }, set: { _ in
            let chosen = arrangement.choosing(order)
            sort = chosen.sort
            ascending = chosen.ascending
        })
    }

    private var filterSymbol: String {
        switch filter {
        case .all: "square.grid.2x2"
        case .favorites: "star"
        case .ready: "checkmark.circle"
        case .notReady: "exclamationmark.triangle"
        }
    }

    private var trimmedQuery: String { query.trimmingCharacters(in: .whitespaces) }

    /// Only in the manual order, outside a search, a filter and a selection.
    private var movesApps: Bool {
        arrangement.movesApps(searching: !trimmedQuery.isEmpty, filter: filter) && !isSelecting
    }

    /// Picks still being installed are not in the order yet; before the first of them is the end.
    private func move(_ ids: [LibraryItem.ID], before: LibraryItem.ID?) {
        let before = before.flatMap { id in model.entries.contains { $0.id == id } ? id : nil }
        Task { await model.move(ids, before: before) }
    }

    private var visibleItems: [LibraryItem] {
        var items = model.items(arranged: arrangement)
        if filter != .all {
            let shown = Set(filter.filtered(model.entries, favorites: model.favorites).map(\.id))
            items = items.filter { $0.entry.map { shown.contains($0.id) } ?? filter.showsPendingImports }
        }
        let query = trimmedQuery
        guard !query.isEmpty else { return items }
        return items.filter { item in
            item.title.localizedStandardContains(query)
                || item.entry?.report?.packageName.localizedStandardContains(query) == true
        }
    }

    private var title: String {
        guard isSelecting else { return filter == .all ? "Library" : Texts.filterName(filter) }
        return switch selectedEntries.count {
        case 0: "Select Items"
        case 1: "1 Item Selected"
        case let count: "\(count) Items Selected"
        }
    }

    /// The installed apps shown, as visibleItems has them but without building the tiles' items, which a Select tap
    /// did several times over; pending files are still being imported and cannot be selected.
    private var selectableIDs: [LibraryItem.ID] {
        var entries = model.entries
        if filter != .all {
            let shown = Set(filter.filtered(entries, favorites: model.favorites).map(\.id))
            entries = entries.filter { shown.contains($0.id) }
        }
        let query = trimmedQuery
        if !query.isEmpty {
            entries = entries.filter { entry in
                entry.displayName.localizedStandardContains(query)
                    || entry.report?.packageName.localizedStandardContains(query) == true
            }
        }
        return entries.map(\.id)
    }

    private var selectedEntries: [LibraryEntry] {
        model.entries.filter { selection.contains($0.id) }
    }

    /// The selected apps that can be favorites: files that are no valid APK are left out.
    private var favoriteCandidates: [LibraryEntry] {
        selectedEntries.filter(\.canBeFavorite)
    }

    private var allSelected: Bool {
        !selectableIDs.isEmpty && selectableIDs.allSatisfy(selection.contains)
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

    private var selectionDeletionShown: Binding<Bool> {
        Binding(get: { selectionDeletion != nil }, set: { if !$0 { selectionDeletion = nil } })
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

#if DEBUG
/// Select mode's timing (the user's hang of 2026-11-07, after many apps were selected): each tap on a tile logs how
/// long the main thread worked until it waited again, and a watchdog logs a main thread blocked for more than 250 ms,
/// with what it was doing. Only times and phases, no names or counts. SelectModeUITests' runs read them.
@MainActor enum SelectTiming {
    private static let phase = OSAllocatedUnfairLock(initialState: "idle")
    private static var watching = false
    static var spent: [String: Double] = [:]
    static var calls: [String: Int] = [:]

    static func measure<T>(_ name: String, _ work: () -> T) -> T {
        let start = CACurrentMediaTime()
        defer {
            spent[name, default: 0] += CACurrentMediaTime() - start
            calls[name, default: 0] += 1
        }
        return work()
    }

    static func tap() {
        watch()
        let start = CACurrentMediaTime()
        phase.withLock { $0 = "select tap" }
        let observer = CFRunLoopObserverCreateWithHandler(nil, CFRunLoopActivity.beforeWaiting.rawValue, false,
                                                          Int.max) { observer, _ in
            NSLog("MINATO-SELECT tap answered in %.1f ms, items %.1f ms in %d calls, icons %.1f ms in %d decodes",
                  (CACurrentMediaTime() - start) * 1000, spent["items", default: 0] * 1000, calls["items", default: 0],
                  spent["icons", default: 0] * 1000, calls["icons", default: 0])
            spent = [:]
            calls = [:]
            CFRunLoopRemoveObserver(CFRunLoopGetMain(), observer, .commonModes)
            phase.withLock { $0 = "idle" }
        }
        CFRunLoopAddObserver(CFRunLoopGetMain(), observer, .commonModes)
    }

    private static func watch() {
        guard !watching else { return }
        watching = true
        let phase = phase
        Thread.detachNewThread {
            while true {
                let sent = CACurrentMediaTime()
                let answered = DispatchSemaphore(value: 0)
                DispatchQueue.main.async { answered.signal() }
                if answered.wait(timeout: .now() + 0.25) == .timedOut {
                    let doing = phase.withLock { $0 }
                    NSLog("MINATO-SELECT main thread blocked over 250 ms (%@)", doing)
                    answered.wait()
                    NSLog("MINATO-SELECT main thread blocked %.0f ms (%@)", (CACurrentMediaTime() - sent) * 1000, doing)
                }
                Thread.sleep(forTimeInterval: 0.05)
            }
        }
    }
}
#endif
