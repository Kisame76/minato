import APKSession
import SwiftUI

/// The welcome sheet, as Apple's apps show theirs: a large title, a row per thing to know with its symbol, and
/// Continue. It shows once over the library (`Welcome`), and again from Settings.
struct WelcomeView: View {
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        VStack(spacing: 0) {
            ScrollView {
                VStack(alignment: .leading, spacing: 32) {
                    Text("Welcome to Minato")
                        .font(.largeTitle.bold())
                        .frame(maxWidth: .infinity)
                        .multilineTextAlignment(.center)
                        .padding(.top, 48)
                        .padding(.bottom, 8)
                    row("Add Apps", "Tap + in Library, or open an APK file with Minato from another app.",
                        systemImage: "plus.app")
                    row("Start an App", "Tap the app in Library or on Home.", systemImage: "hand.tap")
                    // As GameModeView opens its menu (MenuSwipe): iOS gives the app only every other top-edge swipe.
                    row("Back to Minato",
                        "Swipe down with three fingers anywhere, or down from the top edge, sometimes twice. "
                            + "Minato’s menu opens, with Quit.",
                        systemImage: "arrow.uturn.backward.circle")
                }
                .padding(.horizontal, 32)
            }
            Button {
                dismiss()
            } label: {
                Text("Continue")
                    .frame(maxWidth: .infinity)
            }
            .buttonStyle(.borderedProminent)
            .controlSize(.large)
            .padding(.horizontal, 32)
            .padding(.vertical, 24)
        }
    }

    private func row(_ title: String, _ text: String, systemImage: String) -> some View {
        HStack(alignment: .top, spacing: 16) {
            Image(systemName: systemImage)
                .font(.largeTitle)
                .foregroundStyle(.tint)
                .frame(width: 48)
                .accessibilityHidden(true)
            VStack(alignment: .leading, spacing: 2) {
                Text(title)
                    .font(.headline)
                Text(text)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        .accessibilityElement(children: .combine)
    }
}
