import SwiftUI
import UIKit

/// One tile of the library grid, styled like an item in the Files app.
struct AppCard: View {
    let item: LibraryItem
    var iconSize: CGFloat = 76
    /// Home shows a status only when it is not Ready (the navigation design, section 2).
    var showsReadyStatus = true
    var showsSelection = false
    var isSelected = false

    var body: some View {
        VStack(spacing: 4) {
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
                .font(.subheadline)
                .lineLimit(2)
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
        .clipShape(RoundedRectangle(cornerRadius: 17, style: .continuous))
    }
}

struct StatusView: View {
    let status: LibraryItem.Status

    var body: some View {
        switch status {
        case .installing(let fraction):
            VStack(spacing: 4) {
                ProgressView(value: fraction)
                    .frame(width: 72)
                Text(Texts.statusLine(status))
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }
        case .updating:
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
                .multilineTextAlignment(.center)
        }
    }

    /// Grey like the subtitles in Files, colour only where something needs attention.
    private var color: Color {
        switch status {
        case .new, .installing, .updating, .ready: .secondary
        case .needsSplits, .readyWithout: .orange
        case .incompatible, .invalid, .notInstalled: .red
        }
    }
}
