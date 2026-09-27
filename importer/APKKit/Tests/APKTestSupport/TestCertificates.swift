import Foundation

/// Certificates and keys for signature tests. The signer certificates and their keys were generated
/// locally for these tests with LibreSSL (self-signed, valid 100 years); they sign nothing but test
/// APKs. The Play stamp certificate is Google's public source-stamp certificate. SHA-256 values were
/// computed with `shasum -a 256` on the DER files.
public enum TestCertificates {
    /// CN=APKKit Test Signer, O=Minato Tests: EC P-256.
    public static let signer = Data(base64Encoded: """
        MIIBvzCCAWWgAwIBAgIUSiEkGJDKEw8qAd6gr3BhcZIyVUswCgYIKoZIzj0EAwIwNDEbMBkGA1UEAwwSQVBLS2l0IFRlc3Qg\
        U2lnbmVyMRUwEwYDVQQKDAxNaW5hdG8gVGVzdHMwIBcNMjYwOTI2MTkxODQ3WhgPMjEyNjA5MDIxOTE4NDdaMDQxGzAZBgNV\
        BAMMEkFQS0tpdCBUZXN0IFNpZ25lcjEVMBMGA1UECgwMTWluYXRvIFRlc3RzMFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAE\
        TbAQfXyqPWF9LPIAtj/nFMz3LQyjIiwsOMAvOODHfq4uZi3QOPB6YEBIaPD3yLUYwwY+V0oENntkndaQ9cncQqNTMFEwHQYD\
        VR0OBBYEFJI67k4xm2TbWQlHLD3U0bA/tzr3MB8GA1UdIwQYMBaAFJI67k4xm2TbWQlHLD3U0bA/tzr3MA8GA1UdEwEB/wQF\
        MAMBAf8wCgYIKoZIzj0EAwIDSAAwRQIgCPSNelMqeGq4ux3j3MOkEEr56LAMJ4ru9+k7/mV99JoCIQCIM2nRtBlGPYZ34orl\
        3jnGQWr8x/Z9IHRkBKcCsICt7A==
        """)!
    public static let signerSHA256 = "303043e72136743e063d9010a1ce3c0b897995e1f8db61a36f1f9a8ec1867d83"
    /// The private key of `signer`, PKCS#8 (DER).
    public static let signerKey = Data(base64Encoded: """
        MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgClB+79hCCYTT+dIRPqovdXUY0vy1hqoHXefTfEmZeomhRANC\
        AARNsBB9fKo9YX0s8gC2P+cUzPctDKMiLCw4wC844Md+ri5mLdA48HpgQEho8PfItRjDBj5XSgQ2e2Sd1pD1ydxC
        """)!

    /// CN=APKKit Rotated Signer, O=Minato Tests: EC P-256.
    public static let rotatedSigner = Data(base64Encoded: """
        MIIBxTCCAWugAwIBAgIUHGC3Amkp+gDKd3VFXeBbwKlwxiEwCgYIKoZIzj0EAwIwNzEeMBwGA1UEAwwVQVBLS2l0IFJvdGF0\
        ZWQgU2lnbmVyMRUwEwYDVQQKDAxNaW5hdG8gVGVzdHMwIBcNMjYwOTI2MTkxODQ3WhgPMjEyNjA5MDIxOTE4NDdaMDcxHjAc\
        BgNVBAMMFUFQS0tpdCBSb3RhdGVkIFNpZ25lcjEVMBMGA1UECgwMTWluYXRvIFRlc3RzMFkwEwYHKoZIzj0CAQYIKoZIzj0D\
        AQcDQgAEi3EGbSnBotoGY09U60+6Ib4P9qdCNDjTvLIqwGZcQTNdRSGMgSFqbqMoY8ismBY2xphBSBXuzvmpPGQQNKl9dKNT\
        MFEwHQYDVR0OBBYEFELOBZ5iVt1adUO0LDsdTJbEGaPxMB8GA1UdIwQYMBaAFELOBZ5iVt1adUO0LDsdTJbEGaPxMA8GA1Ud\
        EwEB/wQFMAMBAf8wCgYIKoZIzj0EAwIDSAAwRQIhALeSl2vH1awcvdP2ke4vFg6dRcTy1Aqnu8s6sVmnomFQAiAoLjzoXLV7\
        a5gnOZnYgD47dQBGFyTKyCKf6L43JcirdQ==
        """)!
    public static let rotatedSignerSHA256 = "3af6e95ab3f7f08ee27058a1821abece3ff4dec076d8f26a7dfa18250d33ca47"
    /// The private key of `rotatedSigner`, PKCS#8 (DER).
    public static let rotatedSignerKey = Data(base64Encoded: """
        MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgqyJJg5hvZef05j3Aa8JGcTsQQf1LlgN4WlfvYNqWJ9KhRANC\
        AASLcQZtKcGi2gZjT1TrT7ohvg/2p0I0ONO8sirAZlxBM11FIYyBIWpuoyhjyKyYFjbGmEFIFe7O+ak8ZBA0qX10
        """)!

    /// CN=APKKit RSA Signer, O=Minato Tests: RSA 2048.
    public static let rsaSigner = Data(base64Encoded: """
        MIIDSTCCAjGgAwIBAgIUNuKS5ZKen3qk6SwEUlkA9kn46ncwDQYJKoZIhvcNAQELBQAwMzEaMBgGA1UEAwwRQVBLS2l0IFJT\
        QSBTaWduZXIxFTATBgNVBAoMDE1pbmF0byBUZXN0czAgFw0yNjA5MjYxOTE4NDdaGA8yMTI2MDkwMjE5MTg0N1owMzEaMBgG\
        A1UEAwwRQVBLS2l0IFJTQSBTaWduZXIxFTATBgNVBAoMDE1pbmF0byBUZXN0czCCASIwDQYJKoZIhvcNAQEBBQADggEPADCC\
        AQoCggEBAOHqT3heSsGChDqAfYlWkQPEpCqZ6MDot81rFzD5XZXFs54DHo/bF5SbNToIyDuMwDd0zeaSEMK+fRcRqfTScJQj\
        FY81uGDfb5nKf38QNPhtTvhA2/f42jdbsMJ5cTs6iqtgApySmauQmL+jYG8SDfkhvih3vJ3PTjTIe5H1oClxj0jw7Kp11vF4\
        g+lLImiHOqLx8juTVhEldTpPimqV3eEc7uXptjENCUPWZ0uzwXXmASBjOCWTcS+/lW5SUnjNqnfqUBnyUWQPv0wlK5Vpb+GN\
        ks7k/9mhmOM91+F+8UPsvph+di2m0BfPRVreUrJe3PUy3RHa8aB3hAlP/BMgtj8CAwEAAaNTMFEwHQYDVR0OBBYEFGdhTeaO\
        6gw4yV3mE4noQB4N4nvwMB8GA1UdIwQYMBaAFGdhTeaO6gw4yV3mE4noQB4N4nvwMA8GA1UdEwEB/wQFMAMBAf8wDQYJKoZI\
        hvcNAQELBQADggEBANLjjOJK77RaV/qriPtrXyvqhuDMJr6DY/DOK08OmwUM1crAGeacsKXCdRuIBliy5hifhlAMg66SJs+l\
        ciugQ5nfcI70es+TpaBzebif93NTZAXvMPv6+toW4bkMW18t06YBeja82v7Lm9dPGc02fwrHt3KtvFqhwlDwrWDKeTk5sqOk\
        52TO7l63GKK7wP8AzKwikAOu+2zROiif0iw18Zn2d6dyUxWzl4M4HrO6krjQ1vLpqKnbKyG86QGKP9G1Gooz6kiqErAWSKrd\
        E1/fjl2AsdhzHKhdJ1J2/RRJQQuV/y8A+Opqka4CMbdiYE/x1C6akP/gy9zRW5FyAwxyvKw=
        """)!
    public static let rsaSignerSHA256 = "5d3a1d3646397105155eb8cad0a6074fb2a1e7003b2a9b3922ab7ea2e6789faa"
    /// The private key of `rsaSigner`, PKCS#1 (DER).
    public static let rsaSignerKey = Data(base64Encoded: """
        MIIEowIBAAKCAQEA4epPeF5KwYKEOoB9iVaRA8SkKpnowOi3zWsXMPldlcWzngMej9sXlJs1OgjIO4zAN3TN5pIQwr59FxGp\
        9NJwlCMVjzW4YN9vmcp/fxA0+G1O+EDb9/jaN1uwwnlxOzqKq2ACnJKZq5CYv6NgbxIN+SG+KHe8nc9ONMh7kfWgKXGPSPDs\
        qnXW8XiD6UsiaIc6ovHyO5NWESV1Ok+KapXd4Rzu5em2MQ0JQ9ZnS7PBdeYBIGM4JZNxL7+VblJSeM2qd+pQGfJRZA+/TCUr\
        lWlv4Y2SzuT/2aGY4z3X4X7xQ+y+mH52LabQF89FWt5Ssl7c9TLdEdrxoHeECU/8EyC2PwIDAQABAoIBADf2bZJBNV6DQNMn\
        IjHjvk+DJjGhZ61l2KYrqZaJpzujOk/hsO/s04YN/pzuMP/DaZgzOJAtzbYYLu+IgiPx/05K9JvBsHda0g/0QCoPLzf9B+tq\
        UbM3M4M0FPDP2ZNU+KXkRjFYBXYg/x210H0jKU/2Il9u7cvf3x0nLqklAfRRxImIGKOXGQLmmQRntkYP3r7iVZK/mF16osS6\
        IbVd4g1WuNvCYF2chhFkr6KCH4YmXWkGVIOCncJCKiNrkvvw3tkNXVm7FEbqLj8ADWqRkmXzSZjXAd/QcVlAuEJQdG8VPIMw\
        +mHV6vhbYeMayFaQa0Yh2x1jNur7UdJLnzIoR2kCgYEA8WhsV49nAOom9at9THYY+RRS/UthF8wGYy1wwuFF/qr282B8SXr5\
        yINB/aDd283k8p1f8gIP52OX7oVGrACaulzvtHRdAmTySi8SWqOHnrobfJuetOhj6bkZd/4FfOSwBlkLNaqo5EemX2F8CS7a\
        ubqRI10oJmNmqnCYQMuaGVkCgYEA75Im/czfQHf2P0dXMhiW0RLZg+XPTrU47XXyoiwJn3BKtIOiS7cL9wKnvra4oSjonSy0\
        tHPWpt6Q91SP8+cEYjPPw/Vd9ax8nL7ttJcp88pDrZpu2NFd9eidB69MuOMiI8brvawawfNFgdN/Gw+eYS0hq5a/g7s+pVw8\
        rd8WwVcCgYAJHeNxjxXZv/4GqxpJR/B7f5v+utkI2AipuC6Y/X7yNiyrVkp7T+yc+PAGDwwx3920l+LmtdM57tXvCRN5LXCq\
        dxYKX5afn4ZS7OIpkCFwCiE9nHbq6BfV3MqscGhupghLxRMaTL6iGF6dHYZcpmtWNkiC6GK6yGg5KZVmZQoFyQKBgEzIriCY\
        YlGaYV1ksyjGQshd30NsC6axZL6PGCYsMSVd60OyEPltgLM5saPpkoFsyClgI8bWfczb9uCexMiqmXgUHRo0cVoFX6cpdqxz\
        6qlNgaWJos9xQ0lGtdW5tMBpZA7Nec8/8w+IthVnQQzeU6fXvDvvxBAnmmuLhZNAtl1fAoGBAIu4HdNo/h0e7fqJRI8iroWI\
        JabDRXRcTkTefD8dH23VcN2EQ4f9msN2FjNfcpsk2T4Cn3vw4rW19paM12Nl+Jyp+jnGxN+L7df72OhyNq4hrfSdDxl2o1dg\
        0m+wNFOK2ygIwXgVHIrNtwAEWJHlnaNItbDtPgSVSMVv/iFiyj8H
        """)!

    /// CN=APKKit DSA Signer, O=Minato Tests: DSA 2048, which Security.framework cannot verify.
    public static let dsaSigner = Data(base64Encoded: """
        MIIEojCCBFCgAwIBAgIUI3L1FX3KCSdsq29C0HI5WSvuRoMwCwYJYIZIAWUDBAMCMDMxGjAYBgNVBAMMEUFQS0tpdCBEU0Eg\
        U2lnbmVyMRUwEwYDVQQKDAxNaW5hdG8gVGVzdHMwIBcNMjYwOTI2MTkxODQ3WhgPMjEyNjA5MDIxOTE4NDdaMDMxGjAYBgNV\
        BAMMEUFQS0tpdCBEU0EgU2lnbmVyMRUwEwYDVQQKDAxNaW5hdG8gVGVzdHMwggNDMIICNgYHKoZIzjgEATCCAikCggEBAM6T\
        mFIdB3rrAhr631l05ePaW1dZytNYmq51oVk79Ch9579lEN0YmeEe+oQbIQuS4ry4ItacCGSWvrF3GiH9Q3ODfSUUeZl1h8Fz\
        j/tat/rMCzzRPoqs5TBHzWPw2SPCgEcTu0pVQwT97XWHiIdOxZh+++StuDHHI+Yu0SR6CpvU8Wqwuv3y84bb3QXp4jT/WHYa\
        6z2NkBN63YiS8YFNrSFCCX1Olfa1YluDMn5qRXMzehxkRhoiL4En/iEP3vP7fTQYdXhjtm1O5bMtbdZRCxtX2/kKrNiX62M+\
        J1GqqwKJTWEnWodsSmcHwB2IZHEjqQcImU2CVsFORlc4xvkUIacCHQCEbEpoptssmfgc4C1I5hzjIbFCVqDXDugbT5uHAoIB\
        AQC3jp/5VkRxkkk+cykaP5bj9DFmmR2uqBwE2F5FbsiuFMGxt6aRt5HmkMwS/y5+e2mXBOQgunzmSuf/s2JyOql42653t3ed\
        86/EaZItHmriMPv0NHrFT16iZKhmB57i144aIESxvjvyEfNssi4UQnIUZT5p8yJ3wdSGTafdHzx7Chvv6PWK27iBHUWbcMIf\
        Ci/4sfXcRSvkQqSe5zzKLKhoM80Y6KcBNxmTmQD3uQ4UJyOD0Ff/7Beikx79CvvHRA07jLKLlrlaTqSpYmcrFpSRpo8Q0m2E\
        m/gvYhgeSPBx5PM9vyELwdTv/ARnHXvoI7O6ZJ4PEe4okFJ/T6TpI5toA4IBBQACggEAG8EoeR/igfSvLfHvmDCA/0xxsVDX\
        m+bfnbxxjOSwCoZj3mBATnoQDocyhXntmFSxsQpdKUTPasYLkc1RESXA+g0AW6kOYI4+Ec1ZWrpwKC45F+wT7njPowHBwPs5\
        aRb4AH9ax0hGXQesTzB+524DgtuZXWr9keMMGAnFlXGLn3fxdne9TTZYzEAQwCuGqgAq7xu4QzK2VMI3JRUIzpw9x8ymrrTX\
        9vzVDtO6sns4RNnzwyaH3PatykFtJp2RxGY6wSQg7iElPmuYRqCwX6t4QG2xD9rv6JZ7xAZX9LUGtYphVOsZNQWyOQkr799s\
        EOvhSDFOI0vKtp1NCXFlbADQFKNTMFEwHQYDVR0OBBYEFPOLFycITHCXCG5EY5+lg07t7dB9MB8GA1UdIwQYMBaAFPOLFycI\
        THCXCG5EY5+lg07t7dB9MA8GA1UdEwEB/wQFMAMBAf8wCwYJYIZIAWUDBAMCAz8AMDwCHHGDZ6WE7trDqsv6Cn9FOtXRQB9b\
        14SAhy9tb2ACHAw/79cSe2rg48rfLHnZjIymdoutJbI4BofKTgM=
        """)!
    public static let dsaSignerSHA256 = "8c75724bfbcb47f92d41ce2b40e3f7ad8ae5184155025b777b0adc8931d7125f"

    /// PKCS#7 SignedData (DER) carrying only `signer`, as in META-INF/CERT.RSA of a v1-signed APK.
    public static let signerPKCS7 = Data(base64Encoded: """
        MIIB7gYJKoZIhvcNAQcCoIIB3zCCAdsCAQExADALBgkqhkiG9w0BBwGgggHDMIIBvzCCAWWgAwIBAgIUSiEkGJDKEw8qAd6g\
        r3BhcZIyVUswCgYIKoZIzj0EAwIwNDEbMBkGA1UEAwwSQVBLS2l0IFRlc3QgU2lnbmVyMRUwEwYDVQQKDAxNaW5hdG8gVGVz\
        dHMwIBcNMjYwOTI2MTkxODQ3WhgPMjEyNjA5MDIxOTE4NDdaMDQxGzAZBgNVBAMMEkFQS0tpdCBUZXN0IFNpZ25lcjEVMBMG\
        A1UECgwMTWluYXRvIFRlc3RzMFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAETbAQfXyqPWF9LPIAtj/nFMz3LQyjIiwsOMAv\
        OODHfq4uZi3QOPB6YEBIaPD3yLUYwwY+V0oENntkndaQ9cncQqNTMFEwHQYDVR0OBBYEFJI67k4xm2TbWQlHLD3U0bA/tzr3\
        MB8GA1UdIwQYMBaAFJI67k4xm2TbWQlHLD3U0bA/tzr3MA8GA1UdEwEB/wQFMAMBAf8wCgYIKoZIzj0EAwIDSAAwRQIgCPSN\
        elMqeGq4ux3j3MOkEEr56LAMJ4ru9+k7/mV99JoCIQCIM2nRtBlGPYZ34orl3jnGQWr8x/Z9IHRkBKcCsICt7DEA
        """)!

    /// Google Play's source stamp certificate (C=US, O=Google Inc., OU=Android, CN=Android).
    public static let googlePlayStamp = Data(base64Encoded: """
        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
        """)!
}
