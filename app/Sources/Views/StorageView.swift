import APKLibrary
import SwiftUI

/// Settings › Storage (docs/plans/2026-09-24-app-navigation-design.md, section 6): each app's space, largest first,
/// as the iPhone Storage screen lists apps, with the app and its data apart.
struct StorageView: View {
    @Environment(LibraryModel.self) private var model
    @State private var uses: [StorageUse]?

    var body: some View {
        List(uses ?? [], id: \.id) { use in
            let item = model.items.first { $0.id == use.id }
            LabeledContent {
                Text(Self.size(use.total))
            } label: {
                Label {
                    Text(item?.title ?? use.id)
                    Text("App \(Self.size(use.app)), Data \(Self.size(use.data))")
                } icon: {
                    AppIcon(url: item?.iconURL, cornerRadius: 7)
                        .frame(width: 29, height: 29)
                }
            }
        }
        .overlay {
            if uses == nil {
                ProgressView()
            } else if uses?.isEmpty == true {
                ContentUnavailableView("No Apps", systemImage: "internaldrive")
            }
        }
        .navigationTitle("Storage")
        // Read each time the screen shows, as iPhone Storage does; no pull to refresh there either.
        .task { uses = await model.storage() }
    }

    private static func size(_ bytes: UInt64) -> String {
        Int64(clamping: bytes).formatted(.byteCount(style: .file))
    }
}
