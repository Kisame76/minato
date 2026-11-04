import SwiftUI
import UIKit

/// One tile of the grids in Library and on Home, like an app on the iOS Home Screen: its icon, its name in one line
/// under it.
struct AppCard: View {
    let item: LibraryItem
    /// The Home Screen's on this device (HomeScreenGrid).
    var iconSize: CGFloat = 60
    /// Home shows a status only when it is not Ready (the navigation design, section 2).
    var showsReadyStatus = true
    var showsSelection = false
    var isSelected = false
    @Environment(\.dynamicTypeSize) private var typeSize

    var body: some View {
        // The label 8 pt below the icon, as on the Home Screen: the icon's 6 pt for the selection and 2.
        VStack(spacing: 2) {
            AppIcon(url: item.iconURL)
                .frame(width: iconSize, height: iconSize)
                // A small star at the icon's bottom left, like the heart in Photos.
                .overlay(alignment: .bottomLeading) {
                    if item.isFavorite {
                        Image(systemName: "star.fill")
                            .font(.caption.weight(.semibold))
                            .foregroundStyle(.white)
                            .shadow(color: .black.opacity(0.5), radius: 1.5)
                            .padding(5)
                            .accessibilityLabel("Favorite")
                    }
                }
                .overlay(alignment: .bottom) {
                    if showsSelection {
                        SelectionMark(isSelected: isSelected)
                            .padding(.bottom, 6)
                    }
                }
                .overlay { InstallProgress(status: item.status) }
                .padding(6)
                .background(isSelected ? Color(.systemGray5) : .clear, in: .rect(cornerRadius: 12))
            Text(item.title)
                .font(.caption)
                // One line as on the Home Screen; two at the accessibility sizes, which the tile caps at the first, as
                // its column has room for (the List view shows every size). Every tile reserves them, so that the
                // rows are of one height.
                .lineLimit(typeSize.isAccessibilitySize ? 2 : 1, reservesSpace: true)
                .multilineTextAlignment(.center)
                .padding(.horizontal, 4)
                .background(isSelected ? Color(.systemGray5) : .clear, in: .rect(cornerRadius: 5))
            // Hidden but kept where Home leaves Ready out, so that its rows stay of one height too.
            let showsStatus = showsReadyStatus || !item.status.showsAsReady
            StatusView(status: item.status)
                .opacity(showsStatus ? 1 : 0)
                .accessibilityHidden(!showsStatus)
        }
        .frame(maxWidth: .infinity, alignment: .top)
        .dynamicTypeSize(...DynamicTypeSize.accessibility1)
        .accessibilityElement(children: .combine)
        .accessibilityAddTraits(isSelected ? .isSelected : [])
    }

}

/// One app's row in the library's List view (the navigation design, section 3): its icon, its name, version and status
/// with the reason, which a row has room for, and the installed size on the right.
struct AppRow: View {
    let item: LibraryItem
    var showsSelection = false
    var isSelected = false
    @Environment(\.dynamicTypeSize) private var typeSize

    private static let iconSize: CGFloat = 40

    var body: some View {
        // At the accessibility sizes the row stacks, as the Settings app's rows do, and the name wraps.
        let stacks = typeSize.isAccessibilitySize
        let layout = stacks
            ? AnyLayout(VStackLayout(alignment: .leading, spacing: 8)) : AnyLayout(HStackLayout(spacing: 12))
        layout {
            HStack(spacing: 12) {
                if showsSelection {
                    SelectionMark(isSelected: isSelected)
                }
                icon
            }
            VStack(alignment: .leading, spacing: 2) {
                Text(item.title)
                    .lineLimit(stacks ? nil : 1)
                if let version = item.entry?.report.map({ $0.versionName ?? String($0.versionCode) }) {
                    Text("Version \(version)")
                        .font(.subheadline)
                        .foregroundStyle(.secondary)
                        .lineLimit(stacks ? nil : 1)
                }
                StatusView(status: item.status, alignment: .leading)
            }
            if !stacks {
                Spacer(minLength: 8)
            }
            if let entry = item.entry {
                Text(Int64(clamping: entry.installedSize).formatted(.byteCount(style: .file)))
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
                    .monospacedDigit()
            }
        }
        .contentShape(.rect)
        .accessibilityElement(children: .combine)
        .accessibilityAddTraits(isSelected ? .isSelected : [])
    }

    private var icon: some View {
        AppIcon(url: item.iconURL, cornerRadius: Self.iconSize * 17 / 60)
            .frame(width: Self.iconSize, height: Self.iconSize)
            .overlay { InstallProgress(status: item.status) }
            .overlay(alignment: .bottomLeading) {
                if item.isFavorite {
                    Image(systemName: "star.fill")
                        .font(.caption2.weight(.semibold))
                        .foregroundStyle(.white)
                        .shadow(color: .black.opacity(0.5), radius: 1.5)
                        .padding(3)
                        .accessibilityLabel("Favorite")
                }
            }
    }
}

/// An install's progress as a ring on the app's icon, as the Home Screen shows an app that installs; the status line
/// under it says Installing.
struct InstallProgress: View {
    let status: LibraryItem.Status

    var body: some View {
        if case .installing(let fraction) = status {
            ProgressView(value: fraction)
                .progressViewStyle(.circular)
                .padding(6)
                .background(.regularMaterial, in: Circle())
                .accessibilityHidden(true)
        }
    }
}

/// The circle Files shows on every item while selecting.
struct SelectionMark: View {
    let isSelected: Bool

    var body: some View {
        ZStack {
            Circle().fill(isSelected ? Color.accentColor : Color(.systemBackground))
            Circle().strokeBorder(isSelected ? Color.white : Color(.systemGray3), lineWidth: isSelected ? 2 : 1.5)
            if isSelected {
                Image(systemName: "checkmark")
                    .font(.system(size: 11, weight: .bold))
                    .foregroundStyle(.white)
            }
        }
        .frame(width: 24, height: 24)
        .shadow(color: .black.opacity(0.2), radius: 2, y: 1)
    }
}

struct AppIcon: View {
    let url: URL?
    var cornerRadius: CGFloat = 17

    var body: some View {
        Group {
            if let url, let image = UIImage(contentsOfFile: url.path) {
                Image(uiImage: image)
                    .resizable()
                    .scaledToFill()
            } else {
                ZStack {
                    Rectangle().fill(.quaternary)
                    Image(systemName: "app.dashed")
                        .font(.title)
                        .foregroundStyle(.secondary)
                }
            }
        }
        .clipShape(RoundedRectangle(cornerRadius: cornerRadius, style: .continuous))
        // The app's name next to it says which app it is.
        .accessibilityHidden(true)
    }
}

struct StatusView: View {
    let status: LibraryItem.Status
    /// Centred under a tile, leading in a row.
    var alignment: HorizontalAlignment = .center
    /// Get Info says the reason too; a tile and a row show the status alone in one line, which every one of them
    /// reserves, so that the rows are of one height (the user's equal rows). VoiceOver reads the reason everywhere.
    var showsReason = false

    var body: some View {
        HStack(spacing: 4) {
            switch status {
            case .updating, .optimizing:
                ProgressView()
                    .controlSize(.mini)
            default:
                EmptyView()
            }
            if showsReason {
                text(Texts.statusDetail(status))
            } else {
                text(Texts.statusLine(status))
                    .lineLimit(1, reservesSpace: true)
            }
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(Texts.statusDetail(status))
    }

    private func text(_ line: String) -> some View {
        Text(line)
            .font(.footnote)
            .foregroundStyle(color)
            .multilineTextAlignment(alignment == .leading ? .leading : .center)
    }

    /// Grey like the subtitles in Files, colour only where something needs attention.
    private var color: Color {
        switch status {
        case .new, .installing, .updating, .optimizing, .ready, .readyWithout: .secondary
        case .needsSplits: .orange
        case .incompatible, .invalid, .notInstalled: .red
        }
    }
}
