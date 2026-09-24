// C interface of the converter for the app (ADR-0008, ADR-0022): ELF files of an APK in, signed
// container dylibs out, on the iPhone itself. Plain C, so Swift can import it as the module
// APKConvert (module.modulemap).
#ifndef APKCONVERT_APKCONV_H
#define APKCONVERT_APKCONV_H

#include <Security/Security.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Signs containers: with a certificate on the iPhone, ad hoc on the Simulator and the Mac.
typedef struct apkconv_signer apkconv_signer;

/// Prepares signing with `identity`, e.g. the app's development certificate from the keychain.
/// Its certificate chain is completed from the code signature of the Mach-O file `chain_source`:
/// the app's own executable, which Xcode or SideStore signed with the same chain. The certificate
/// must belong to the team the app is signed for, or iOS would not load what it signs. With
/// `identity` NULL the signer signs ad hoc, which is enough on the Simulator; `chain_source` is
/// then not read. `identifier` becomes every container's signing identifier: the app's bundle ID,
/// which iOS checks against the app's provisioning profile (ADR-0008, spike S1b).
/// Returns NULL with the reason in `error`.
apkconv_signer *apkconv_signer_create(SecIdentityRef identity, const char *chain_source, const char *identifier,
                                      char *error, size_t error_size);

/// Like apkconv_signer_create, for a private key and its certificate (DER) that the keychain keeps
/// as two items instead of one identity, as after importing an unencrypted PKCS #12 file (ADR-0023).
apkconv_signer *apkconv_signer_create_with_key(SecKeyRef private_key, const void *certificate, size_t certificate_size,
                                               const char *chain_source, const char *identifier, char *error,
                                               size_t error_size);

void apkconv_signer_free(apkconv_signer *signer);

/// What converting one file cost.
typedef struct {
    double convert_milliseconds;
    double sign_milliseconds;
    uint64_t container_bytes;
} apkconv_statistics;

/// Converts the ELF file `elf_path` into the container dylib `container_path`, for the platform
/// this library is built for, and signs it with `signer`. The container is written to a new file
/// that then replaces any old one, because the kernel remembers signatures per file (ADR-0008).
/// `statistics` may be NULL. Returns 0, or -1 with the reason in `error`.
int apkconv_convert(const apkconv_signer *signer, const char *elf_path, const char *container_path,
                    apkconv_statistics *statistics, char *error, size_t error_size);

/// Asks the kernel whether this process may load the container `path`, the checks dyld makes
/// before it maps a file: its code signature is registered (F_ADDFILESIGS_RETURN) and library
/// validation lets it in (F_CHECK_LV). Loads nothing. Returns 0, or -1 with the reason in `error`.
int apkconv_check_signature(const char *path, char *error, size_t error_size);

/// Signs a small test container in `directory`, checks it with apkconv_check_signature and
/// removes it again: whether iOS accepts what `signer` signs, e.g. right after a certificate is
/// imported. Returns 0, or -1 with the reason in `error`.
int apkconv_check_signer(const apkconv_signer *signer, const char *directory, char *error, size_t error_size);

/// What the self-test measured.
typedef struct {
    double sign_milliseconds;
    double load_milliseconds;
    int value;  // what the test function returned: 42
} apkconv_self_test_result;

/// The self-test after a certificate import (ADR-0023): signs a small test container with one
/// function in `directory`, asks the kernel like apkconv_check_signature, loads it with dlopen as
/// the loader loads containers, and calls the function, which returns 42. The file is removed
/// again; the loaded image, a few KiB, stays mapped. Returns 0, or -1 with the reason in `error`.
int apkconv_self_test(const apkconv_signer *signer, const char *directory, apkconv_self_test_result *result,
                      char *error, size_t error_size);

/// Reads a PKCS #12 file whose key and certificate are not encrypted, as SideStore 0.6.4 and later
/// writes it, which SecPKCS12Import rejects (ADR-0023). Copies the RSA private key (PKCS #1 DER, for
/// SecKeyCreateWithData) to `key` and the certificate that belongs to it (DER) to `certificate`,
/// and sets both lengths, also when a buffer is too small. Returns 0; 1 when the file is
/// encrypted, which is SecPKCS12Import's case; -1 with the reason in `error`, also for a buffer
/// that is too small.
int apkconv_read_unencrypted_pkcs12(const void *pkcs12, size_t size, void *key, size_t key_size, size_t *key_length,
                                    void *certificate, size_t certificate_size, size_t *certificate_length,
                                    char *error, size_t error_size);

/// What the app shows about a certificate.
typedef struct {
    char common_name[256];   // subject CN, e.g. "Apple Development: …"
    char team[64];           // subject OU: the team ID
    char serial_number[80];  // uppercase hex, two digits per byte, leading zero bytes dropped
    int64_t not_before;      // start of its validity, in seconds since 1970
    int64_t not_after;       // end of its validity, in seconds since 1970
} apkconv_certificate;

/// Reads the DER certificate `der` of `size` bytes. Returns 0, or -1 with the reason in `error`.
int apkconv_describe_certificate(const void *der, size_t size, apkconv_certificate *certificate, char *error,
                                 size_t error_size);

/// The team ID in the code signature of the Mach-O file `path`, "" for an ad-hoc signature.
/// Returns 0, or -1 with the reason in `error`.
int apkconv_signing_team(const char *path, char *team, size_t team_size, char *error, size_t error_size);

/// The common names of the certificates in the code signature of the Mach-O file `path`, one per
/// line, in the order the signature carries them; empty for an ad-hoc signature. For the app's own
/// executable these are the certificates the signer completes an imported one's chain from
/// (ADR-0022), which the diagnostics show (ADR-0023). Returns 0, or -1 with the reason in `error`.
int apkconv_signing_chain(const char *path, char *names, size_t size, char *error, size_t error_size);

/// The certificate that signed the Mach-O file `path`, in DER: the leaf of its code signature. For
/// the app's own executable it is the certificate Xcode or SideStore installed the app with, which
/// iOS accepts for converted code. Copies it to `certificate`, which holds `size` bytes, and sets
/// `length` to its length, also when it does not fit. Returns 0, or -1 with the reason in `error`,
/// also for an ad-hoc signature and for a buffer that is too small.
int apkconv_signing_certificate(const char *path, void *certificate, size_t size, size_t *length, char *error,
                                size_t error_size);

#ifdef __cplusplus
}
#endif

#endif
