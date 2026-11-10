import APKLibrary
import UIKit

/// Android 12+'s starting window over the session (ADR-0076 decisions 1 and 3): the colour of the app's theme, or
/// Android's default for the mode, with the library's icon in the middle, 160 points across (1 dp is 1 point at the
/// session's density) and masked to a circle, on a circle of the theme's icon background colour where it names one. It
/// takes no touches and lies above the session's windows and surfaces; at every frame of the display it runs
/// `onFrame`, where the session removes it at the first frame of the starting activity's window.
final class StartingWindowView: UIView {
    static let iconSize: CGFloat = 160

    private let onFrame: () -> Void
    private let iconBackground: UIView?
    private let icon: UIImageView?
    private var link: CADisplayLink?

    init(_ colours: StartingWindow, icon image: UIImage?, onFrame: @escaping () -> Void) {
        self.onFrame = onFrame
        iconBackground = colours.iconBackground.map { colour in
            let circle = UIView()
            circle.backgroundColor = Self.colour(colour)
            circle.layer.cornerRadius = Self.iconSize / 2
            return circle
        }
        icon = image.map { image in
            let view = UIImageView(image: image)
            view.contentMode = .scaleAspectFill
            view.clipsToBounds = true
            view.layer.cornerRadius = Self.iconSize / 2
            return view
        }
        super.init(frame: .zero)
        backgroundColor = Self.colour(colours.splash)
        isUserInteractionEnabled = false
        accessibilityElementsHidden = true
        autoresizingMask = [.flexibleWidth, .flexibleHeight]
        // Above the layers SurfaceLayers adds to the session's view later: a dialog's window stays under it, as
        // Android's starting window lies above the app's windows.
        layer.zPosition = 1_000
        for view in [iconBackground, icon].compactMap({ $0 }) { addSubview(view) }
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) { fatalError("not from a nib") }

    override func layoutSubviews() {
        super.layoutSubviews()
        let square = CGRect(x: bounds.midX - Self.iconSize / 2, y: bounds.midY - Self.iconSize / 2,
                            width: Self.iconSize, height: Self.iconSize)
        iconBackground?.frame = square
        icon?.frame = square
    }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        if window != nil, link == nil {
            let link = CADisplayLink(target: self, selector: #selector(frameShown))
            link.add(to: .main, forMode: .common)
            self.link = link
        } else if window == nil {
            // The link holds its target: it goes with the view.
            link?.invalidate()
            link = nil
        }
    }

    @objc private func frameShown() {
        onFrame()
    }

    private static func colour(_ rgb: UInt32) -> UIColor {
        UIColor(red: CGFloat((rgb >> 16) & 0xFF) / 255, green: CGFloat((rgb >> 8) & 0xFF) / 255,
                blue: CGFloat(rgb & 0xFF) / 255, alpha: 1)
    }
}
