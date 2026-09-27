import APKLibrary
import APKSession
import SwiftUI
import UIKit

/// Game mode's full-screen window (ADR-0032, decisions 5 and 9): the app's layer over the whole screen,
/// without status bar and home indicator, the system gestures deferred on every edge, and the menu behind
/// a swipe down from the top edge. Nothing lies on the layer while the menu is closed.
struct GameModeView: View {
    @State private var session: GameSession
    @Environment(\.scenePhase) private var scenePhase

    init(entry: LibraryEntry) {
        _session = State(initialValue: GameSession(entry: entry))
    }

    var body: some View {
        // The edges defer the system's gestures, so the first swipe down from the top edge opens the
        // menu, and a second one the Notification Center.
        MetalLayerView(onTopEdgeSwipe: { session.openMenu() }, onLayout: { pixels in
            Task { @MainActor in session.windowLaidOut(pixels) }
        }, onTouches: { phase, changes, scale in
            session.touched(phase, changes, scale: scale)
        })
            .ignoresSafeArea()
            .overlay(alignment: .top) {
                if session.showsHint {
                    Text("Swipe down from the top edge for the menu.")
                        .font(.callout)
                        .padding(.horizontal, 16)
                        .padding(.vertical, 10)
                        .background(.regularMaterial, in: Capsule())
                        .padding(.top, 24)
                        .transition(.opacity)
                }
            }
            .animation(.default, value: session.showsHint)
            .background(.black)
            .statusBarHidden(true)
            .persistentSystemOverlays(.hidden)
            .defersSystemGestures(on: .all)
            .confirmationDialog(session.entry.displayName, isPresented: menuShown, titleVisibility: .visible) {
                // Not the cancel role: iOS leaves that button out of the dialog, and a tap beside it closes
                // the menu anyway.
                Button("Continue") { session.closeMenu() }
                Button("Quit") { session.quit() }
            }
            .alert("Open Minato again to return to the library.", isPresented: $session.restartFailed) {
                Button("OK") { exit(0) }
            }
            .onAppear {
                // The guest starts once the window is laid out in the orientation held here.
                guard let scene = UIApplication.shared.connectedScenes.first as? UIWindowScene else {
                    return session.holds(.atStart)
                }
                session.holds(AppDelegate.hold(session.orientation, in: scene) { [session] in
                    // The orientation the window has, then.
                    session.holds(.atStart)
                })
            }
            .onChange(of: scenePhase, initial: true) { old, new in
                session.sceneChanged(from: old, to: new)
            }
            .onReceive(NotificationCenter.default.publisher(for: UIApplication.didReceiveMemoryWarningNotification)) { _ in
                session.memoryWarning()
            }
    }

    private var menuShown: Binding<Bool> {
        Binding(get: { session.menuIsOpen }, set: { if !$0 { session.closeMenu() } })
    }
}
