import APKLibrary
import APKSession
import SwiftUI

/// What the tiles of a tab share (the navigation design, section 2): the app whose Get Info is open, the app a tap did
/// not start, and the tile whose Delete asks.
@MainActor
@Observable
final class TileActions {
    /// An app that a tap did not start, and why.
    struct Blocked {
        let entry: LibraryEntry
        let reason: LaunchDecision.Reason
    }

    var shownEntry: LibraryEntry?
    var blocked: Blocked?
    /// The tile whose Delete asks, by its key: an app can have two tiles on Home.
    var deletingTile: String?
    var deleting: LibraryEntry?
}

/// One app's tile, the same in Library and on Home: a tap starts the app or says why not, touch and hold offers Get
/// Info, the favorite and Delete; while selecting, a tap selects.
struct AppTile: View {
    let item: LibraryItem
    let actions: TileActions
    /// Tells the app's tiles apart where it has more than one.
    var key = ""
    var iconSize: CGFloat = 76
    var showsReadyStatus = true
    var isSelecting = false
    var isSelected = false
    /// A tap while selecting.
    var toggle: () -> Void = {}
    @Environment(LibraryModel.self) private var model
    @Environment(SigningCoordinator.self) private var signing
    @AppStorage(RootTab.storageKey) private var tab = RootTab.library

    var body: some View {
        AppCard(item: item, iconSize: iconSize, showsReadyStatus: showsReadyStatus,
                showsSelection: isSelecting && item.entry != nil, isSelected: isSelected)
            .contentShape(.rect)
            .onTapGesture { tap() }
            .contextMenu {
                if !isSelecting, let entry = item.entry {
                    Button("Get Info", systemImage: "info.circle") { actions.shownEntry = entry }
                    if entry.canBeFavorite {
                        if item.isFavorite {
                            Button("Remove from Favorites", systemImage: "star.slash") {
                                Task { await model.setFavorite([entry], false) }
                            }
                        } else {
                            Button("Add to Favorites", systemImage: "star") {
                                Task { await model.setFavorite([entry], true) }
                            }
                        }
                    }
                    Divider()
                    Button("Delete", systemImage: "trash", role: .destructive) {
                        actions.deleting = entry
                        actions.deletingTile = tileKey
                    }
                }
            }
            .deletionDialog(isPresented: deletionShown, entries: actions.deleting.map { [$0] }) { entries in
                Task { await model.remove(entries) }
            }
    }

    private var tileKey: String { key + item.id }

    private var deletionShown: Binding<Bool> {
        Binding(get: { actions.deletingTile == tileKey },
                set: { if !$0 { actions.deletingTile = nil; actions.deleting = nil } })
    }

    private func tap() {
        // Pending files are still being imported: nothing to start or select yet.
        guard let entry = item.entry else { return }
        guard isSelecting else {
            launch(entry)
            return
        }
        toggle()
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
                actions.blocked = TileActions.Blocked(entry: entry, reason: reason)
            }
        }
    }
}

extension View {
    /// The tiles' Get Info sheet and the alert for an app a tap did not start, once per tab.
    func tileActions(_ actions: TileActions) -> some View {
        modifier(TileActionsModifier(actions: actions))
    }

    /// Asks before deleting: app data cannot be restored.
    func deletionDialog(isPresented: Binding<Bool>, entries: [LibraryEntry]?,
                        perform delete: @escaping ([LibraryEntry]) -> Void) -> some View {
        let title = entries.map { $0.count == 1 ? "Delete “\($0[0].displayName)”?" : "Delete \($0.count) Items?" } ?? ""
        return confirmationDialog(title, isPresented: isPresented, titleVisibility: .visible,
                                  presenting: entries) { entries in
            Button("Delete", role: .destructive) { delete(entries) }
        } message: { entries in
            Text(entries.count == 1
                ? "Its installed files and app data will be deleted. The original APK is kept."
                : "Their installed files and app data will be deleted. The original APKs are kept.")
        }
    }
}

private struct TileActionsModifier: ViewModifier {
    @Bindable var actions: TileActions

    func body(content: Content) -> some View {
        content
            .sheet(item: $actions.shownEntry) { entry in
                AppDetailView(entry: entry)
            }
            .alert(title, isPresented: blockedShown, presenting: actions.blocked) { blocked in
                Button("Get Info") { actions.shownEntry = blocked.entry }
                Button("OK", role: .cancel) {}
            } message: { blocked in
                Text(Texts.launchProblem(of: blocked.entry, reason: blocked.reason).message)
            }
    }

    private var title: String {
        actions.blocked.map { Texts.launchProblem(of: $0.entry, reason: $0.reason).title } ?? ""
    }

    private var blockedShown: Binding<Bool> {
        Binding(get: { actions.blocked != nil }, set: { if !$0 { actions.blocked = nil } })
    }
}
