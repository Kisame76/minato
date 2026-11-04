import APKLibrary
import APKSession
import SwiftUI
import UIKit

/// Game mode's full-screen window (ADR-0032, decisions 5 and 9): the app's layer over the whole screen,
/// without status bar and home indicator, the system gestures deferred on every edge, and the menu behind
/// a swipe down from the top edge. Nothing lies on the layer while the menu is closed, unless the FPS label
/// is on.
struct GameModeView: View {
    @State private var session: GameSession
    @Environment(\.scenePhase) private var scenePhase

    init(entry: LibraryEntry) {
        _session = State(initialValue: GameSession(entry: entry))
    }

    /// A session made beforehand, as a benchmark run's (ADR-0037, decision 3).
    init(session: GameSession) {
        _session = State(initialValue: session)
    }

    var body: some View {
        // The menu opens with a swipe down with three fingers anywhere, which iOS leaves to the app. The swipe
        // down from the top edge stays as a second way, but iOS gives only every other one to the app, since the
        // edges defer the system's gestures (ADR-0032, decision 9, amended 2026-11-01).
        MetalLayerView(onLayer: { session.attach($0, surfaces: $1) }, onTopEdgeSwipe: { session.openMenu() },
                       onLayout: { pixels in
            Task { @MainActor in session.windowLaidOut(pixels) }
        }, onTouches: { phase, changes, scale in
            session.touched(phase, changes, scale: scale)
        }, onKeys: { phase, presses in
            session.keyed(phase, presses)
        }, onTextView: { session.attach($0) }, onKeyboardCover: { session.keyboardCovers($0) },
                       onView: { session.attach($0) })
            .ignoresSafeArea()
            // VoiceOver keeps the three-finger swipe for scrolling: its escape gesture, a two-finger scrub, opens the
            // menu, with Quit.
            .accessibilityElement()
            .accessibilityLabel(session.entry.displayName)
            .accessibilityHint("Scrub with two fingers for Minato’s menu.")
            .accessibilityAction(.escape) { session.openMenu() }
            .overlay(alignment: .top) {
                if session.showsHint {
                    Text("Swipe down with three fingers for the menu.")
                        .font(.callout)
                        .padding(.horizontal, 16)
                        .padding(.vertical, 10)
                        .background(.regularMaterial, in: Capsule())
                        .padding(.top, 24)
                        // The app's touches and the menu's swipe go through it, before the app's first frame too.
                        .allowsHitTesting(false)
                        .transition(.opacity)
                }
            }
            .animation(.default, value: session.showsHint)
            .overlay(alignment: .topLeading) {
                // Only while it is on: any view on the layer makes Core Animation composite it.
                if session.showsFPS, let fps = session.fps {
                    Text("\(fps) FPS")
                        .font(.caption.monospacedDigit().weight(.semibold))
                        .foregroundStyle(.white)
                        .padding(.horizontal, 6)
                        .padding(.vertical, 3)
                        .background(.black.opacity(0.6), in: RoundedRectangle(cornerRadius: 4))
                        .padding(8)
                        .allowsHitTesting(false)
                }
            }
            .background(.black)
            .statusBarHidden(!Self.showsStatusBar)
            .persistentSystemOverlays(.hidden)
            .defersSystemGestures(on: .all)
            .confirmationDialog(session.entry.displayName, isPresented: menuShown, titleVisibility: .visible) {
                // Not the cancel role: iOS leaves that button out of the dialog, and a tap beside it closes
                // the menu anyway.
                Button("Continue") { session.closeMenu() }
                Button(session.showsFPS ? "Hide FPS" : "Show FPS") {
                    session.showFPS(!session.showsFPS)
                    session.closeMenu()
                }
                Button("Quit") { session.quit() }
            }
            .alert("Open Minato again to return to the library.", isPresented: $session.restartFailed) {
                Button("OK") { _exit(0) }
            }
            .onAppear {
                // Only the session SwiftUI keeps takes over the process, before the guest can start.
                session.open()
                // The guest starts once the window is laid out in the orientation held here.
                guard let scene = UIApplication.shared.connectedScenes.first as? UIWindowScene else {
                    return session.holds(.atStart)
                }
                session.holds(AppDelegate.hold(session.orientation, in: scene) { [session] in
                    // The orientation the window has, then.
                    session.holds(.atStart)
                })
            }
            .onDisappear { session.close() }
            .onChange(of: scenePhase, initial: true) { old, new in
                session.sceneChanged(from: old, to: new)
            }
            .onReceive(NotificationCenter.default.publisher(for: UIApplication.didReceiveMemoryWarningNotification)) { _ in
                session.memoryWarning()
            }
    }

    /// Hidden in every session; the Debug build shows it on session-check's request (ADR-0038, step e).
    private static var showsStatusBar: Bool {
        #if DEBUG
        SessionCheck.showsStatusBar
        #else
        false
        #endif
    }

    private var menuShown: Binding<Bool> {
        Binding(get: { session.menuIsOpen }, set: { if !$0 { session.closeMenu() } })
    }
}
