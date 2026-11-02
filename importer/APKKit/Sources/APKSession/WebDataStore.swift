import CryptoKit
import Foundation

/// The web data of an app's WebViews (ADR-0056 decision 7): the guest names the host's WKWebsiteDataStore after its
/// package with Java's UUID.nameUUIDFromBytes (WebViewProviderImpl, CookieManagerImpl), and deleting the app removes
/// that store, as Android drops an app's WebView data with the app.
public enum WebDataStore {
    /// What the guest puts before the package name.
    static let namePrefix = "minato-webview:"

    /// Java's UUID.nameUUIDFromBytes of "minato-webview:<package>": the bytes' MD5 with the version 3 and the IETF
    /// variant, no namespace.
    public static func identifier(forPackage package: String) -> UUID {
        var bytes = Array(Insecure.MD5.hash(data: Data((namePrefix + package).utf8)))
        bytes[6] = bytes[6] & 0x0f | 0x30
        bytes[8] = bytes[8] & 0x3f | 0x80
        return UUID(uuid: (bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], bytes[8],
                           bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]))
    }
}
