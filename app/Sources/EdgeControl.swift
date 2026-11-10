#if DEBUG
import SwiftUI
import UIKit

/// The control of device-regressions entry 3 (the landscape edge's first swipe): with MINATO_EDGE_CONTROL=1 in its
/// launch environment, Minato shows only a bare UIKit view controller in landscape that hides the home indicator and
/// defers every screen edge by its own overrides, outside game mode and its session. MinatoUITests'
/// testSwipeUpFromBottomTwice swipes it as it swipes a session: if the control too loses the first swipe to the
/// system, that is iOS's own behaviour, not Minato's.
enum EdgeControl {
    static var isRequested: Bool { ProcessInfo.processInfo.environment["MINATO_EDGE_CONTROL"] == "1" }

    /// Stands in SwiftUI's window until it appears, then hands the window to the control's view controller.
    struct Placeholder: View {
        var body: some View {
            Color.clear.onAppear {
                for case let scene as UIWindowScene in UIApplication.shared.connectedScenes {
                    for window in scene.windows where window.isKeyWindow || scene.windows.count == 1 {
                        window.rootViewController = Root()
                    }
                }
            }
        }
    }

    /// The control: its view counts the touches that begin in it, in its accessibility label ("edge control touches
    /// <n>"), which the runner reads.
    final class Root: UIViewController {
        private var touches = 0

        override func viewDidLoad() {
            super.viewDidLoad()
            view.backgroundColor = .systemGreen
            view.isAccessibilityElement = true
            view.accessibilityIdentifier = "edge-control"
            view.accessibilityLabel = "edge control touches 0"
        }

        override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
            self.touches += 1
            view.accessibilityLabel = "edge control touches \(self.touches)"
            super.touchesBegan(touches, with: event)
        }

        override var prefersStatusBarHidden: Bool { true }
        override var prefersHomeIndicatorAutoHidden: Bool { true }
        override var preferredScreenEdgesDeferringSystemGestures: UIRectEdge { .all }
        override var supportedInterfaceOrientations: UIInterfaceOrientationMask { .landscape }
    }
}
#endif
