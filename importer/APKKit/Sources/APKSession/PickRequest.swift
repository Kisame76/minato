import UniformTypeIdentifiers

/// A pick request (ADR-0057 step a, APKRT_REQUEST_PICK) as the runtime checked it: "<kind>\t<flags>\t<max>\t<MIME
/// types>\t<title>", the MIME types separated by newlines (guest_interface.h).
public struct PickRequest: Equatable, Sendable {
    public enum Kind: Int, Sendable {
        /// ACTION_OPEN_DOCUMENT and ACTION_GET_CONTENT: files, cloned into Download (step b).
        case openDocument = 1
        case getContent = 2
        /// ACTION_CREATE_DOCUMENT: a place to save (step d).
        case createDocument = 3
        /// ACTION_OPEN_DOCUMENT_TREE: a folder (step d).
        case openTree = 4
        /// MediaStore.ACTION_PICK_IMAGES: photos and videos (step c).
        case photos = 5
    }

    public let kind: Kind
    /// EXTRA_ALLOW_MULTIPLE (kinds 1, 2 and 5).
    public let several: Bool
    /// The photo picker's maximum, 0 for none.
    public let maximum: Int
    public let mimeTypes: [String]
    public let title: String

    public init?(text: String) {
        let fields = text.split(separator: "\t", omittingEmptySubsequences: false).map(String.init)
        guard fields.count == 5, let number = Int(fields[0]), String(number) == fields[0],
              let kind = Kind(rawValue: number),
              fields[1] == "0" || fields[1] == "1",
              let maximum = Int(fields[2]), String(maximum) == fields[2], (0...1000).contains(maximum)
        else { return nil }
        self.kind = kind
        several = fields[1] == "1"
        self.maximum = maximum
        mimeTypes = fields[3].isEmpty
            ? [] : fields[3].split(separator: "\n", omittingEmptySubsequences: false).map(String.init)
        title = fields[4]
    }

    /// The most documents an answer may name: one without EXTRA_ALLOW_MULTIPLE, else the manifest's limit.
    public var documentsAllowed: Int { several ? DocumentClones.pickLimit : 1 }

    /// What the document picker offers: each MIME type's UTType, "image/*", "video/*", "audio/*" and "text/*" as
    /// their kinds, and every file when there is none or one that iOS does not know, as Android's picker shows all
    /// files for "*/*": `.data`, as a folder or a package (`.item`) would only end as a cancel.
    public var contentTypes: [UTType] {
        var types: [UTType] = []
        for mime in mimeTypes {
            let type: UTType
            switch mime {
            case "image/*": type = .image
            case "video/*": type = .movie
            case "audio/*": type = .audio
            case "text/*": type = .text
            default:
                guard !mime.hasSuffix("/*"), let known = UTType(mimeType: mime), !known.isDynamic else {
                    return [.data]
                }
                type = known
            }
            if !types.contains(type) { types.append(type) }
        }
        return types.isEmpty ? [.data] : types
    }
}
