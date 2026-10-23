import Testing
import UniformTypeIdentifiers

@testable import APKSession

/// ADR-0057 step a: the pick request's text as the runtime checked it, "<kind>\t<flags>\t<max>\t<MIME types>\t<title>",
/// and what the iPhone's picker is asked to show.
@Suite("PickRequest")
struct PickRequestTests {
    @Test func aDocumentRequestIsRead() throws {
        let request = try #require(PickRequest(text: "1\t1\t0\timage/png\ntext/plain\t"))
        #expect(request.kind == .openDocument)
        #expect(request.several)
        #expect(request.mimeTypes == ["image/png", "text/plain"])
        #expect(request.title == "")
        let one = try #require(PickRequest(text: "2\t0\t0\t\t"))
        #expect(one.kind == .getContent)
        #expect(!one.several)
        #expect(one.mimeTypes.isEmpty)
    }

    @Test func theOtherKindsAreRead() throws {
        #expect(try #require(PickRequest(text: "3\t0\t0\ttext/plain\tnote.txt")).kind == .createDocument)
        #expect(try #require(PickRequest(text: "3\t0\t0\ttext/plain\tnote.txt")).title == "note.txt")
        #expect(try #require(PickRequest(text: "4\t0\t0\t\t")).kind == .openTree)
        let photos = try #require(PickRequest(text: "5\t1\t20\timage/*\nvideo/*\t"))
        #expect(photos.kind == .photos)
        #expect(photos.maximum == 20)
    }

    @Test func aTextOutsideTheContractIsNone() {
        for text in ["", "1", "1\t0\t0\t", "6\t0\t0\t\t", "0\t0\t0\t\t", "1\t2\t0\t\t", "1\tx\t0\t\t",
                     "1\t0\t0\t\t\t", "+1\t0\t0\t\t", "1\t0\t01\t\t", "5\t0\t1001\t\t"] {
            #expect(PickRequest(text: text) == nil, "\(text)")
        }
    }

    @Test func theTypesThePickerShows() throws {
        func types(_ mimes: String) throws -> [UTType] {
            try #require(PickRequest(text: "1\t0\t0\t\(mimes)\t")).contentTypes
        }
        #expect(try types("") == [.data])
        #expect(try types("*/*") == [.data])
        #expect(try types("image/*") == [.image])
        #expect(try types("video/*\naudio/*") == [.movie, .audio])
        #expect(try types("text/*") == [.text])
        #expect(try types("application/pdf") == [.pdf])
        #expect(try types("image/png\nimage/png") == [.png])
        #expect(try types("application/x-nothing-known") == [.data])
        #expect(try types("font/*") == [.data])
    }

    /// A request without EXTRA_ALLOW_MULTIPLE takes one document, one with it up to the manifest's 1000.
    @Test func howManyDocumentsARequestTakes() throws {
        #expect(try #require(PickRequest(text: "1\t0\t0\t\t")).documentsAllowed == 1)
        #expect(try #require(PickRequest(text: "2\t1\t0\t\t")).documentsAllowed == DocumentClones.pickLimit)
    }
}
