/// A tap on one of the app's notifications (ADR-0074 decision 4). While the app's session runs, the guest finds the
/// notification by its id and the FNV-1a 64 of its tag, and sends its PendingIntent itself, as SystemUI does. After
/// the process ended, the notification's stored content intent (Intent.toUri, which the guest wrote) starts the app
/// only when it names, explicitly, an activity of the notification's own package, as a PendingIntent carries its
/// creator's identity (the lead's condition): every other one opens the app at its launcher.
public enum NotificationTap {
    /// The longest stored intent, in bytes.
    static let intentLimit = 2048

    /// `uri` when it is an "intent:" URI of Intent.toUri with exactly one component, "<package>/<class>", of `package`,
    /// whose class (".Name" short for the package's) is one of `activities`, with no other package and no selector,
    /// in printable ASCII within intentLimit; else nil, and the app opens at its launcher.
    public static func startIntent(_ uri: String, package: String, activities: Set<String>) -> String? {
        let bytes = Array(uri.utf8)
        guard !bytes.isEmpty, bytes.count <= intentLimit, bytes.allSatisfy({ $0 >= 0x21 && $0 <= 0x7e }),
              uri.hasPrefix("intent:"), uri.hasSuffix(";end") else { return nil }
        let marker = "#Intent;"
        guard let start = uri.range(of: marker), uri.range(of: marker, range: start.upperBound..<uri.endIndex) == nil
        else { return nil }
        let body = uri[start.upperBound..<uri.index(uri.endIndex, offsetBy: -3)]  // up to and with the last ';'
        var fields = body.split(separator: ";", omittingEmptySubsequences: false).map(String.init)
        guard fields.last == "" else { return nil }
        fields.removeLast()
        var component: String?
        for field in fields {
            guard field != "SEL" else { return nil }
            if field.hasPrefix("component=") {
                guard component == nil else { return nil }
                component = String(field.dropFirst("component=".count))
            } else if field.hasPrefix("package="), field != "package=\(package)" {
                return nil
            }
        }
        guard let component, let slash = component.firstIndex(of: "/"),
              component[..<slash] == package else { return nil }
        let name = String(component[component.index(after: slash)...])
        guard !name.isEmpty else { return nil }
        let qualified = name.hasPrefix(".") ? package + name : name
        return activities.contains(qualified) ? uri : nil
    }

    /// FNV-1a 64 of the tag's UTF-8, which the tap's event carries with the id (APKRT_EVENT_NOTIFICATION_TAP).
    public static func tagHash(_ tag: String) -> UInt64 {
        var hash: UInt64 = 0xcbf2_9ce4_8422_2325
        for byte in tag.utf8 {
            hash ^= UInt64(byte)
            hash = hash &* 0x0000_0100_0000_01b3
        }
        return hash
    }
}
