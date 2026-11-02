import Foundation
import Testing

@testable import APKSession

/// The web data of an app's WebViews (ADR-0056 decision 7): the host's WKWebsiteDataStore with the id the guest names,
/// which deleting the app removes, as Android drops an app's WebView data with it.
struct WebDataStoreTests {
    @Test func theIdIsJavasNameUUIDOfThePackage() {
        // Java's UUID.nameUUIDFromBytes("minato-webview:org.example.game"): MD5, version 3, the IETF variant.
        #expect(WebDataStore.identifier(forPackage: "org.example.game")
            == UUID(uuidString: "43DCA83D-6A86-3DE6-9226-0AF4B96BC043"))
        #expect(WebDataStore.identifier(forPackage: "io.github.kisame76.minato.test.webview")
            == UUID(uuidString: "D720DD50-37E3-35D2-9167-32FDFAEEC36C"))
    }

    @Test func theGuestNamesItsStoreTheSameWay() throws {
        // WebViewProviderImpl and CookieManagerImpl make the id from the same bytes.
        let webkit = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
            .appendingPathComponent("../../../../framework/java/com/android/internal/minato/webkit").standardized
        for file in ["WebViewProviderImpl.java", "CookieManagerImpl.java"] {
            let source = try String(contentsOf: webkit.appendingPathComponent(file), encoding: .utf8)
            #expect(source.contains("UUID.nameUUIDFromBytes((\"\(WebDataStore.namePrefix)\" + "), "\(file)")
        }
    }
}
