import APKLibrary
import APKSigning
import SwiftUI
import UIKit

/// The library's tabs, Apple's tab bar at the bottom:
/// Home, Library and Settings; Library has the search. The last tab is kept, so the restart after an app comes back
/// to it (ADR-0003).
enum RootTab: String, CaseIterable {
    static let storageKey = "selectedTab"

    case home, library, settings
}

/// The tabs in UIKit's tab bar controller, whose bar SwiftUI's TabView shows too, so that a swipe left or right moves
/// between them as pages side by side that follow the finger, as the user wished (the navigation design, the tab
/// swipe): TabView has no swipe, and a paging scroll view would need a bar of its own instead of the system's.
struct RootView: View {
    @Environment(LibraryModel.self) private var library
    @Environment(SigningCoordinator.self) private var signing
    @AppStorage(RootTab.storageKey) private var tab = RootTab.library

    var body: some View {
        // Only when a step by the user is needed; otherwise signing stays invisible (ADR-0023).
        SwipeTabs(selection: $tab, settingsBadge: signing.state.needsUser(next: signing.next),
                  library: library, signing: signing)
            .ignoresSafeArea()
    }
}

private struct SwipeTabs: UIViewControllerRepresentable {
    @Binding var selection: RootTab
    let settingsBadge: Bool
    let library: LibraryModel
    let signing: SigningCoordinator

    func makeUIViewController(context: Context) -> SwipeTabBarController {
        func page(_ view: some View, _ title: String, _ symbol: String) -> UIViewController {
            let page = UIHostingController(rootView: view.environment(library).environment(signing))
            page.tabBarItem = UITabBarItem(title: title, image: UIImage(systemName: symbol), selectedImage: nil)
            return page
        }
        let controller = SwipeTabBarController()
        controller.viewControllers = [page(HomeView(), "Home", "house"),
                                      page(LibraryView(), "Library", "square.grid.2x2"),
                                      page(SettingsView(), "Settings", "gearshape")]
        controller.selectedIndex = RootTab.allCases.firstIndex(of: selection) ?? 1
        return controller
    }

    func updateUIViewController(_ controller: SwipeTabBarController, context: Context) {
        controller.onSelect = { index in selection = RootTab.allCases[index] }
        controller.show(RootTab.allCases.firstIndex(of: selection) ?? 1)
        controller.viewControllers?.last?.tabBarItem.badgeValue = settingsBadge ? "1" : nil
    }
}

/// UIKit's tab bar controller with a swipe between its tabs: Apple's interactive tab transition
/// (UITabBarControllerDelegate's animation and interaction controllers) drives the pages with the finger, the bar's
/// selection follows at once and goes back if the swipe springs back; the first and the last tab stop it (TabSwipe).
/// It starts only on a tab's root screen, as the pushed screens keep their back swipe, and loses to what moves
/// sideways itself: a text field, the tab bar, the List view's rows with their swipes, a touch and hold that already
/// lifts an app or opens its menu. Vertical scrolling waits until the swipe has failed.
final class SwipeTabBarController: UITabBarController, UITabBarControllerDelegate, UIGestureRecognizerDelegate {
    var onSelect: (Int) -> Void = { _ in }
    private let pan = UIPanGestureRecognizer()
    private var interaction: UIPercentDrivenInteractiveTransition?
    /// How far the pages have followed the finger, 0 to 1. Kept here: the interaction's percentComplete reads 0
    /// throughout a tab bar controller's transition on iOS 26, so a slow swipe past half the way sprang back.
    private var progress: CGFloat = 0
    /// From the swipe's start until its pages have settled, finished or sprung back. Meanwhile no other swipe starts
    /// and a tap on the bar waits: a second transition started while one still slides left no page in the window.
    private var swiping = false
    /// Whether UIKit has started the swipe's pages, and a lift that came before (a flick within one turn of the run
    /// loop): finishing or cancelling the interaction before its start left the transition paused at its start for
    /// good, the page frozen and the bar's taps ignored.
    private var started = false
    private var early: (interaction: UIPercentDrivenInteractiveTransition, finishes: Bool)?
    private var swipe: TabSwipe { TabSwipe(count: viewControllers?.count ?? 0) }

    override func viewDidLoad() {
        super.viewDidLoad()
        delegate = self
        pan.addTarget(self, action: #selector(panned(_:)))
        pan.delegate = self
        view.addGestureRecognizer(pan)
    }

    #if DEBUG
    private var stressed = false

    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        if !stressed {
            stressed = true
            stressIfAsked()
        }
    }
    #endif

    /// The selection SwiftUI holds, when it changed elsewhere (a file opened with Minato goes to Library).
    func show(_ index: Int) {
        guard !swiping, index != selectedIndex else { return }
        selectedIndex = index
    }

    func tabBarController(_ controller: UITabBarController, shouldSelect viewController: UIViewController) -> Bool {
        !sliding
    }

    func tabBarController(_ controller: UITabBarController, didSelect viewController: UIViewController) {
        onSelect(selectedIndex)
    }

    // MARK: - The swipe

    func gestureRecognizerShouldBegin(_ recognizer: UIGestureRecognizer) -> Bool {
        guard recognizer === pan else { return true }
        guard !sliding else { return false }
        let velocity = pan.velocity(in: view)
        guard TabSwipe.isHorizontal(dx: velocity.x, dy: velocity.y),
              swipe.target(from: selectedIndex, translation: velocity.x) != nil,
              let page = selectedViewController, isAtRoot(page) else { return false }
        let point = pan.location(in: view)
        guard !tabBar.frame.contains(point), let hit = view.hitTest(point, with: nil) else { return false }
        return !movesSideways(hit)
    }

    /// Scroll views wait for the swipe to fail, so that a vertical scroll starts as soon as the move is not across.
    func gestureRecognizer(_ recognizer: UIGestureRecognizer,
                           shouldBeRequiredToFailBy other: UIGestureRecognizer) -> Bool {
        recognizer === pan && other.view is UIScrollView && other is UIPanGestureRecognizer
    }

    @objc private func panned(_ recognizer: UIPanGestureRecognizer) {
        let translation = recognizer.translation(in: view).x
        let velocity = recognizer.velocity(in: view).x
        switch recognizer.state {
        case .began: begin(velocity: velocity)
        case .changed: move(translation)
        case .ended: end(translation, velocity: velocity, released: true)
        case .cancelled, .failed: end(translation, velocity: velocity, released: false)
        default: break
        }
    }

    /// The swipe's start: the bar selects the tab it goes to, and the pages follow the finger from here.
    fileprivate func begin(velocity: CGFloat) {
        guard !sliding, let target = swipe.target(from: selectedIndex, translation: velocity) else { return }
        interaction = UIPercentDrivenInteractiveTransition()
        interaction?.completionCurve = .easeOut
        progress = 0
        started = false
        early = nil
        swiping = true
        selectedIndex = target
    }

    fileprivate func move(_ translation: CGFloat) {
        guard let interaction else { return }
        // Only the way the swipe began: back past the start holds the page there.
        let towards = (translation < 0) == movesForward
        progress = towards ? TabSwipe.progress(translation: translation, width: view.bounds.width) : 0
        interaction.update(progress)
    }

    /// The finger's lift (`released`) or the system's cancel: the pages go on or spring back.
    fileprivate func end(_ translation: CGFloat, velocity: CGFloat, released: Bool) {
        guard let interaction else { return }
        let finishes = released && (translation < 0) == movesForward
            && TabSwipe.finishes(progress: progress, velocity: velocity, translation: translation)
        self.interaction = nil
        if started {
            settle(interaction, finishes: finishes)
        } else {
            early = (interaction, finishes)
        }
    }

    private func settle(_ interaction: UIPercentDrivenInteractiveTransition, finishes: Bool) {
        if finishes { interaction.finish() } else { interaction.cancel() }
    }

    /// UIKit has started the pages: a lift that came first settles them now, once the interaction has paused them.
    private func pagesStarted() {
        started = true
        guard let early else { return }
        self.early = nil
        DispatchQueue.main.async { self.settle(early.interaction, finishes: early.finishes) }
    }

    /// A swipe's pages, or the bar's own change of tab, still on their way.
    private var sliding: Bool { swiping || transitionCoordinator != nil }

    /// Whether the swipe in progress goes to a later tab, a swipe to the left.
    private var movesForward = true

    private func isAtRoot(_ page: UIViewController) -> Bool {
        if let navigation = page as? UINavigationController { return navigation.viewControllers.count <= 1 }
        return page.children.allSatisfy(isAtRoot)
    }

    private func movesSideways(_ hit: UIView) -> Bool {
        var view: UIView? = hit
        while let current = view, current !== self.view {
            if current is UITextField || current is UITextView || current is UISearchBar { return true }
            // Library's List view: its rows swipe to favorite and delete.
            let inLibrary = selectedIndex == RootTab.allCases.firstIndex(of: .library)
            if current is UICollectionViewCell, inLibrary { return true }
            let pressed = current.gestureRecognizers?.contains {
                $0 is UILongPressGestureRecognizer && ($0.state == .began || $0.state == .changed)
            } ?? false
            if pressed { return true }
            view = current.superview
        }
        return false
    }

    // MARK: - The pages side by side

    func tabBarController(_ controller: UITabBarController,
                          animationControllerForTransitionFrom from: UIViewController,
                          to: UIViewController) -> (any UIViewControllerAnimatedTransitioning)? {
        guard interaction != nil, let pages = viewControllers, let fromIndex = pages.firstIndex(of: from),
              let toIndex = pages.firstIndex(of: to) else { return nil }
        movesForward = toIndex > fromIndex
        return PageSlide(forward: movesForward, started: { [weak self] in self?.pagesStarted() }) { [weak self] in
            guard let self else { return }
            swiping = false
            onSelect(selectedIndex)
        }
    }

    func tabBarController(_ controller: UITabBarController,
                          interactionControllerFor animationController: any UIViewControllerAnimatedTransitioning)
        -> (any UIViewControllerInteractiveTransitioning)? {
        interaction
    }
}

#if DEBUG
/// For TabSwipeUITests: with MINATO_TAB_SWIPE_STRESS=<case> in its environment, Minato swipes by itself at its start
/// what a user's quick fingers do and XCUITest cannot (it waits for each animation's end): "back", a swipe back while
/// the last one still slides; "tap", a tap on Home while a page slides; "flick-left" and "flick-right", a flick whose
/// lift comes before UIKit has started the pages. The test checks the page once it has settled.
extension SwipeTabBarController {
    func stressIfAsked() {
        guard let stress = ProcessInfo.processInfo.environment["MINATO_TAB_SWIPE_STRESS"] else { return }
        let width = view.bounds.width
        // A swipe of 60 % in 4 steps of 20 ms each, let go with a flick; `at` is its start in seconds.
        func swipe(at start: Double, left: Bool) {
            let sign: CGFloat = left ? -1 : 1
            after(start) { self.begin(velocity: sign * 300) }
            for step in 1...4 {
                after(start + 0.02 * Double(step)) { self.move(sign * width * 0.15 * CGFloat(step)) }
            }
            after(start + 0.1) { self.end(sign * width * 0.6, velocity: sign * 900, released: true) }
        }
        func tap(at start: Double, _ index: Int) {
            after(start) {
                guard let page = self.viewControllers?[index],
                      self.delegate?.tabBarController?(self, shouldSelect: page) ?? true else { return }
                self.selectedIndex = index
                self.tabBarController(self, didSelect: page)
            }
        }
        // A flick: the lift in the same turn of the run loop as the start, before UIKit starts the pages.
        func flick(at start: Double, left: Bool) {
            let sign: CGFloat = left ? -1 : 1
            after(start) {
                self.begin(velocity: sign * 900)
                self.end(sign * width * 0.1, velocity: sign * 900, released: true)
            }
        }
        // Each case from Library, where both ways lead somewhere; a swipe's lift is a flick, so its page slides on.
        after(1.5) {
            self.selectedIndex = 1
            self.onSelect(1)
        }
        switch stress {
        case "back":
            // A swipe back while the last one still slides.
            swipe(at: 2.0, left: true)
            swipe(at: 2.15, left: false)
        case "tap":
            // A tap on Home while a page slides.
            swipe(at: 2.0, left: true)
            tap(at: 2.15, 0)
        case "flick-left", "flick-right":
            // A flick whose lift comes before UIKit starts the pages.
            flick(at: 2.0, left: stress == "flick-left")
        default:
            break
        }
    }

    private func after(_ seconds: Double, _ work: @escaping () -> Void) {
        DispatchQueue.main.asyncAfter(deadline: .now() + seconds, execute: work)
    }
}
#endif

/// The two pages side by side, the new one coming in from the side the finger moves away from.
private final class PageSlide: NSObject, UIViewControllerAnimatedTransitioning {
    let forward: Bool
    let started: () -> Void
    let done: () -> Void

    init(forward: Bool, started: @escaping () -> Void, done: @escaping () -> Void) {
        self.forward = forward
        self.started = started
        self.done = done
    }

    func transitionDuration(using context: (any UIViewControllerContextTransitioning)?) -> TimeInterval { 0.3 }

    func animateTransition(using context: any UIViewControllerContextTransitioning) {
        guard let from = context.view(forKey: .from), let to = context.view(forKey: .to),
              let toController = context.viewController(forKey: .to) else {
            context.completeTransition(false)
            return
        }
        let container = context.containerView
        let width = container.bounds.width
        let final = context.finalFrame(for: toController)
        let start = from.frame
        to.frame = final.offsetBy(dx: forward ? width : -width, dy: 0)
        container.addSubview(to)
        UIView.animate(withDuration: transitionDuration(using: context), delay: 0, options: .curveLinear) {
            from.frame = start.offsetBy(dx: self.forward ? -width : width, dy: 0)
            to.frame = final
        } completion: { _ in
            let cancelled = context.transitionWasCancelled
            if cancelled {
                to.removeFromSuperview()
                from.frame = start
            }
            context.completeTransition(!cancelled)
            self.done()
        }
        started()
    }
}

/// Home (section 2), like the iOS Home Screen: the 4 apps opened last in one row, then the favorites in their own
/// order, 4 columns of Home Screen sized icons; the tiles act as in Library.
struct HomeView: View {
    @Environment(LibraryModel.self) private var model
    @State private var actions = TileActions()
    @State private var width: CGFloat = 0

    var body: some View {
        NavigationStack {
            Group {
                if model.items.isEmpty {
                    ContentUnavailableView {
                        Label("No Apps", systemImage: "house")
                    } description: {
                        Text("Tap + in Library to add APK files, or open an APK with Minato from another app.")
                    }
                } else if recentlyOpened.isEmpty && favorites.isEmpty {
                    ContentUnavailableView {
                        Label("Nothing Here Yet", systemImage: "house")
                    } description: {
                        Text("Apps you open and your favorites appear here. To add a favorite, touch and hold an app "
                            + "in Library.")
                    }
                } else {
                    ScrollView {
                        VStack(alignment: .leading, spacing: 24) {
                            if !recentlyOpened.isEmpty { section("Recently Opened", recentlyOpened) }
                            if !favorites.isEmpty { section("Favorites", favorites) }
                        }
                        .padding(.top, 8)
                    }
                    .onGeometryChange(for: CGFloat.self) { $0.size.width } action: { width = $0 }
                }
            }
            .navigationTitle("Home")
            .tileActions(actions)
        }
    }

    private func section(_ title: String, _ items: [LibraryItem]) -> some View {
        VStack(alignment: .leading, spacing: 12) {
            Text(title)
                .font(.title3.weight(.semibold))
                .padding(.horizontal)
                .accessibilityAddTraits(.isHeader)
            TileGrid(items: items, width: width) { item, iconSize in
                AppTile(item: item, actions: actions, key: title, iconSize: iconSize, showsReadyStatus: false)
            }
        }
    }

    private var recentlyOpened: [LibraryItem] {
        items(LibrarySort.recentlyOpened(model.entries, lastOpened: model.lastOpened))
    }

    private var favorites: [LibraryItem] {
        items(LibrarySort.favorites(model.entries, favorites: model.favorites))
    }

    /// The library's tiles for these entries, in their order.
    private func items(_ entries: [LibraryEntry]) -> [LibraryItem] {
        let byID = Dictionary(model.items.map { ($0.id, $0) }, uniquingKeysWith: { first, _ in first })
        return entries.compactMap { byID[$0.id] }
    }
}
