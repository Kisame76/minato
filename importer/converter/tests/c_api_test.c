// Compiles the app's converter header as plain C, the way Swift reads it, and calls it: converting
// and signing ad hoc, the kernel's check of the result, reading the certificate of a signature, and
// the refusals.
// Usage: c_api_test <scratch directory> <NDK fixtures directory> [<container signed by codesign>]
// The container codesign signed with an Apple Development certificate comes from the Xcode device
// build with the fixtures (tools/device-suite.sh); without it, those checks are skipped.
#include "APKConvert/apkconv.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;

static void expect(int condition, const char *what) {
    printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) ++failures;
}

int main(int argc, char **argv) {
    if (argc != 3 && argc != 4) {
        fprintf(stderr, "usage: c_api_test <scratch directory> <fixtures directory> [<signed container>]\n");
        return 2;
    }
    const char *scratch = argv[1];
    mkdir(scratch, 0755);
    char error[512] = "";

    expect(apkconv_signer_create(NULL, NULL, "", error, sizeof error) == NULL && strstr(error, "identifier") != NULL,
           "apkconv_signer_create refuses an empty identifier");
    apkconv_signer *signer = apkconv_signer_create(NULL, NULL, "io.example.converter-test", error, sizeof error);
    expect(signer != NULL, "apkconv_signer_create signs ad hoc without an identity");
    if (signer == NULL) return 1;

    expect(apkconv_check_signer(signer, scratch, error, sizeof error) == 0, "apkconv_check_signer: the kernel takes it");

    apkconv_self_test_result result = {0};
    const int tested = apkconv_self_test(signer, scratch, &result, error, sizeof error);
    if (tested != 0) fprintf(stderr, "apkconv_self_test: %s\n", error);
    expect(tested == 0 && result.value == 42 && result.sign_milliseconds >= 0 && result.load_milliseconds >= 0,
           "apkconv_self_test signs, loads and calls the test container: 42");
    error[0] = '\0';
    expect(apkconv_self_test(signer, "/nonexistent/folder", &result, error, sizeof error) == -1 && error[0] != '\0',
           "apkconv_self_test names a folder it cannot write to");

    unsigned char key[4096];
    unsigned char key_certificate[4096];
    size_t key_length = 0;
    size_t key_certificate_length = 0;
    error[0] = '\0';
    expect(apkconv_read_unencrypted_pkcs12("garbage, not PKCS12", 19, key, sizeof key, &key_length, key_certificate,
                                           sizeof key_certificate, &key_certificate_length, error, sizeof error) == -1 &&
               error[0] != '\0',
           "apkconv_read_unencrypted_pkcs12 refuses garbage");

    char fixture[1024];
    char container[1024];
    snprintf(fixture, sizeof fixture, "%s/raw-hello", argv[2]);
    snprintf(container, sizeof container, "%s/raw-hello.dylib", scratch);
    if (access(fixture, R_OK) == 0) {
        apkconv_statistics statistics = {0};
        const int converted = apkconv_convert(signer, fixture, container, &statistics, error, sizeof error);
        if (converted != 0) fprintf(stderr, "apkconv_convert: %s\n", error);
        expect(converted == 0 && statistics.container_bytes > 0, "apkconv_convert converts and signs raw-hello");
        expect(apkconv_check_signature(container, error, sizeof error) == 0,
               "apkconv_check_signature: the kernel takes the container");
        char team[64] = "unset";
        expect(apkconv_signing_team(container, team, sizeof team, error, sizeof error) == 0 && team[0] == '\0',
               "apkconv_signing_team is empty for an ad-hoc signature");
        unsigned char leaf[8192];
        size_t length = 0;
        error[0] = '\0';
        expect(apkconv_signing_certificate(container, leaf, sizeof leaf, &length, error, sizeof error) == -1 &&
                   strstr(error, "ad hoc") != NULL,
               "apkconv_signing_certificate: an ad-hoc signature has no certificate");
        // Converting again replaces the file.
        expect(apkconv_convert(signer, fixture, container, NULL, error, sizeof error) == 0,
               "apkconv_convert replaces an existing container");

        // A valid ELF file for another machine: e_machine, at offset 18, set to EM_X86_64 (62).
        char foreign[1024];
        snprintf(foreign, sizeof foreign, "%s/foreign-machine.so", scratch);
        FILE *source = fopen(fixture, "rb");
        FILE *copy = fopen(foreign, "wb");
        long offset = 0;
        for (int byte; (byte = fgetc(source)) != EOF; ++offset) {
            fputc(offset == 18 ? 62 : offset == 19 ? 0 : byte, copy);
        }
        fclose(source);
        fclose(copy);
        error[0] = '\0';
        expect(apkconv_convert(signer, foreign, container, NULL, error, sizeof error) == -1 &&
                   strncmp(error, "incompatible: ", 14) == 0 && strstr(error, "AArch64") != NULL,
               "apkconv_convert refuses code for another machine as incompatible");
    } else {
        printf("     (fixtures missing, conversion skipped)\n");
    }

    // "incompatible: " marks a file that cannot run on iOS, which the app shows as incompatible (ADR-0032);
    // a file it cannot open is no such case.
    error[0] = '\0';
    expect(apkconv_convert(signer, "/nonexistent/libgame.so", container, NULL, error, sizeof error) == -1 &&
               strstr(error, "/nonexistent/libgame.so") != NULL && strncmp(error, "incompatible: ", 14) != 0,
           "apkconv_convert names a file it cannot open");
    char not_elf[1024];
    snprintf(not_elf, sizeof not_elf, "%s/not-elf.so", scratch);
    FILE *file = fopen(not_elf, "w");
    fputs("definitely not an ELF file, but long enough to be looked at by the reader.", file);
    fclose(file);
    error[0] = '\0';
    expect(apkconv_convert(signer, not_elf, container, NULL, error, sizeof error) == -1 &&
               strncmp(error, "incompatible: ", 14) == 0,
           "apkconv_convert refuses a file that is not ELF as incompatible");
    char empty[1024];
    snprintf(empty, sizeof empty, "%s/empty.so", scratch);
    fclose(fopen(empty, "w"));
    error[0] = '\0';
    expect(apkconv_convert(signer, empty, container, NULL, error, sizeof error) == -1 &&
               strncmp(error, "incompatible: ", 14) == 0 && strstr(error, "empty") != NULL,
           "apkconv_convert refuses an empty file as incompatible");
    error[0] = '\0';
    expect(apkconv_check_signature(not_elf, error, sizeof error) == -1 && strstr(error, "Mach-O") != NULL,
           "apkconv_check_signature refuses a file that is not Mach-O");

    apkconv_certificate certificate;
    error[0] = '\0';
    expect(apkconv_describe_certificate("garbage", 7, &certificate, error, sizeof error) == -1 && error[0] != '\0',
           "apkconv_describe_certificate refuses garbage");
    char team[64];
    error[0] = '\0';
    expect(apkconv_signing_team("/nonexistent/app", team, sizeof team, error, sizeof error) == -1 &&
               strstr(error, "/nonexistent/app") != NULL,
           "apkconv_signing_team names a file it cannot open");

    unsigned char leaf[8192];
    size_t length = 0;
    error[0] = '\0';
    expect(apkconv_signing_certificate("/nonexistent/app", leaf, sizeof leaf, &length, error, sizeof error) == -1 &&
               strstr(error, "/nonexistent/app") != NULL,
           "apkconv_signing_certificate names a file it cannot open");
    if (argc == 4 && access(argv[3], R_OK) == 0) {
        const int read = apkconv_signing_certificate(argv[3], leaf, sizeof leaf, &length, error, sizeof error);
        if (read != 0) fprintf(stderr, "apkconv_signing_certificate: %s\n", error);
        expect(read == 0 && length > 0 && length <= sizeof leaf,
               "apkconv_signing_certificate reads the certificate of an Apple Development signature");
        apkconv_certificate described;
        char signing_team[64] = "";
        expect(read == 0 && apkconv_describe_certificate(leaf, length, &described, error, sizeof error) == 0 &&
                   apkconv_signing_team(argv[3], signing_team, sizeof signing_team, error, sizeof error) == 0 &&
                   signing_team[0] != '\0' && strcmp(described.team, signing_team) == 0,
               "the certificate belongs to the team of the signature");
        size_t needed = 0;
        error[0] = '\0';
        expect(apkconv_signing_certificate(argv[3], leaf, 16, &needed, error, sizeof error) == -1 &&
                   needed == length && error[0] != '\0',
               "apkconv_signing_certificate says how large a buffer it needs");
    } else {
        printf("     (no container signed with an Apple Development certificate, skipped)\n");
    }

    apkconv_signer_free(signer);
    return failures == 0 ? 0 : 1;
}
