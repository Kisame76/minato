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
    /// The tile whose Clear App Data asks, by its key.
    var clearingTile: String?
    var clearing: LibraryEntry?
    /// The report Report a Problem shows (ADR-0070).
    var reporting: AppReport?
}

/// One app's tile, the same in Library and on Home: a tap starts the app or says why not, touch and hold offers Get
/// Info, the favorite and Delete; while selecting, a tap selects. As a row of the List view, a swipe right makes it a
/// favorite and a swipe left deletes it (the navigation design, section 3).
struct AppTile: View {
    let item: LibraryItem
    let actions: TileActions
    var asRow = false
    /// Tells the app's tiles apart where it has more than one.
    var key = ""
    var iconSize: CGFloat = 60
    var showsReadyStatus = true
    var isSelecting = false
    var isSelected = false
    /// A tap while selecting.
    var toggle: () -> Void = {}
    @Environment(LibraryModel.self) private var model
    @Environment(SigningCoordinator.self) private var signing
    @AppStorage(RootTab.storageKey) private var tab = RootTab.library

    var body: some View {
        rowSwipes(tile)
            .deletionDialog(isPresented: deletionShown, entries: actions.deleting.map { [$0] }) { entries in
                Task { await model.remove(entries) }
            }
            .clearDataDialog(isPresented: clearingShown, entry: actions.clearing) { entry in
                Task { await model.clearData(of: entry) }
            }
    }

    private var tile: some View {
        Group {
            if asRow {
                AppRow(item: item, showsSelection: isSelecting && item.entry != nil, isSelected: isSelected)
            } else {
                AppCard(item: item, iconSize: iconSize, showsReadyStatus: showsReadyStatus,
                        showsSelection: isSelecting && item.entry != nil, isSelected: isSelected)
            }
        }
        .contentShape(.rect)
        .onTapGesture { tap() }
        // VoiceOver's double tap does what a tap does: starts the app, or selects it.
        .accessibilityAddTraits(item.entry == nil ? [] : .isButton)
        .accessibilityAction { tap() }
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
                Button("Report a Problem", systemImage: "exclamationmark.bubble") {
                    actions.reporting = model.report(of: entry)
                }
                Divider()
                if !entry.id.hasPrefix("file:") {
                    Button("Clear App Data", systemImage: "eraser", role: .destructive) {
                        actions.clearing = entry
                        actions.clearingTile = tileKey
                    }
                }
                Button("Delete", systemImage: "trash", role: .destructive) {
                    actions.deleting = entry
                    actions.deletingTile = tileKey
                }
            }
        }
    }

    /// The List view's swipes; a grid tile has none, since a swipe modifier there still lets the tile slide along its
    /// row and snap back.
    @ViewBuilder private func rowSwipes(_ content: some View) -> some View {
        if asRow {
            content
                .swipeActions(edge: .leading) {
                    if !isSelecting, let entry = item.entry, entry.canBeFavorite {
                        Button(item.isFavorite ? "Unfavorite" : "Favorite",
                               systemImage: item.isFavorite ? "star.slash" : "star") {
                            Task { await model.setFavorite([entry], !item.isFavorite) }
                        }
                        .tint(.yellow)
                    }
                }
                .swipeActions(edge: .trailing) {
                    if !isSelecting, let entry = item.entry {
                        // It asks first, so the row stays until the answer.
                        Button("Delete", systemImage: "trash") {
                            actions.deleting = entry
                            actions.deletingTile = tileKey
                        }
                        .tint(.red)
                    }
                }
        } else {
            content
        }
    }

    private var tileKey: String { key + item.id }

    private var deletionShown: Binding<Bool> {
        Binding(get: { actions.deletingTile == tileKey },
                set: { if !$0 { actions.deletingTile = nil; actions.deleting = nil } })
    }

    private var clearingShown: Binding<Bool> {
        Binding(get: { actions.clearingTile == tileKey },
                set: { if !$0 { actions.clearingTile = nil; actions.clearing = nil } })
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

extension View {
    /// Asks before clearing an app's data, as Android's Clear storage confirms: it cannot be restored.
    func clearDataDialog(isPresented: Binding<Bool>, entry: LibraryEntry?,
                         perform clear: @escaping (LibraryEntry) -> Void) -> some View {
        confirmationDialog(entry.map { "Clear the Data of “\($0.displayName)”?" } ?? "", isPresented: isPresented,
                           titleVisibility: .visible, presenting: entry) { entry in
            Button("Clear App Data", role: .destructive) { clear(entry) }
        } message: { _ in
            Text("Its saved data, such as progress and settings, will be deleted, as if it were new. The app and its "
                + "OBB files are kept.")
        }
    }
}

private struct TileActionsModifier: ViewModifier {
    @Bindable var actions: TileActions
    @Environment(LibraryModel.self) private var model

    func body(content: Content) -> some View {
        content
            .sheet(item: $actions.shownEntry) { entry in
                AppDetailView(entry: entry)
            }
            .sheet(item: $actions.reporting) { report in
                ReportSheet(report: report)
            }
            .alert(title, isPresented: blockedShown, presenting: actions.blocked) { blocked in
                Button("Get Info") { actions.shownEntry = blocked.entry }
                Button("Report a Problem") { actions.reporting = model.report(of: blocked.entry) }
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

/// Home's and Library's tiles in 4 columns as the Home Screen lays out its apps on this device (HomeScreenGrid): its
/// icon size and row pitch, the margins as wide as the gaps, over the `width` the grid has. With `move`, tiles are
/// dragged into another place (`reordering`).
struct TileGrid<Item: Identifiable, Tile: View>: View where Item.ID: Sendable {
    let items: [Item]
    let width: CGFloat
    var move: (([Item.ID], Item.ID?) -> Void)?
    @ViewBuilder let tile: (Item, CGFloat) -> Tile

    init(items: [Item], width: CGFloat, move: (([Item.ID], Item.ID?) -> Void)? = nil,
         @ViewBuilder tile: @escaping (Item, CGFloat) -> Tile) {
        self.items = items
        self.width = width
        self.move = move
        self.tile = tile
    }

    var body: some View {
        let grid = HomeScreenGrid(width: width, screenWidth: Self.screenWidth)
        // A column per icon and gap, centred on the icon, so that a label may be wider than its icon, as there.
        let columns = Array(repeating: GridItem(.fixed(grid.iconSize + grid.spacing), spacing: 0, alignment: .top),
                            count: HomeScreenGrid.columns)
        LazyVGrid(columns: columns,
                  spacing: grid.rowSpacing(labelLineHeight: UIFont.preferredFont(forTextStyle: .caption1).lineHeight)) {
            ForEach(items) { tile($0, grid.iconSize) }
                .reorderableItems()
        }
        .reordering(Item.self, move: move)
        .padding(.horizontal, grid.spacing / 2)
    }

    /// The width of the screen in portrait, which says the device (the iPhone Duo's two screens are two widths).
    private static var screenWidth: CGFloat {
        let bounds = (UIApplication.shared.connectedScenes.first as? UIWindowScene)?.screen.bounds ?? .zero
        return min(bounds.width, bounds.height)
    }
}

extension DynamicViewContent {
    /// Items that a `reordering` container around them drags into another place, from iOS 27.
    @ViewBuilder func reorderableItems() -> some View {
        if #available(iOS 27.0, *) { reorderable() } else { self }
    }
}

extension View {
    /// Drags the `reorderableItems` inside into another place, as on the Home Screen: touch and hold, then move the
    /// finger. From iOS 27, whose SwiftUI reorders grids and lists without an edit mode beside a context menu (the
    /// navigation design's risks); `move` gets the dragged items and the item they go before, nil for the end. Without
    /// `move`, or before iOS 27, nothing moves.
    @ViewBuilder func reordering<Item: Identifiable>(_ item: Item.Type,
                                                     move: (([Item.ID], Item.ID?) -> Void)?) -> some View
    where Item.ID: Sendable {
        if #available(iOS 27.0, *) {
            reorderContainer(for: Item.self, isEnabled: move != nil) { difference in
                switch difference.destination.position {
                case .before(let id): move?(difference.sources, id)
                case .end: move?(difference.sources, nil)
                }
            }
        } else {
            self
        }
    }
}
