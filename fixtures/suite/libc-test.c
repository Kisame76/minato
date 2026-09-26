// libc-test: the C library apart from files, threads and signals (M2 step 5c): strings, formatted
// output and input, number conversions, memory, sorting, time, the environment, locale and errno.
// Every line is exactly specified by C, POSIX or bionic's sources, so Android prints the same.
// Expected output: fixtures/expected/libc-test.out
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <locale.h>
#include <malloc.h>
#include <math.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

static void registered_first(void) {
    printf("atexit: registered first, runs last\n");
}

static void registered_second(void) {
    printf("atexit: registered second, runs first\n");
}

static int compare_ints(const void *left, const void *right) {
    const int a = *(const int *)left, b = *(const int *)right;
    return (a > b) - (a < b);
}

static void strings(void) {
    char buffer[64];
    printf("strlen: %zu\n", strlen("android"));
    printf("strcmp: %d %d %d\n", strcmp("a", "b") < 0, strcmp("b", "a") > 0, strcmp("same", "same") == 0);
    strcpy(buffer, "hello");
    strcat(buffer, ", world");
    printf("strcat: %s\n", buffer);
    printf("strchr: %td strrchr: %td\n", strchr(buffer, 'o') - buffer, strrchr(buffer, 'o') - buffer);
    printf("strstr: %s\n", strstr(buffer, "wor"));
    printf("strncmp: %d strcasecmp: %d\n", strncmp("android", "andromeda", 5), strcasecmp("HeLLo", "hello"));
    printf("strspn: %zu strcspn: %zu\n", strspn("aabbc", "ab"), strcspn("hello", "lo"));
    char tokens[] = "a,b;;c";
    char *state = NULL;
    printf("strtok_r:");
    for (char *token = strtok_r(tokens, ",;", &state); token != NULL; token = strtok_r(NULL, ",;", &state)) {
        printf(" [%s]", token);
    }
    printf("\n");
    char overlap[] = "abcdef";
    memmove(overlap + 2, overlap, 4);
    printf("memmove: %s\n", overlap);
    const char haystack[] = "xyzzy";
    printf("memchr: %td memrchr: %td\n", (const char *)memchr(haystack, 'z', 5) - haystack,
           (const char *)memrchr(haystack, 'z', 5) - haystack);
    char *copy = strdup("duplicate");
    char *part = strndup("truncate", 5);
    printf("strdup: %s strndup: %s\n", copy, part);
    free(copy);
    free(part);
    char large[4096];
    memset(large, 'a', sizeof large - 1);
    large[sizeof large - 1] = '\0';
    large[3000] = 'b';
    printf("long strings: strlen %zu, strchr %td\n", strlen(large), strchr(large, 'b') - large);
    printf("strerror: %s | %s\n", strerror(ENOENT), strerror(ERANGE));
    printf("strsignal: %s\n", strsignal(SIGSEGV));
}

static void formatting(void) {
    printf("%d|%5d|%-5d|%05d|%+d\n", 42, 42, 42, 42, 42);
    printf("%x|%X|%#x|%o|%#o\n", 255, 255, 255, 8, 8);
    printf("%lld|%llu|%" PRId64 "\n", LLONG_MIN, ULLONG_MAX, INT64_C(-1));
    printf("%zu|%zd|%td\n", (size_t)123, (ssize_t)-5, (ptrdiff_t)7);
    printf("%.3s|%10.4s|%-6s|\n", "abcdef", "abcdef", "ab");
    printf("%c%c%c|%%\n", 'o', 'k', '!');
    printf("%2$s %1$s\n", "world", "hello");
    printf("%f|%.2f|%10.3f|%-10.1f|\n", 3.14159265, 2.675, -1.5, 0.05);
    printf("%e|%.0e|%E\n", 12345.678, 7.0e-10, 1e100);
    printf("%g|%g|%g|%g|%g\n", 0.0001, 0.00001, 123456.0, 1234567.0, 100.0);
    printf("%a|%a|%A\n", 1.0, 0.1, -2.5);
    printf("%.17g|%.17g\n", 0.1, 1.0 / 3);
    printf("%f|%f|%f|%g|%F\n", INFINITY, -INFINITY, NAN, -0.0, INFINITY);
    volatile long double three = 3;
    printf("long double: %zu bytes, %.30Lf\n", sizeof(long double), 1 / three);
    char small[5];
    const int wanted = snprintf(small, sizeof small, "%s", "truncated");
    printf("snprintf: %d [%s]\n", wanted, small);
}

static void conversions(void) {
    char *end;
    printf("strtol: %ld %ld %ld %ld\n", strtol("0x1f", NULL, 0), strtol("077", NULL, 0), strtol("-42abc", &end, 10),
           strtol("z", NULL, 36));
    printf("strtol stops at: %s\n", end);
    errno = 0;
    const long overflow = strtol("99999999999999999999", NULL, 10);
    printf("strtol overflow: %ld errno %d\n", overflow, errno);
    printf("strtoul: %lu\n", strtoul("-1", NULL, 10));
    printf("atoi: %d atol: %ld\n", atoi("  -17xyz"), atol("123456789012"));
    printf("strtod: %.17g %.17g %g\n", strtod("0.1", NULL), strtod("0x1.8p1", NULL), strtod("-inf", NULL));
    printf("strtof: %.9g\n", strtof("3.4028235e38", NULL));
    int first = 0, second = 0;
    char word[16] = "";
    float fraction = 0;
    const int matched = sscanf("12 34 word 2.5", "%d %d %15s %f", &first, &second, word, &fraction);
    printf("sscanf: %d -> %d %d %s %.1f\n", matched, first, second, word, fraction);
    printf("ctype: %d %d %d %c %c\n", isalpha('a') != 0, isdigit('5') != 0, isspace('\t') != 0, toupper('q'),
           tolower('Q'));
}

static void memory(void) {
    char *block = malloc(100);
    memset(block, 'x', 100);
    block = realloc(block, 100000);
    int kept = 1;
    for (int i = 0; i < 100; ++i) kept &= block[i] == 'x';
    printf("realloc keeps the contents: %s, usable size big enough: %s\n", kept ? "yes" : "no",
           malloc_usable_size(block) >= 100000 ? "yes" : "no");
    free(block);

    int *zeros = calloc(1000, sizeof *zeros);
    int all_zero = 1;
    for (int i = 0; i < 1000; ++i) all_zero &= zeros[i] == 0;
    printf("calloc zeroes: %s\n", all_zero ? "yes" : "no");
    free(zeros);

    void *aligned = NULL;
    const int result = posix_memalign(&aligned, 4096, 100);
    printf("posix_memalign: %d, aligned: %s\n", result, (uintptr_t)aligned % 4096 == 0 ? "yes" : "no");
    free(aligned);

    // Many blocks of many sizes, each filled with its own pattern and checked before it is freed. At
    // most 58 MiB of one size class at a time: more than the allocator's region of 64 MiB would spill
    // over here, with a message, and not on Android, whose regions have 256 MiB (ADR-0035).
    enum { count = 2000 };
    static unsigned char *blocks[count];
    static size_t sizes[count];
    unsigned long checked = 0;
    for (int round = 0; round < 3; ++round) {
        for (int i = 0; i < count; ++i) {
            sizes[i] = (size_t)((i * 7919 + round * 104729) % 70000) + 1;
            blocks[i] = malloc(sizes[i]);
            memset(blocks[i], (i + round) & 0xff, sizes[i]);
        }
        for (int i = 0; i < count; ++i) {
            for (size_t j = 0; j < sizes[i]; j += 97) checked += blocks[i][j] == ((i + round) & 0xff);
            free(blocks[i]);
        }
    }
    printf("malloc stress: %lu bytes checked\n", checked);
}

static void sorting(void) {
    int values[] = {5, 3, 9, 1, 7, 2, 8, 6, 4, 0};
    qsort(values, 10, sizeof values[0], compare_ints);
    printf("qsort:");
    for (int i = 0; i < 10; ++i) printf(" %d", values[i]);
    printf("\n");
    const int key = 7;
    const int *found = bsearch(&key, values, 10, sizeof values[0], compare_ints);
    printf("bsearch: %td\n", found - values);
}

static void times(void) {
    const time_t moment = 1700000000;
    struct tm broken;
    gmtime_r(&moment, &broken);
    char text[64];
    strftime(text, sizeof text, "%Y-%m-%d %H:%M:%S %a %b %j", &broken);
    printf("gmtime: %s, wday %d, yday %d\n", text, broken.tm_wday, broken.tm_yday);
    printf("timegm: %lld\n", (long long)timegm(&broken));
    printf("difftime: %.1f\n", difftime(moment + 90, moment));

    struct timespec before, after;
    clock_gettime(CLOCK_MONOTONIC, &before);
    const struct timespec pause = {0, 20 * 1000 * 1000};
    const int slept = nanosleep(&pause, NULL);
    clock_gettime(CLOCK_MONOTONIC, &after);
    const long long elapsed = (after.tv_sec - before.tv_sec) * 1000000000LL + (after.tv_nsec - before.tv_nsec);
    printf("nanosleep: %d, at least 20 ms passed: %s\n", slept, elapsed >= 20 * 1000 * 1000 ? "yes" : "no");
    const time_t now = time(NULL);
    printf("time after 2023: %s\n", now > moment ? "yes" : "no");

    // Time zones come from the system's tzdata, the same rules on every device for these dates.
    setenv("TZ", "Europe/Berlin", 1);
    tzset();
    const time_t moments[] = {moment, 1690000000};
    for (int i = 0; i < 2; ++i) {
        struct tm local;
        localtime_r(&moments[i], &local);
        strftime(text, sizeof text, "%Y-%m-%d %H:%M:%S %Z", &local);
        printf("localtime Europe/Berlin: %s, isdst %d, gmtoff %ld, mktime back: %s\n", text, local.tm_isdst,
               local.tm_gmtoff, mktime(&local) == moments[i] ? "yes" : "no");
    }
    unsetenv("TZ");
    tzset();
}

static void environment(void) {
    setenv("APKRT_TEST", "first", 1);
    setenv("APKRT_TEST", "second", 0);
    const char *kept = getenv("APKRT_TEST");
    printf("setenv without overwrite keeps: %s\n", kept);
    setenv("APKRT_TEST", "third", 1);
    printf("setenv with overwrite: %s\n", getenv("APKRT_TEST"));
    unsetenv("APKRT_TEST");
    printf("unsetenv: %s\n", getenv("APKRT_TEST") == NULL ? "gone" : "still there");
}

static void locale_and_wide(void) {
    printf("locale: %s, MB_CUR_MAX %zu\n", setlocale(LC_ALL, NULL), (size_t)MB_CUR_MAX);
    const char *text = "a\xc3\xa4\xe2\x82\xac";  // a, a umlaut, euro sign
    wchar_t wide[8];
    const size_t count = mbstowcs(wide, text, 8);
    printf("mbstowcs: %zu ->", count);
    for (size_t i = 0; i < count; ++i) printf(" U+%04X", (unsigned)wide[i]);
    printf("\n");
    char back[16];
    printf("wcstombs: %zu, same bytes: %s\n", wcstombs(back, wide, sizeof back),
           strcmp(back, text) == 0 ? "yes" : "no");
}

static void errors(void) {
    errno = 0;
    const int result = close(-1);
    printf("close(-1): %d, errno %d (%s)\n", result, errno, strerror(errno));
    errno = 0;
    const off_t offset = lseek(-1, 0, SEEK_SET);
    printf("lseek(-1): %lld, errno %d\n", (long long)offset, errno);
}

int main(void) {
    // C wants errno 0 at program start, Android breaks that: libc.so's start-up opens
    // libnetd_client.so, and linker64 looks for it in LD_LIBRARY_PATH (/data/local/tmp) first, where
    // the failed open() leaves ENOENT (confirmed on the Android 15 emulator; 0 without LD_LIBRARY_PATH).
    printf("errno at the start: %d\n", errno);
    atexit(registered_first);
    atexit(registered_second);
    strings();
    formatting();
    conversions();
    memory();
    sorting();
    times();
    environment();
    locale_and_wide();
    errors();
    return 0;
}
