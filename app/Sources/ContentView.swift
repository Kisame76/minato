import SwiftUI
import UIKit

struct ContentView: View {
    private let appVersion = Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "?"
    private let systemVersion = UIDevice.current.systemVersion

    var body: some View {
        NavigationStack {
            ContentUnavailableView {
                Label("Noch keine Spiele", systemImage: "gamecontroller")
            } description: {
                Text("Die Library kommt mit Meilenstein M1.")
            }
            .navigationTitle("APK-Runner")
            .safeAreaInset(edge: .bottom) {
                Text("Version \(appVersion) · iOS \(systemVersion)")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                    .padding(.bottom, 8)
            }
        }
    }
}

#Preview {
    ContentView()
}
