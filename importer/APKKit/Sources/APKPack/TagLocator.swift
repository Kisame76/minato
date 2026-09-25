import Foundation

/// Where each start tag of an XML text and each of its attributes begin, by line, in document order.
/// XMLParser only reports the line where a tag ends, so a tag over several lines would name the wrong
/// line in make-apk's messages. Comments, CDATA sections, processing instructions, declarations and
/// end tags are skipped; for text that does not parse, XMLParser's own error counts.
struct TagLocator {
    struct Tag {
        var line: Int
        var attributes: [(name: String, line: Int)]
    }

    private(set) var tags: [Tag] = []

    init(_ text: Data) {
        let bytes = [UInt8](text)
        var index = 0
        var line = 1

        func at(_ literal: String) -> Bool {
            bytes[index...].starts(with: literal.utf8)
        }
        /// Moves on by one byte and counts the line it leaves.
        func advance() {
            if bytes[index] == UInt8(ascii: "\n") { line += 1 }
            index += 1
        }
        func skip(past terminator: String) {
            while index < bytes.count && !at(terminator) { advance() }
            index = min(index + terminator.utf8.count, bytes.count)
        }
        func isSpace(_ byte: UInt8) -> Bool {
            byte == 0x20 || byte == 0x09 || byte == 0x0A || byte == 0x0D
        }
        /// Bytes up to a space, `=`, `>` or `/`: an element or attribute name.
        func name() -> String {
            let start = index
            while index < bytes.count, !isSpace(bytes[index]), !"=>/".utf8.contains(bytes[index]) { index += 1 }
            return String(decoding: bytes[start..<index], as: UTF8.self)
        }

        while index < bytes.count {
            guard bytes[index] == UInt8(ascii: "<") else {
                advance()
                continue
            }
            if at("<!--") {
                skip(past: "-->")
            } else if at("<![CDATA[") {
                skip(past: "]]>")
            } else if at("<?") {
                skip(past: "?>")
            } else if at("<!") || at("</") {
                skip(past: ">")
            } else {
                var tag = Tag(line: line, attributes: [])
                index += 1
                _ = name()
                while index < bytes.count {
                    let byte = bytes[index]
                    if byte == UInt8(ascii: ">") {
                        index += 1
                        break
                    }
                    if isSpace(byte) || byte == UInt8(ascii: "/") {
                        advance()
                        continue
                    }
                    let attributeLine = line
                    let attribute = name()
                    while index < bytes.count, isSpace(bytes[index]) || bytes[index] == UInt8(ascii: "=") { advance() }
                    if index < bytes.count, bytes[index] == UInt8(ascii: "\"") || bytes[index] == UInt8(ascii: "'") {
                        let quote = bytes[index]
                        index += 1
                        while index < bytes.count, bytes[index] != quote { advance() }
                        index += 1
                    }
                    if !attribute.isEmpty {
                        tag.attributes.append((attribute, attributeLine))
                    }
                }
                tags.append(tag)
            }
        }
    }
}
