import APKConvert
import Foundation

// tools/converter-message-check.sh's check of app/Sources/Signing/ConverterError.swift: a refusal longer than a
// kilobyte reaches the app whole through the buffer the app hands APKConvert, as a Unity test APK's x18
// refusal did not (cut at 1,024 bytes). Arguments: the ELF file the converter refuses, a container path.

var failures = 0

func check(_ condition: Bool, _ what: String) {
    print("\(condition ? "ok  " : "FAIL") \(what)")
    if !condition { failures += 1 }
}

let arguments = CommandLine.arguments
var created = converterErrorBuffer()
guard let signer = apkconv_signer_create(nil, arguments[0], "io.github.kisame76.minato.converter-message-check",
                                         &created, created.count) else {
    print("FAIL the signer: \(converterMessage(in: created))")
    exit(1)
}
defer { apkconv_signer_free(signer) }

var app = converterErrorBuffer()
let refused = apkconv_convert(signer, arguments[1], arguments[2], nil, &app, app.count)
var whole = [CChar](repeating: 0, count: 1 << 16)
_ = apkconv_convert(signer, arguments[1], arguments[2], nil, &whole, whole.count)
let message = converterMessage(in: app)
let expected = converterMessage(in: whole)

check(refused != 0, "the converter refuses the file")
check(expected.utf8.count > 1024, "the refusal is longer than a kilobyte (\(expected.utf8.count) bytes)")
check(message == expected, "the app's buffer holds the whole refusal (\(message.utf8.count) of \(expected.utf8.count) bytes)")
exit(failures == 0 ? 0 : 1)
