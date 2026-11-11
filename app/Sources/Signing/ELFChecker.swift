import APKConvert
import Foundation
import os

private let log = Logger(subsystem: "io.github.kisame76.minato", category: "import")

/// The converter's check mode for the import (ADR-0055 a2): every reason apkconv_refusals names for a library, one per
/// line. A list longer than the first buffer takes one more call with the size the converter asks for; that size is
/// not trusted: past `maximumCapacity`, or larger again at the second call, the check throws. A throw is logged with the
/// library's name, and the import then leaves the library to the converter's refusal at signing (NativeCode's try?):
/// a file that cannot be read or a lack of memory is not the library's fault.
struct ELFChecker: Sendable {
    /// apkconv_refusals' form, so that a check can hand the checker a converter that answers otherwise.
    typealias Refusals = @Sendable (UnsafePointer<CChar>?, UnsafeMutablePointer<CChar>?, Int, UnsafeMutablePointer<Int>?,
                                    UnsafeMutablePointer<CChar>?, Int) -> Int32

    let initialCapacity: Int
    let maximumCapacity: Int
    private let call: Refusals

    /// A refusal names its reasons with counts and addresses, as the converter's error does, so the first buffer is
    /// that one's size (ConverterError.swift); 1 MiB is far past any list the converter writes.
    init(initialCapacity: Int = converterErrorCapacity, maximumCapacity: Int = 1 << 20,
         refusals: @escaping Refusals = { apkconv_refusals($0, $1, $2, $3, $4, $5) }) {
        self.initialCapacity = initialCapacity
        self.maximumCapacity = maximumCapacity
        call = refusals
    }

    func refusals(of elf: URL) throws -> [String] {
        do {
            return try check(elf)
        } catch {
            log.error(
                "\(elf.lastPathComponent) could not be checked: \(error.localizedDescription)")
            throw error
        }
    }

    private func check(_ elf: URL) throws -> [String] {
        let first = try ask(elf, capacity: initialCapacity)
        if first.needed <= initialCapacity {
            return try lines(of: first)
        }
        guard first.needed <= maximumCapacity else {
            throw ELFCheckError.message("the converter's reasons for \(elf.lastPathComponent) take \(first.needed) bytes, "
                                        + "more than the \(maximumCapacity) the app reads")
        }
        let second = try ask(elf, capacity: first.needed)
        guard second.needed <= first.needed else {
            throw ELFCheckError.message("the converter's reasons for \(elf.lastPathComponent) grew from \(first.needed) "
                                        + "to \(second.needed) bytes between two calls")
        }
        return try lines(of: second)
    }

    private struct Answer {
        var count: Int32
        var text: String
        /// The size the reasons take with their end; a size_t past Int's range comes negative and counts as too large.
        var needed: Int
    }

    private func ask(_ elf: URL, capacity: Int) throws -> Answer {
        var reasons = [CChar](repeating: 0, count: capacity)
        var error = converterErrorBuffer()
        var needed = 0
        let count = elf.path.withCString { call($0, &reasons, reasons.count, &needed, &error, error.count) }
        guard count >= 0 else {
            throw ELFCheckError.message(converterMessage(in: error))
        }
        return Answer(count: count, text: converterMessage(in: reasons), needed: needed < 0 ? .max : needed)
    }

    /// The reasons, one per line; an empty line is an empty reason, so that a count that is not the lines' shows.
    private func lines(of answer: Answer) throws -> [String] {
        let lines = answer.count == 0 && answer.text.isEmpty
            ? [] : answer.text.split(separator: "\n", omittingEmptySubsequences: false).map(String.init)
        guard lines.count == Int(answer.count) else {
            throw ELFCheckError.message("the converter counted \(answer.count) reasons but wrote \(lines.count)")
        }
        return lines
    }
}

enum ELFCheckError: LocalizedError {
    case message(String)

    var errorDescription: String? {
        switch self {
        case .message(let message): message
        }
    }
}
