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
                .padding(6)
                .background(isSelected ? Color(.systemGray5) : .clear, in: .rect(cornerRadius: 12))
            Text(item.title)
                .font(.caption)
                .lineLimit(1)
                .multilineTextAlignment(.center)
                .padding(.horizontal, 4)
                .background(isSelected ? Color(.systemGray5) : .clear, in: .rect(cornerRadius: 5))
            if showsReadyStatus || !isReady {
                StatusView(status: item.status)
            }
        }
        .frame(maxWidth: .infinity, alignment: .top)
        .accessibilityElement(children: .combine)
        .accessibilityAddTraits(isSelected ? .isSelected : [])
    }

    private var isReady: Bool {
        if case .ready = item.status { true } else { false }
    }
}

/// One app's row in the library's List view (the navigation design, section 3): its icon, its name, version and status
/// with the reason, which a row has room for, and the installed size on the right.
struct AppRow: View {
    let item: LibraryItem
    var showsSelection = false
    var isSelected = false

    private static let iconSize: CGFloat = 40

    var body: some View {
        HStack(spacing: 12) {
            if showsSelection {
                SelectionMark(isSelected: isSelected)
            }
            AppIcon(url: item.iconURL, cornerRadius: Self.iconSize * 17 / 60)
                .frame(width: Self.iconSize, height: Self.iconSize)
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
            VStack(alignment: .leading, spacing: 2) {
                Text(item.title)
                    .lineLimit(1)
                if let version = item.entry?.report.map({ $0.versionName ?? String($0.versionCode) }) {
                    Text("Version \(version)")
                        .font(.subheadline)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                }
                StatusView(status: item.status, alignment: .leading)
            }
            Spacer(minLength: 8)
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
    }
}

struct StatusView: View {
    let status: LibraryItem.Status
    /// Centred under a tile, leading in a row.
    var alignment: HorizontalAlignment = .center

    var body: some View {
        switch status {
        case .installing(let fraction):
            VStack(alignment: alignment, spacing: 4) {
                ProgressView(value: fraction)
                    .frame(width: 72)
                Text(Texts.statusLine(status))
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }
        case .updating, .optimizing:
            HStack(spacing: 4) {
                ProgressView()
                    .controlSize(.mini)
                Text(Texts.statusLine(status))
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }
        default:
            Text(Texts.statusLine(status))
                .font(.footnote)
                .foregroundStyle(color)
                .lineLimit(2)
                .multilineTextAlignment(alignment == .leading ? .leading : .center)
        }
    }

    /// Grey like the subtitles in Files, colour only where something needs attention.
    private var color: Color {
        switch status {
        case .new, .installing, .updating, .optimizing, .ready: .secondary
        case .needsSplits, .readyWithout: .orange
        case .incompatible, .invalid, .notInstalled: .red
        }
    }
}
