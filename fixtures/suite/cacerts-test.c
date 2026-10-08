// cacerts-test: the CA certificates of Android 15's trust store as an app finds them (ADR-0046, decision 4):
// Conscrypt's APEX folder, which TrustedCertificateStore reads from SDK 34 on, and /system/etc/security/cacerts,
// which apps and native libraries read on their own (Go's crypto/x509, Dart's): how many files each has, whether they
// have the same names, a digest of each folder's names and contents (the set, root for root), and ISRG Root X1's
// file in each, its first line and its subject.
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// FNV-1a over `bytes`, from `hash`.
static unsigned fnv(unsigned hash, const unsigned char *bytes, size_t length) {
    for (size_t each = 0; each < length; ++each) hash = (hash ^ bytes[each]) * 16777619u;
    return hash;
}

// The digest of the sorted `names` in `folder` and of each file's bytes.
static unsigned digest_of(const char *folder, char **names, int count) {
    unsigned hash = 2166136261u;
    for (int each = 0; each < count; ++each) {
        hash = fnv(hash, (const unsigned char *)names[each], strlen(names[each]) + 1);
        char path[512];
        snprintf(path, sizeof path, "%s/%s", folder, names[each]);
        FILE *file = fopen(path, "rb");
        if (file == NULL) continue;
        unsigned char buffer[4096];
        for (size_t got; (got = fread(buffer, 1, sizeof buffer, file)) > 0;) hash = fnv(hash, buffer, got);
        fclose(file);
    }
    return hash;
}

static int compare(const void *one, const void *other) {
    return strcmp(*(char *const *)one, *(char *const *)other);
}

// The names in `folder` but . and .., sorted into `out`; their number, or -1 for a folder that is not there.
static int names_of(const char *folder, char ***out) {
    DIR *dir = opendir(folder);
    if (dir == NULL) return -1;
    int count = 0, room = 0;
    char **names = NULL;
    for (struct dirent *each; (each = readdir(dir)) != NULL;) {
        if (strcmp(each->d_name, ".") == 0 || strcmp(each->d_name, "..") == 0) continue;
        if (count == room) {
            room = room == 0 ? 64 : room * 2;
            names = realloc(names, (size_t)room * sizeof *names);
        }
        names[count++] = strdup(each->d_name);
    }
    closedir(dir);
    if (count > 0) qsort(names, (size_t)count, sizeof *names, compare);
    *out = names;
    return count;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    char **apex = NULL, **system = NULL;
    const int in_apex = names_of("/apex/com.android.conscrypt/cacerts", &apex);
    const int in_system = names_of("/system/etc/security/cacerts", &system);
    printf("/apex/com.android.conscrypt/cacerts: %d files\n", in_apex);
    printf("/system/etc/security/cacerts: %d files\n", in_system);
    int same = in_apex > 0 && in_apex == in_system;
    for (int each = 0; same && each < in_apex; ++each) same = strcmp(apex[each], system[each]) == 0;
    printf("the same names: %s\n", same ? "yes" : "no");
    printf("digests of names and contents: %08x, %08x\n",
           digest_of("/apex/com.android.conscrypt/cacerts", apex, in_apex > 0 ? in_apex : 0),
           digest_of("/system/etc/security/cacerts", system, in_system > 0 ? in_system : 0));
    const char *paths[] = {"/apex/com.android.conscrypt/cacerts/6187b673.0", "/system/etc/security/cacerts/6187b673.0"};
    for (int each = 0; each < 2; ++each) {
        FILE *file = fopen(paths[each], "r");
        if (file == NULL) {
            printf("%s: not there\n", paths[each]);
            continue;
        }
        char line[256], first[256] = "", subject[256] = "";
        while (fgets(line, sizeof line, file) != NULL) {
            if (first[0] == '\0') snprintf(first, sizeof first, "%s", line);
            const char *found = strstr(line, "Subject: ");
            if (found != NULL && subject[0] == '\0') {
                snprintf(subject, sizeof subject, "%s", found + strlen("Subject: "));
            }
        }
        fclose(file);
        first[strcspn(first, "\n")] = '\0';
        subject[strcspn(subject, "\n")] = '\0';
        printf("%s: %s, %s\n", paths[each], first, subject);
    }
    return 0;
}
