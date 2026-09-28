/* libGLES's functions written by hand (ADR-0029, decisions 14 and 16; ADR-0030, decision 4):
   - The extension filter: glGetString(GL_EXTENSIONS) and glGetStringi leave out the extensions that
     tools/frontgen withholds (gles_withheld.inc), and GL_NUM_EXTENSIONS counts what glGetStringi lists, in
     every query that answers it. ANGLE's string stays as it is, so a filtered copy is kept per string.
   - glClientWaitSync waits in slices of at most 10 ms, so a guest signal waits one slice at most; its flush
     is glFlush, which does not wait for Metal to schedule the command buffer.
   - The debug callback and glGetPointerv's answers about it pass on here: graphics/host's functions in
     their slots put a host function of theirs in front of the app's (decision 16). */
#include <float.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "libgles.h"

MINATO_HOST(glGetString);
MINATO_HOST(glGetStringi);
MINATO_HOST(glGetBooleanv);
MINATO_HOST(glGetFloatv);
MINATO_HOST(glGetIntegerv);
MINATO_HOST(glGetInteger64v);
MINATO_HOST(glDebugMessageCallback);
MINATO_HOST(glDebugMessageCallbackKHR);
MINATO_HOST(glGetPointerv);
MINATO_HOST(glGetPointervKHR);
MINATO_HOST(glClientWaitSync);

/* GL_ANGLE_robust_client_memory's forms of the queries, which no NDK header declares. */
void glGetBooleanvRobustANGLE(GLenum pname, GLsizei size, GLsizei *length, GLboolean *data);
void glGetFloatvRobustANGLE(GLenum pname, GLsizei size, GLsizei *length, GLfloat *data);
void glGetIntegervRobustANGLE(GLenum pname, GLsizei size, GLsizei *length, GLint *data);
void glGetInteger64vRobustANGLE(GLenum pname, GLsizei size, GLsizei *length, GLint64 *data);
MINATO_HOST(glGetBooleanvRobustANGLE);
MINATO_HOST(glGetFloatvRobustANGLE);
MINATO_HOST(glGetIntegervRobustANGLE);
MINATO_HOST(glGetInteger64vRobustANGLE);

/* The extensions withheld by name, and by a prefix, which also takes names beyond the registry
   (EGL_ANGLE_platform_angle_display_key). The first entry of each is empty. */
static const char *const withheld[] = {
    "",
#define MINATO_WITHHELD(name) #name,
#define MINATO_WITHHELD_PREFIX(prefix)
#include "gles_withheld.inc"
#undef MINATO_WITHHELD
#undef MINATO_WITHHELD_PREFIX
};
static const char *const withheld_prefixes[] = {
    "",
#define MINATO_WITHHELD(name)
#define MINATO_WITHHELD_PREFIX(prefix) #prefix,
#include "gles_withheld.inc"
#undef MINATO_WITHHELD
#undef MINATO_WITHHELD_PREFIX
};

static int is_withheld(const char *name, size_t length) {
    for (size_t index = 1; index < sizeof withheld / sizeof *withheld; index++) {
        if (strlen(withheld[index]) == length && memcmp(withheld[index], name, length) == 0) {
            return 1;
        }
    }
    for (size_t index = 1; index < sizeof withheld_prefixes / sizeof *withheld_prefixes; index++) {
        const size_t prefix = strlen(withheld_prefixes[index]);
        if (prefix <= length && memcmp(withheld_prefixes[index], name, prefix) == 0) {
            return 1;
        }
    }
    return 0;
}

/* One of ANGLE's extension strings and what is kept of it: the names, and each one's index in ANGLE's
   list, which glGetStringi numbers as the string orders them. Kept for the process's life: a context's
   string stays, and contexts bring few of them. */
struct filtered {
    struct filtered *next;
    char *angle;
    char *text;
    GLuint count;
    GLuint *indices;
};
static struct filtered *filtered;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static struct filtered *make(const char *angle) {
    const size_t length = strlen(angle);
    GLuint names = 0;
    for (const char *at = angle; *at != '\0'; at++) {
        names += *at != ' ' && (at == angle || at[-1] == ' ');
    }
    struct filtered *made = calloc(1, sizeof *made);
    if (made == NULL || (made->angle = strdup(angle)) == NULL || (made->text = malloc(length + 1)) == NULL ||
        (made->indices = malloc((names + 1) * sizeof *made->indices)) == NULL) {
        if (made != NULL) {
            free(made->angle);
            free(made->text);
            free(made);
        }
        return NULL;
    }
    char *out = made->text;
    GLuint index = 0;
    for (const char *at = angle; *at != '\0';) {
        if (*at == ' ') {
            at++;
            continue;
        }
        const size_t size = strcspn(at, " ");
        if (!is_withheld(at, size)) {
            if (out != made->text) {
                *out++ = ' ';
            }
            memcpy(out, at, size);
            out += size;
            made->indices[made->count++] = index;
        }
        index++;
        at += size;
    }
    *out = '\0';
    return made;
}

/* What is kept of ANGLE's string, or NULL without memory. */
static const struct filtered *filter(const char *angle) {
    pthread_mutex_lock(&lock);
    struct filtered *entry = filtered;
    while (entry != NULL && strcmp(entry->angle, angle) != 0) {
        entry = entry->next;
    }
    if (entry == NULL && (entry = make(angle)) != NULL) {
        entry->next = filtered;
        filtered = entry;
    }
    pthread_mutex_unlock(&lock);
    return entry;
}

/* The current context's extensions as kept, or NULL without one. */
static const struct filtered *current(void) {
    const GLubyte *angle = minato_host_glGetString(GL_EXTENSIONS);
    return angle != NULL ? filter((const char *)angle) : NULL;
}

const GLubyte *glGetString(GLenum name) {
    const GLubyte *string = minato_host_glGetString(name);
    if (name != GL_EXTENSIONS || string == NULL) {
        return string;
    }
    const struct filtered *kept = filter((const char *)string);
    return kept != NULL ? (const GLubyte *)kept->text : string;
}

const GLubyte *glGetStringi(GLenum name, GLuint index) {
    const struct filtered *kept = name == GL_EXTENSIONS ? current() : NULL;
    if (kept == NULL) {
        return minato_host_glGetStringi(name, index);
    }
    /* ANGLE's own name at its index; past the names kept, an index past all of ANGLE's, which it answers
       with GL_INVALID_VALUE. */
    return minato_host_glGetStringi(name, index < kept->count ? kept->indices[index] : UINT32_MAX);
}

/* The queries of GL_NUM_EXTENSIONS: ANGLE's first, which may refuse it (an ES 2.0 context) and then leaves
   the value alone; a value it gave becomes the count of the names kept. */
#define COUNTED(type, name, unset, as)                                                        \
    void name(GLenum pname, type *data) {                                                     \
        if (pname != GL_NUM_EXTENSIONS || data == NULL) {                                     \
            minato_host_##name(pname, data);                                                  \
            return;                                                                           \
        }                                                                                     \
        const type before = *data;                                                            \
        *data = (unset);                                                                      \
        minato_host_##name(pname, data);                                                      \
        const struct filtered *kept = *data != (unset) ? current() : NULL;                    \
        *data = *data == (unset) ? before : kept != NULL ? (as) : *data;                      \
    }
COUNTED(GLint, glGetIntegerv, INT32_MIN, (GLint)kept->count)
COUNTED(GLint64, glGetInteger64v, INT64_MIN, (GLint64)kept->count)
COUNTED(GLfloat, glGetFloatv, -FLT_MAX, (GLfloat)kept->count)
COUNTED(GLboolean, glGetBooleanv, 0x5a, kept->count != 0 ? GL_TRUE : GL_FALSE)
#undef COUNTED

#define COUNTED_ROBUST(type, name, unset, as)                                                 \
    void name(GLenum pname, GLsizei size, GLsizei *length, type *data) {                      \
        if (pname != GL_NUM_EXTENSIONS || data == NULL || size < 1) {                         \
            minato_host_##name(pname, size, length, data);                                    \
            return;                                                                           \
        }                                                                                     \
        const type before = *data;                                                            \
        *data = (unset);                                                                      \
        minato_host_##name(pname, size, length, data);                                        \
        const struct filtered *kept = *data != (unset) ? current() : NULL;                    \
        *data = *data == (unset) ? before : kept != NULL ? (as) : *data;                      \
    }
COUNTED_ROBUST(GLint, glGetIntegervRobustANGLE, INT32_MIN, (GLint)kept->count)
COUNTED_ROBUST(GLint64, glGetInteger64vRobustANGLE, INT64_MIN, (GLint64)kept->count)
COUNTED_ROBUST(GLfloat, glGetFloatvRobustANGLE, -FLT_MAX, (GLfloat)kept->count)
COUNTED_ROBUST(GLboolean, glGetBooleanvRobustANGLE, 0x5a, kept->count != 0 ? GL_TRUE : GL_FALSE)
#undef COUNTED_ROBUST

void glDebugMessageCallback(GLDEBUGPROC callback, const void *user) {
    minato_host_glDebugMessageCallback(callback, user);
}

void glDebugMessageCallbackKHR(GLDEBUGPROCKHR callback, const void *user) {
    minato_host_glDebugMessageCallbackKHR(callback, user);
}

void glGetPointerv(GLenum pname, void **params) {
    minato_host_glGetPointerv(pname, params);
}

void glGetPointervKHR(GLenum pname, void **params) {
    minato_host_glGetPointervKHR(pname, params);
}

#define SLICE_NS 10000000u

static uint64_t now_ns(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (uint64_t)time.tv_sec * 1000000000u + (uint64_t)time.tv_nsec;
}

/* In slices of at most 10 ms, with a return to guest code between them (ADR-0030, decision 4); a timeout
   of one slice or less goes as it is. A sync signaled after the first slice was satisfied by the wait, not
   signaled already. ANGLE flushes for GL_SYNC_FLUSH_COMMANDS_BIT until Metal scheduled the command buffer,
   which a buffer behind a GPU wait is not, in one host call: once the sync is not signaled already, the
   flush is glFlush here, which does not wait. */
GLenum glClientWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout) {
    const uint64_t start = now_ns();
    if (flags & GL_SYNC_FLUSH_COMMANDS_BIT) {
        flags &= ~(GLbitfield)GL_SYNC_FLUSH_COMMANDS_BIT;
        const GLenum now = minato_host_glClientWaitSync(sync, flags, 0);
        if (now != GL_TIMEOUT_EXPIRED || timeout == 0) {
            return now;
        }
        glFlush();
    }
    for (int slices = 0;; slices++) {
        const uint64_t waited = now_ns() - start, left = waited < timeout ? timeout - waited : 0;
        const GLenum result = minato_host_glClientWaitSync(sync, flags, left < SLICE_NS ? left : SLICE_NS);
        if (result != GL_TIMEOUT_EXPIRED || left <= SLICE_NS) {
            return result == GL_ALREADY_SIGNALED && slices > 0 ? GL_CONDITION_SATISFIED : result;
        }
    }
}
