import APKLibrary
import APKSigning
import SwiftUI

/// The library's tabs, Apple's tab bar at the bottom:
/// Home, Library and Settings; Library has the search. The last tab is kept, so the restart after an app comes back
/// to it (ADR-0003).
enum RootTab: String {
    static let storageKey = "selectedTab"

    case home, library, settings
}

struct RootView: View {
    @Environment(SigningCoordinator.self) private var signing
    @AppStorage(RootTab.storageKey) private var tab = RootTab.library

    var body: some View {
        TabView(selection: $tab) {
            Tab("Home", systemImage: "house", value: RootTab.home) {
                HomeView()
            }
            Tab("Library", systemImage: "square.grid.2x2", value: RootTab.library) {
                LibraryView()
            }
            Tab("Settings", systemImage: "gearshape", value: RootTab.settings) {
                SettingsView()
            }
            // Only when a step by the user is needed; otherwise signing stays invisible (ADR-0023).
            .badge(signing.state.needsUser(next: signing.next) ? 1 : 0)
        }
    }
}

/// Home (section 2), like the iOS Home Screen: the 4 apps opened last in one row, then the favorites in their own
/// order, 4 columns of Home Screen sized icons; the tiles act as in Library.
struct HomeView: View {
    @Environment(LibraryModel.self) private var model
    @State private var actions = TileActions()

    /// Apple's icon size on the Home Screen.
    private static let iconSize: CGFloat = 60
    private let columns = Array(repeating: GridItem(.flexible(), spacing: 8, alignment: .top), count: 4)

    var body: some View {
        NavigationStack {
            Group {
                if model.items.isEmpty {
                    ContentUnavailableView {
                        Label("No Apps", systemImage: "house")
                    } description: {
                        Text("Tap + in Library to add APK files, or open an APK with Minato from another app.")
                    }
                } else if recentlyOpened.isEmpty && favorites.isEmpty {
                    ContentUnavailableView {
                        Label("Nothing Here Yet", systemImage: "house")
                    } description: {
                        Text("Apps you open and your favorites appear here. To add a favorite, touch and hold an app "
                            + "in Library.")
                    }
                } else {
                    ScrollView {
                        VStack(alignment: .leading, spacing: 24) {
                            if !recentlyOpened.isEmpty { section("Recently Opened", recentlyOpened) }
                            if !favorites.isEmpty { section("Favorites", favorites) }
                        }
                        .padding(.horizontal)
                        .padding(.top, 8)
                    }
                }
            }
            .navigationTitle("Home")
            .tileActions(actions)
        }
    }

    private func section(_ title: String, _ items: [LibraryItem]) -> some View {
        VStack(alignment: .leading, spacing: 12) {
            Text(title)
                .font(.title3.weight(.semibold))
            LazyVGrid(columns: columns, spacing: 20) {
                ForEach(items) { item in
                    AppTile(item: item, actions: actions, key: title, iconSize: Self.iconSize,
                            showsReadyStatus: false)
                }
            }
        }
    }

    private var recentlyOpened: [LibraryItem] {
        items(LibrarySort.recentlyOpened(model.entries, lastOpened: model.lastOpened))
    }

    private var favorites: [LibraryItem] {
        items(LibrarySort.favorites(model.entries, favorites: model.favorites))
    }

    /// The library's tiles for these entries, in their order.
    private func items(_ entries: [LibraryEntry]) -> [LibraryItem] {
        let byID = Dictionary(model.items.map { ($0.id, $0) }, uniquingKeysWith: { first, _ in first })
        return entries.compactMap { byID[$0.id] }
    }
}
