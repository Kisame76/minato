import Foundation

/// Certificates for signature tests. The two signer certificates were generated locally for these
/// tests (self-signed, EC P-256); the Play stamp certificate is Google's public source-stamp certificate.
/// SHA-256 values were computed with `shasum -a 256` on the DER files.
public enum TestCertificates {
    /// CN=APKKit Test Signer, O=APK-Runner Tests
    public static let signer = Data(base64Encoded: """
        MIIBxjCCAW2gAwIBAgIUF1B1+y/W0pxzRKq87eOy2T2CO+EwCgYIKoZIzj0EAwIwODEbMBkGA1UEAwwSQVBLS2l0IFRlc3Qg\
        U2lnbmVyMRkwFwYDVQQKDBBBUEstUnVubmVyIFRlc3RzMCAXDTI2MDkyMjAwNTA0M1oYDzIxMjYwODI5MDA1MDQzWjA4MRsw\
        GQYDVQQDDBJBUEtLaXQgVGVzdCBTaWduZXIxGTAXBgNVBAoMEEFQSy1SdW5uZXIgVGVzdHMwWTATBgcqhkjOPQIBBggqhkjO\
        PQMBBwNCAATz3xVShuxswrDDx6zVTa8IC+5ol5KbtXkV/n6ddc//2gp6a+r+X21llVKMaHdLNSJc49kCeuiuhZd+aeHVMpdk\
        o1MwUTAdBgNVHQ4EFgQUj7up5mhqUaNwk9dUI95+3roUUSIwHwYDVR0jBBgwFoAUj7up5mhqUaNwk9dUI95+3roUUSIwDwYD\
        VR0TAQH/BAUwAwEB/zAKBggqhkjOPQQDAgNHADBEAiAPnpaL9FgjjcxrmtfU3k2cXRiBMxllrnNsmu4FlzLOTAIgKLU+Ww22\
        JZaCUYhTc6rvxYCPjrEytAoQHreUxkztpfs=
        """)!
    public static let signerSHA256 = "adcde656fe9aa215bd1cc915c7bfbb4ad631dc0ee9e8e207abb6740497907fdb"

    /// CN=APKKit Rotated Signer, O=APK-Runner Tests
    public static let rotatedSigner = Data(base64Encoded: """
        MIIBzTCCAXOgAwIBAgIUJlZzbM/WD2fbm2/bamAScqFZKCUwCgYIKoZIzj0EAwIwOzEeMBwGA1UEAwwVQVBLS2l0IFJvdGF0\
        ZWQgU2lnbmVyMRkwFwYDVQQKDBBBUEstUnVubmVyIFRlc3RzMCAXDTI2MDkyMjAwNTA1MFoYDzIxMjYwODI5MDA1MDUwWjA7\
        MR4wHAYDVQQDDBVBUEtLaXQgUm90YXRlZCBTaWduZXIxGTAXBgNVBAoMEEFQSy1SdW5uZXIgVGVzdHMwWTATBgcqhkjOPQIB\
        BggqhkjOPQMBBwNCAARPx8Ax5H40T4CaznEwCJyAv6hF59BEvg65PS2U5CbXh3YxIyPoiNWU3AuAfYnW2KqZmSr0has57Q7F\
        Xyl/I+z8o1MwUTAdBgNVHQ4EFgQUmCp0ESMKH0Ac3/rumwN3f8NVJaUwHwYDVR0jBBgwFoAUmCp0ESMKH0Ac3/rumwN3f8NV\
        JaUwDwYDVR0TAQH/BAUwAwEB/zAKBggqhkjOPQQDAgNIADBFAiEAoZt75awFI0Cxq4uKYDLdTSb6aYBktxcfCEs1bEZFZN0C\
        IGgO2MjCjws96Va2Mk+A+SPPC1Ow4nKfL7SfVsqLHvmK
        """)!
    public static let rotatedSignerSHA256 = "8308aa2f15668e96188118c8c402f92bb0ec8cd19821128f976101b58f3fa612"

    /// PKCS#7 SignedData (DER) carrying `signer`, as in META-INF/CERT.RSA of a v1-signed APK.
    public static let signerPKCS7 = Data(base64Encoded: """
        MIIB9QYJKoZIhvcNAQcCoIIB5jCCAeICAQExADALBgkqhkiG9w0BBwGgggHKMIIBxjCCAW2gAwIBAgIUF1B1+y/W0pxzRKq8\
        7eOy2T2CO+EwCgYIKoZIzj0EAwIwODEbMBkGA1UEAwwSQVBLS2l0IFRlc3QgU2lnbmVyMRkwFwYDVQQKDBBBUEstUnVubmVy\
        IFRlc3RzMCAXDTI2MDkyMjAwNTA0M1oYDzIxMjYwODI5MDA1MDQzWjA4MRswGQYDVQQDDBJBUEtLaXQgVGVzdCBTaWduZXIx\
        GTAXBgNVBAoMEEFQSy1SdW5uZXIgVGVzdHMwWTATBgcqhkjOPQIBBggqhkjOPQMBBwNCAATz3xVShuxswrDDx6zVTa8IC+5o\
        l5KbtXkV/n6ddc//2gp6a+r+X21llVKMaHdLNSJc49kCeuiuhZd+aeHVMpdko1MwUTAdBgNVHQ4EFgQUj7up5mhqUaNwk9dU\
        I95+3roUUSIwHwYDVR0jBBgwFoAUj7up5mhqUaNwk9dUI95+3roUUSIwDwYDVR0TAQH/BAUwAwEB/zAKBggqhkjOPQQDAgNH\
        ADBEAiAPnpaL9FgjjcxrmtfU3k2cXRiBMxllrnNsmu4FlzLOTAIgKLU+Ww22JZaCUYhTc6rvxYCPjrEytAoQHreUxkztpfsx\
        AA==
        """)!

    /// Google Play's source stamp certificate (C=US, O=Google Inc., OU=Android, CN=Android).
    public static let googlePlayStamp = Data(base64Encoded: """
        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
        """)!
}
