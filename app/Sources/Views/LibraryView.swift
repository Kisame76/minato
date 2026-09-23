import APKLibrary
import SwiftUI
import UniformTypeIdentifiers

extension UTType {
    static let apk = UTType(importedAs: "com.android.package-archive")
}

/// The library, laid out like a folder in the Files app: icon grid, search, a "…" menu with Select
/// and the signing certificate, and +.
/// Tapping an app starts it; touch and hold offers Get Info and Delete.
struct LibraryView: View {
    @Environment(LibraryModel.self) private var model
    @State private var shownEntry: LibraryEntry?
    @State private var blockedLaunch: LibraryEntry?
    @State private var isImporting = false
    @State private var query = ""
    @State private var isSelecting = false
    @State private var selection: Set<LibraryItem.ID> = []
    @State private var deletion: Deletion?
    @State private var showsSigning = false

    private let columns = [GridItem(.adaptive(minimum: 104, maximum: 160), spacing: 8, alignment: .top)]

    var body: some View {
        NavigationStack {
            content
                .navigationTitle(title)
                .navigationBarTitleDisplayMode(.inline)
                .searchable(text: $query, placement: .navigationBarDrawer(displayMode: .automatic), prompt: "Search")
                .toolbar { toolbar }
                .fileImporter(isPresented: $isImporting, allowedContentTypes: [.apk], allowsMultipleSelection: true) { result in
                    guard case .success(let urls) = result else { return }
                    Task { await model.open(urls) }
                }
                .sheet(item: $shownEntry) { entry in
                    AppDetailView(entry: entry)
                }
                .sheet(isPresented: $showsSigning) {
                    SigningCertificateView()
                }
                .alert(launchAlertTitle, isPresented: launchAlertShown, presenting: blockedLaunch) { entry in
                    Button("Get Info") { shownEntry = entry }
                    Button("OK", role: .cancel) {}
                } message: { entry in
                    Text(Texts.launchProblem(of: entry).message)
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
            .refreshable { await model.refresh() }
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
                    Divider()
                    Button("Signing Certificate", systemImage: "signature") { showsSigning = true }
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
            Text("Tap + to add APK files, or open an APK with APK-Runner from another app.")
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
        guard isSelecting else { return "APK-Runner" }
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

    /// There is no runtime yet (M2, M3), so for now this explains why the app cannot start.
    private func launch(_ entry: LibraryEntry) {
        blockedLaunch = entry
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

    private var launchAlertTitle: String {
        blockedLaunch.map { Texts.launchProblem(of: $0).title } ?? ""
    }

    private var launchAlertShown: Binding<Bool> {
        Binding(get: { blockedLaunch != nil }, set: { if !$0 { blockedLaunch = nil } })
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
