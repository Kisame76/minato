// boringssl-test (ADR-0046 b): BoringSSL's static libraries as libjavacrypto links them, libcrypto_static with the
// ARMv8 crypto extension at compile time and libssl, under known answers. It prints, in this order:
//   - SHA-256 of "abc" (FIPS 180-2, appendix B.1: ba7816bf...f20015ad);
//   - HMAC-SHA256 of RFC 4231's test case 2 (5bdcc146...64ec3843);
//   - AES-128-GCM of test case 4 of McGrew and Viega's "The Galois/Counter Mode of Operation (GCM)" (42831ec2...
//     3d58e091, tag 5bc94fbc...e7121a47), and whether it opens again, also with a changed tag;
//   - X25519 of RFC 7748, section 6.1: Alice's public value (8520f009...aa9b4e6a) and the shared secret
//     (4a5d9d5b...1e161742);
//   - whether AES-GCM runs on the crypto extension (EVP_has_aes_hardware): BoringSSL reads it from AT_HWCAP unless
//     the compiler was told, and ours reports FP and ASIMD only (runtime/src/loader.h);
//   - libssl's default cipher list, its length and its first cipher, which is AES-GCM with the extension and
//     ChaCha20 without it.
// runtime.boringssl runs it through apk-run; fixtures/expected/boringssl-test.out is what it prints on Android 15.
// With --measure it prints how fast AES-128-GCM seals 16 MB instead, as information.
#include <openssl/aead.h>
#include <openssl/curve25519.h>
#include <openssl/digest.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>

#include <stdio.h>
#include <string.h>
#include <time.h>

static void print_hex(const uint8_t *data, size_t length) {
    for (size_t index = 0; index < length; index++) printf("%02x", data[index]);
}

// The bytes of hex text, as the publications give the inputs; returns their number.
static size_t from_hex(const char *text, uint8_t *out) {
    size_t length = 0;
    for (; text[0] != '\0' && text[1] != '\0'; text += 2) {
        unsigned value = 0;
        sscanf(text, "%2x", &value);
        out[length++] = (uint8_t)value;
    }
    return length;
}

// --measure: AES-128-GCM seals 16 MB in place, in pieces of 64 KB.
static int measure(void) {
    static uint8_t buffer[65536 + EVP_AEAD_MAX_OVERHEAD];
    uint8_t key[16] = {0}, nonce[12] = {0};
    EVP_AEAD_CTX aead;
    if (!EVP_AEAD_CTX_init(&aead, EVP_aead_aes_128_gcm(), key, sizeof key, EVP_AEAD_DEFAULT_TAG_LENGTH, NULL)) return 1;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int piece = 0; piece < 256; piece++) {
        size_t length = 0;
        nonce[0] = (uint8_t)piece;
        if (!EVP_AEAD_CTX_seal(&aead, buffer, &length, sizeof buffer, nonce, sizeof nonce, buffer, 65536, NULL, 0)) {
            return 1;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    EVP_AEAD_CTX_cleanup(&aead);
    double seconds = (double)(end.tv_sec - start.tv_sec) + (double)(end.tv_nsec - start.tv_nsec) / 1e9;
    printf("AES-128-GCM seals 16 MB in %.1f ms, %.0f MB/s; on the crypto extension: %s\n", seconds * 1e3,
           16 / seconds, EVP_has_aes_hardware() ? "yes" : "no");
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--measure") == 0) return measure();

    uint8_t digest[SHA256_DIGEST_LENGTH];
    SHA256((const uint8_t *)"abc", 3, digest);
    printf("SHA-256(\"abc\"): ");
    print_hex(digest, sizeof digest);
    printf("\n");

    const char *key = "Jefe", *data = "what do ya want for nothing?";
    uint8_t mac[EVP_MAX_MD_SIZE];
    unsigned mac_length = 0;
    printf("HMAC-SHA256, RFC 4231 case 2: ");
    if (HMAC(EVP_sha256(), key, strlen(key), (const uint8_t *)data, strlen(data), mac, &mac_length) != NULL) {
        print_hex(mac, mac_length);
    } else {
        printf("failed");
    }
    printf("\n");

    uint8_t gcm_key[16], nonce[12], plaintext[60], ad[20], sealed[sizeof plaintext + 16], opened[sizeof plaintext];
    from_hex("feffe9928665731c6d6a8f9467308308", gcm_key);
    from_hex("cafebabefacedbaddecaf888", nonce);
    size_t plaintext_length = from_hex("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
                                       "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39", plaintext);
    size_t ad_length = from_hex("feedfacedeadbeeffeedfacedeadbeefabaddad2", ad);
    EVP_AEAD_CTX aead;
    size_t sealed_length = 0, opened_length = 0;
    printf("AES-128-GCM, GCM test case 4: ");
    if (EVP_AEAD_CTX_init(&aead, EVP_aead_aes_128_gcm(), gcm_key, sizeof gcm_key, EVP_AEAD_DEFAULT_TAG_LENGTH, NULL) &&
        EVP_AEAD_CTX_seal(&aead, sealed, &sealed_length, sizeof sealed, nonce, sizeof nonce, plaintext,
                          plaintext_length, ad, ad_length)) {
        print_hex(sealed, sealed_length - 16);
        printf(", tag ");
        print_hex(sealed + sealed_length - 16, 16);
        int opens = EVP_AEAD_CTX_open(&aead, opened, &opened_length, sizeof opened, nonce, sizeof nonce, sealed,
                                      sealed_length, ad, ad_length) &&
                    opened_length == plaintext_length && memcmp(opened, plaintext, plaintext_length) == 0;
        sealed[sealed_length - 1] ^= 1;
        int forged = EVP_AEAD_CTX_open(&aead, opened, &opened_length, sizeof opened, nonce, sizeof nonce, sealed,
                                       sealed_length, ad, ad_length);
        printf(", opens: %s, with a changed tag: %s", opens ? "yes" : "no", forged ? "yes" : "no");
        EVP_AEAD_CTX_cleanup(&aead);
    } else {
        printf("failed");
    }
    printf("\n");

    uint8_t alice_private[32], bob_public[32], alice_public[32], shared[32];
    from_hex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", alice_private);
    from_hex("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f", bob_public);
    X25519_public_from_private(alice_public, alice_private);
    printf("X25519, RFC 7748 6.1: public ");
    print_hex(alice_public, sizeof alice_public);
    printf(", shared ");
    if (X25519(shared, alice_private, bob_public)) {
        print_hex(shared, sizeof shared);
    } else {
        printf("failed");
    }
    printf("\n");

    printf("AES-GCM on the crypto extension: %s\n", EVP_has_aes_hardware() ? "yes" : "no");

    SSL_CTX *context = SSL_CTX_new(TLS_method());
    if (context == NULL) {
        printf("libssl: SSL_CTX_new failed\n");
        return 1;
    }
    STACK_OF(SSL_CIPHER) *ciphers = SSL_CTX_get_ciphers(context);
    printf("libssl: %zu ciphers, first %s\n", sk_SSL_CIPHER_num(ciphers),
           SSL_CIPHER_standard_name(sk_SSL_CIPHER_value(ciphers, 0)));
    SSL_CTX_free(context);
    return 0;
}
