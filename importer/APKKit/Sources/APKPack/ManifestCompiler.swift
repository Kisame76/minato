import Foundation

/// Compiles a text AndroidManifest.xml into Android's binary XML, the way aapt2 does (ADR-0031):
/// element names pass through; `package` and a closed table of `android:` attributes are encoded
/// with aapt2's value types, and each element's attributes are sorted by resource ID, which
/// libandroidfw's attribute lookup relies on. Anything else is refused with file and line.
public enum ManifestCompiler {
    static let androidNamespace = "http://schemas.android.com/apk/res/android"

    enum Encoding: Sendable {
        case string
        case boolean
        case integer
        /// `android:value` of `<meta-data>`: boolean, then integer, otherwise string, as aapt2 infers it.
        case inferred
    }

    struct Definition: Sendable {
        let id: UInt32
        let encoding: Encoding
    }

    /// The `android:` attributes make-apk knows, with their IDs from AOSP's public-final.xml (read
    /// 2026-09-25). A new one comes with a test and a check on the emulator.
    static let attributes: [String: Definition] = [
        "label": Definition(id: 0x0101_0001, encoding: .string),
        "name": Definition(id: 0x0101_0003, encoding: .string),
        "hasCode": Definition(id: 0x0101_000C, encoding: .boolean),
        "exported": Definition(id: 0x0101_0010, encoding: .boolean),
        "value": Definition(id: 0x0101_0024, encoding: .inferred),
        "minSdkVersion": Definition(id: 0x0101_020C, encoding: .integer),
        "versionCode": Definition(id: 0x0101_021B, encoding: .integer),
        "versionName": Definition(id: 0x0101_021C, encoding: .string),
        "targetSdkVersion": Definition(id: 0x0101_0270, encoding: .integer),
        "glEsVersion": Definition(id: 0x0101_0281, encoding: .integer),
        "required": Definition(id: 0x0101_028E, encoding: .boolean),
        "extractNativeLibs": Definition(id: 0x0101_04EA, encoding: .boolean),
        "version": Definition(id: 0x0101_0519, encoding: .integer),
    ]

    /// `text` is the manifest's content, `file` its name for messages.
    public static func compile(_ text: Data, file: String) throws -> Data {
        let parser = XMLParser(data: text)
        let reader = Reader(file: file, tags: TagLocator(text).tags)
        parser.delegate = reader
        let parsed = parser.parse()
        if let error = reader.error { throw error }
        guard parsed, let root = reader.root else {
            let reason = (parser.parserError as NSError?)?.userInfo["NSXMLParserErrorMessage"] as? String
            throw PackError(file: file, line: parser.lineNumber,
                            message: "the XML does not parse: \(reason?.trimmingCharacters(in: .whitespacesAndNewlines) ?? "unknown error")")
        }
        return AXMLBuilder().build(root)
    }

    /// Builds the element tree while XMLParser reads the text. Namespace prefixes are resolved here
    /// through the `xmlns` declarations in scope, because XMLParser leaves attribute prefixes as they are.
    private final class Reader: NSObject, XMLParserDelegate {
        let file: String
        var root: AXMLBuilder.Element?
        var error: PackError?
        private var open: [AXMLBuilder.Element] = []
        private var scopes: [[String: String]] = [[:]]
        /// Where the start tags stand, in document order, for the lines in messages.
        private let tags: [TagLocator.Tag]
        private var nextTag = 0

        init(file: String, tags: [TagLocator.Tag]) {
            self.file = file
            self.tags = tags
        }

        func parser(_ parser: XMLParser, didStartElement elementName: String, namespaceURI: String?,
                    qualifiedName: String?, attributes: [String: String] = [:]) {
            do {
                var scope = scopes.last ?? [:]
                for (name, uri) in attributes where name == "xmlns" || name.hasPrefix("xmlns:") {
                    scope[name == "xmlns" ? "" : String(name.dropFirst("xmlns:".count))] = uri
                }
                scopes.append(scope)
                // XMLParser reports the line where a tag ends; messages name the line where it starts,
                // or where the attribute stands.
                let tag = nextTag < tags.count ? tags[nextTag] : nil
                nextTag += 1
                let line = tag?.line ?? parser.lineNumber
                func attributeLine(_ attribute: String) -> Int {
                    tag?.attributes.first { $0.name == attribute }?.line ?? parser.lineNumber
                }
                // In the order of the text, so that the first problem is the one reported.
                func position(_ attribute: String) -> Int {
                    tag?.attributes.firstIndex { $0.name == attribute } ?? Int.max
                }
                let names = attributes.keys.sorted { (position($0), $0) < (position($1), $1) }

                if elementName.contains(":") || !(scope[""] ?? "").isEmpty {
                    throw fail(line, "<\(elementName)>: elements in a namespace are not supported")
                }
                let isRoot = open.isEmpty
                if isRoot && elementName != "manifest" {
                    throw fail(line, "the root element is <\(elementName)>, not <manifest>")
                }

                var compiled: [AXMLBuilder.Attribute] = []
                for name in names where name != "xmlns" && !name.hasPrefix("xmlns:") {
                    let attribute = try self.attribute(name, attributes[name] ?? "", of: elementName, isRoot: isRoot,
                                                       scope: scope, line: attributeLine(name))
                    // Two prefixes of the same namespace name the same attribute, which aapt2 refuses.
                    if compiled.contains(where: { $0.namespace == attribute.namespace && $0.name == attribute.name }) {
                        throw fail(attributeLine(name),
                                   "\(attribute.namespace == nil ? "" : "android:")\(attribute.name) is given twice on <\(elementName)>")
                    }
                    compiled.append(attribute)
                }
                if isRoot && !compiled.contains(where: { $0.namespace == nil && $0.name == "package" }) {
                    throw fail(line, "<manifest> has no package attribute")
                }
                open.append(AXMLBuilder.Element(elementName, compiled.sorted(by: Self.aapt2Order)))
            } catch let failure as PackError {
                stop(parser, failure)
            } catch {
                stop(parser, fail(parser.lineNumber, "\(error)"))
            }
        }

        func parser(_ parser: XMLParser, didEndElement elementName: String, namespaceURI: String?, qualifiedName: String?) {
            guard error == nil, let element = open.popLast() else { return }
            scopes.removeLast()
            if open.isEmpty {
                root = element
            } else {
                open[open.count - 1].children.append(element)
            }
        }

        func parser(_ parser: XMLParser, foundCharacters string: String) {
            guard error == nil, !string.allSatisfy(\.isWhitespace) else { return }
            stop(parser, fail(parser.lineNumber, "text inside <\(open.last?.name ?? "?")> is not supported"))
        }

        func parser(_ parser: XMLParser, foundCDATA CDATABlock: Data) {
            guard error == nil else { return }
            stop(parser, fail(parser.lineNumber, "text inside <\(open.last?.name ?? "?")> is not supported"))
        }

        private func attribute(_ name: String, _ value: String, of element: String, isRoot: Bool,
                               scope: [String: String], line: Int) throws -> AXMLBuilder.Attribute {
            guard let colon = name.firstIndex(of: ":") else {
                guard isRoot, name == "package" else { throw fail(line, "unknown attribute \(name) on <\(element)>") }
                guard Self.isPackageName(value) else {
                    throw fail(line, "package \"\(value)\" is not an Android package name: two or more segments, each a letter followed by letters, digits or _")
                }
                return .plain("package", .string(value))
            }
            let prefix = String(name[..<colon])
            let local = String(name[name.index(after: colon)...])
            guard let uri = scope[prefix] else { throw fail(line, "\(name): the prefix \(prefix) is not declared") }
            guard uri == ManifestCompiler.androidNamespace else {
                throw fail(line, "\(name): the namespace \(uri) is not supported")
            }
            guard let definition = ManifestCompiler.attributes[local] else {
                throw fail(line, "unknown attribute android:\(local) on <\(element)>")
            }
            if value.hasPrefix("@") || value.hasPrefix("?") {
                throw fail(line, "android:\(local): references such as \(value) are not supported")
            }
            // aapt2 trims some values and resolves backslash escapes in others; make-apk writes values
            // exactly as they are and so refuses what it would write differently.
            if value.first?.isWhitespace == true || value.last?.isWhitespace == true {
                throw fail(line, "android:\(local): spaces at the start or the end are not supported")
            }
            if value.contains("\\") {
                throw fail(line, "android:\(local): backslash escapes are not supported")
            }
            return .android(local, definition.id, try encode(value, as: definition.encoding, name: "android:\(local)", line: line))
        }

        private func encode(_ value: String, as encoding: Encoding, name: String, line: Int) throws -> AXMLBuilder.Value {
            switch encoding {
            case .string:
                return .string(value)
            case .boolean:
                guard let flag = Self.boolean(value) else { throw fail(line, "\(name) must be true or false, not \"\(value)\"") }
                return .bool(flag)
            case .integer:
                guard let number = Self.integer(value) else { throw fail(line, "\(name) must be an integer, not \"\(value)\"") }
                return number
            case .inferred:
                if let flag = Self.boolean(value) { return .bool(flag) }
                if let number = Self.integer(value) { return number }
                // aapt2 would read these as a color, or as a float or dimension, which make-apk does not write.
                if let first = value.first, first == "#" || first.isNumber || ".-+".contains(first) {
                    throw fail(line, "\(name) \"\(value)\" would be a color or a number other than an integer, which make-apk does not write")
                }
                return .string(value)
            }
        }

        /// aapt2's order (`cmp_xml_attribute_by_id`): attributes with a resource ID by ID, then the others.
        private static func aapt2Order(_ a: AXMLBuilder.Attribute, _ b: AXMLBuilder.Attribute) -> Bool {
            switch (a.resourceID, b.resourceID) {
            case let (left?, right?): left < right
            case (.some, .none): true
            case (.none, .some): false
            case (.none, .none): (a.namespace ?? "", a.name) < (b.namespace ?? "", b.name)
            }
        }

        /// aapt2's `IsAndroidPackageName`: two or more segments, each an ASCII letter followed by
        /// ASCII letters, digits or underscores.
        private static func isPackageName(_ text: String) -> Bool {
            let segments = text.split(separator: ".", omittingEmptySubsequences: false)
            return segments.count >= 2 && segments.allSatisfy { segment in
                guard let first = segment.first, first.isASCII, first.isLetter else { return false }
                return segment.allSatisfy { $0.isASCII && ($0.isLetter || $0.isNumber || $0 == "_") }
            }
        }

        /// The spellings aapt2's `ParseBool` accepts.
        private static func boolean(_ text: String) -> Bool? {
            switch text {
            case "true", "TRUE", "True": true
            case "false", "FALSE", "False": false
            default: nil
            }
        }

        /// Decimal fits Int32 and becomes TYPE_INT_DEC; `0x…` fits UInt32 and becomes TYPE_INT_HEX.
        /// Like aapt2 (`stringToInt`), only a lowercase x makes a hex number.
        private static func integer(_ text: String) -> AXMLBuilder.Value? {
            if text.hasPrefix("0x") {
                let digits = text.dropFirst(2)
                guard !digits.isEmpty, digits.allSatisfy(\.isHexDigit), let number = UInt32(digits, radix: 16) else { return nil }
                return .hex(number)
            }
            let digits = text.hasPrefix("-") ? text.dropFirst() : Substring(text)
            guard !digits.isEmpty, digits.allSatisfy({ $0.isASCII && $0.isNumber }), let number = Int32(text) else { return nil }
            return .int(number)
        }

        private func fail(_ line: Int, _ message: String) -> PackError {
            PackError(file: file, line: line, message: message)
        }

        private func stop(_ parser: XMLParser, _ failure: PackError) {
            if error == nil { error = failure }
            parser.abortParsing()
        }
    }
}
