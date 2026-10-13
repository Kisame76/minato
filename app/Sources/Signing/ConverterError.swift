import Foundation

/// The size of the buffer APKConvert's C functions write their error into. A refusal names every reason a library
/// cannot run, with counts and addresses per reason (ADR-0020, the Unity test APK step A), or every segment of its layout, which
/// runs past a kilobyte; c_api.cpp cuts the text at the buffer's end. A library with every reason the converter knows
/// stays below about 6 KB (Runtime's estimate), so 16 KiB holds any.
let converterErrorCapacity = 16 * 1024

/// An empty error buffer for APKConvert's C functions.
func converterErrorBuffer() -> [CChar] {
    [CChar](repeating: 0, count: converterErrorCapacity)
}

/// The text a C function of APKConvert left in its error buffer.
func converterMessage(in buffer: [CChar]) -> String {
    String(decoding: buffer.prefix { $0 != 0 }.map { UInt8(bitPattern: $0) }, as: UTF8.self)
}
