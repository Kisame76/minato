import APKSigning
import SwiftUI

/// The library's tabs (docs/plans/2026-09-24-app-navigation-design.md, section 1), Apple's tab bar at the bottom:
/// Home, Library and Settings, and the search as the bar's own search tab. The last tab is kept, so the restart
/// after an app comes back to it (ADR-0003).
enum RootTab: String {
    static let storageKey = "selectedTab"

    case home, library, settings, search
}

struct RootView: View {
    @Environment(SigningCoordinator.self) private var signing
    @AppStorage(RootTab.storageKey) private var tab = RootTab.library
    @State private var query = ""

    var body: some View {
        TabView(selection: $tab) {
            Tab("Home", systemImage: "house", value: RootTab.home) {
                HomeView()
            }
            Tab("Library", systemImage: "square.grid.2x2", value: RootTab.library) {
                LibraryView(query: .constant(""))
            }
            Tab("Settings", systemImage: "gearshape", value: RootTab.settings) {
                SettingsView()
            }
            // Only when a step by the user is needed; otherwise signing stays invisible (ADR-0023).
            .badge(signing.state.needsUser(next: signing.next) ? 1 : 0)
            Tab(value: RootTab.search, role: .search) {
                LibraryView(searching: true, query: $query)
            }
        }
    }
}

/// Home (section 2): Recently Opened and Favorites come with step N4; until then its empty states.
struct HomeView: View {
    @Environment(LibraryModel.self) private var model

    var body: some View {
        NavigationStack {
            Group {
                if model.items.isEmpty {
                    ContentUnavailableView {
                        Label("No Apps", systemImage: "house")
                    } description: {
                        Text("Tap + in Library to add APK files, or open an APK with Minato from another app.")
                    }
                } else {
                    ContentUnavailableView {
                        Label("Nothing Here Yet", systemImage: "house")
                    } description: {
                        Text("Apps you open and your favorites appear here.")
                    }
                }
            }
            .navigationTitle("Home")
        }
    }
}
