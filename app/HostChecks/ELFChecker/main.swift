import APKConvert
import Foundation

// tools/elf-checker-check.sh's checks of app/Sources/Signing/ELFChecker.swift, the import's checker over
// apkconv_refusals (ADR-0055 a2): a list longer than the first buffer comes whole after one more call, and a size the
// converter asks for is not trusted: past the cap, growing again, or a count that is not the lines' throws, after at
// most two calls. Arguments: an ELF file the converter refuses.

var failures = 0

func check(_ condition: Bool, _ what: String) {
    print("\(condition ? "ok  " : "FAIL") \(what)")
    if !condition { failures += 1 }
}

func throwsError(_ body: () throws -> [String]) -> String? {
    do {
        _ = try body()
        return nil
    } catch {
        return error.localizedDescription
    }
}

let refused = URL(fileURLWithPath: CommandLine.arguments[1])

// The converter's own list, read with a buffer that holds it.
var whole = [CChar](repeating: 0, count: 1 << 16)
var error = converterErrorBuffer()
let count = apkconv_refusals(refused.path, &whole, whole.count, nil, &error, error.count)
let expected = converterMessage(in: whole).split(separator: "\n").map(String.init)
check(count > 0 && expected.count == Int(count), "the converter refuses the file, \(count) reasons")
let size = converterMessage(in: whole).utf8.count + 1
check(size > 256, "its reasons take more than 256 bytes (\(size))")

// The real converter, with a first buffer too small and with one that holds the list.
check((try? ELFChecker(initialCapacity: 256).refusals(of: refused)) == expected,
      "a list longer than the first buffer comes whole")
check((try? ELFChecker().refusals(of: refused)) == expected, "a list that fits the first buffer comes whole")
check(throwsError { try ELFChecker().refusals(of: refused.appendingPathExtension("missing")) } != nil,
      "a file that cannot be read throws")

/// A converter that answers with `answers` in turn, one per call, and counts the calls.
final class Fake: @unchecked Sendable {
    struct Answer {
        var count: Int32
        var text: String
        var needed: Int
    }

    let answers: [Answer]
    var calls = 0
    var sizes: [Int] = []

    init(_ answers: [Answer]) {
        self.answers = answers
    }

    var refusals: ELFChecker.Refusals {
        { [self] _, reasons, size, needed, error, errorSize in
            let answer = answers[min(calls, answers.count - 1)]
            calls += 1
            sizes.append(size)
            if answer.count < 0 {
                _ = answer.text.withCString { strlcpy(error, $0, errorSize) }
            } else if let reasons, size > 0 {
                _ = answer.text.withCString { strlcpy(reasons, $0, size) }
            }
            needed?.pointee = answer.needed
            return answer.count
        }
    }
}

let two = "first reason\nsecond reason"
let fits = Fake([.init(count: 2, text: two, needed: two.utf8.count + 1)])
check((try? ELFChecker(initialCapacity: 64, refusals: fits.refusals).refusals(of: refused)) == ["first reason", "second reason"]
      && fits.calls == 1, "a list that fits takes one call")

let grows = Fake([.init(count: 2, text: two, needed: 200), .init(count: 2, text: two, needed: 200)])
check((try? ELFChecker(initialCapacity: 16, refusals: grows.refusals).refusals(of: refused)) == ["first reason", "second reason"]
      && grows.calls == 2 && grows.sizes == [16, 200], "a longer list takes a second call with the size asked for")

// The edges: a size of the first buffer takes one call, one byte more a second, the cap itself is read.
let fifteen = String(repeating: "x", count: 15), sixteen = String(repeating: "y", count: 16)
let exact = Fake([.init(count: 1, text: fifteen, needed: 16)])
check((try? ELFChecker(initialCapacity: 16, refusals: exact.refusals).refusals(of: refused)) == [fifteen]
      && exact.calls == 1, "a list of the first buffer's size takes one call")
let oneMore = Fake([.init(count: 1, text: sixteen, needed: 17)])
check((try? ELFChecker(initialCapacity: 16, refusals: oneMore.refusals).refusals(of: refused)) == [sixteen]
      && oneMore.calls == 2, "a list one byte longer comes whole after a second call")
let cap = String(repeating: "z", count: 63)
let atCap = Fake([.init(count: 1, text: cap, needed: 64)])
check((try? ELFChecker(initialCapacity: 16, maximumCapacity: 64, refusals: atCap.refusals).refusals(of: refused)) == [cap]
      && atCap.calls == 2 && atCap.sizes == [16, 64], "a list of the cap's size is read")

let past = Fake([.init(count: 1, text: "reason", needed: (1 << 20) + 1)])
check(throwsError { try ELFChecker(initialCapacity: 16, refusals: past.refusals).refusals(of: refused) } != nil
      && past.calls == 1, "a size past the cap throws, without a second call")

let huge = Fake([.init(count: 1, text: "reason", needed: -1)])
check(throwsError { try ELFChecker(initialCapacity: 16, refusals: huge.refusals).refusals(of: refused) } != nil
      && huge.calls == 1, "a size past Int's range (size_t's top) throws, without a second call")

let again = Fake([.init(count: 1, text: "reason", needed: 100), .init(count: 1, text: "reason", needed: 101)])
check(throwsError { try ELFChecker(initialCapacity: 16, refusals: again.refusals).refusals(of: refused) } != nil
      && again.calls == 2, "a list that grows again throws after the second call")

let fails = Fake([.init(count: -1, text: "cannot open the file", needed: 0)])
check(throwsError { try ELFChecker(refusals: fails.refusals).refusals(of: refused) } == "cannot open the file",
      "the converter's error throws with its text")

let miscounted = Fake([.init(count: 3, text: two, needed: two.utf8.count + 1)])
check(throwsError { try ELFChecker(refusals: miscounted.refusals).refusals(of: refused) } != nil,
      "a count that is not the lines' throws")

let emptyReason = Fake([.init(count: 2, text: "first reason\n", needed: 14)])
check((try? ELFChecker(refusals: emptyReason.refusals).refusals(of: refused)) == ["first reason", ""],
      "an empty reason is a line of its own")

let takes = Fake([.init(count: 0, text: "", needed: 1)])
check((try? ELFChecker(refusals: takes.refusals).refusals(of: refused)) == [], "a library the converter takes has no reason")

exit(failures == 0 ? 0 : 1)
