// libmediandk.so's AMediaFormat (ADR-0060 e2b3): named values of one type each, in the order they were first set, as
// Android's AMessage keeps them. A getter answers false for a name that is not there or has another type; a set
// replaces a name's value and type in place. A string or a buffer a getter gives stays valid until the format
// changes or goes, as Android's do. toString gives "name: type(value)" for each entry, joined by ", " [from memory
// of AOSP's NdkMediaFormat.cpp, not read again]. minato_AMediaFormat_count and _entry (APKRuntime/media_codec.h) let
// the framework's natives walk the entries. Bionic's C library alone, as the rest of runtime/android: no C++ library.
#include <media/NdkMediaFormat.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "APKRuntime/media_codec.h"

namespace {

struct Entry {
    char *name;
    int kind;
    int64_t integer;
    double real;
    char *text;      // a string's
    uint8_t *bytes;  // a buffer's
    size_t size;
    int32_t rect[4];
};

}  // namespace

struct AMediaFormat {
    Entry *entries;
    size_t count, capacity;
    char *description;  // toString's
};

namespace {

void clear_value(Entry *entry) {
    free(entry->text);
    free(entry->bytes);
    entry->text = nullptr, entry->bytes = nullptr, entry->size = 0;
    entry->integer = 0, entry->real = 0;
    memset(entry->rect, 0, sizeof entry->rect);
}

Entry *find(AMediaFormat *format, const char *name, int kind) {
    if (format == nullptr || name == nullptr) return nullptr;
    for (size_t index = 0; index < format->count; index++) {
        Entry *entry = &format->entries[index];
        if (strcmp(entry->name, name) == 0) return entry->kind == kind ? entry : nullptr;
    }
    return nullptr;
}

/// The entry `name` emptied for a value of `kind`, appended when new; NULL without memory.
Entry *put(AMediaFormat *format, const char *name, int kind) {
    if (format == nullptr || name == nullptr) return nullptr;
    for (size_t index = 0; index < format->count; index++) {
        Entry *entry = &format->entries[index];
        if (strcmp(entry->name, name) == 0) {
            clear_value(entry);
            entry->kind = kind;
            return entry;
        }
    }
    if (format->count == format->capacity) {
        const size_t capacity = format->capacity == 0 ? 8 : format->capacity * 2;
        auto *grown = static_cast<Entry *>(realloc(format->entries, capacity * sizeof(Entry)));
        if (grown == nullptr) return nullptr;
        format->entries = grown, format->capacity = capacity;
    }
    char *copy = strdup(name);
    if (copy == nullptr) return nullptr;
    Entry *entry = &format->entries[format->count++];
    memset(entry, 0, sizeof *entry);
    entry->name = copy;
    entry->kind = kind;
    return entry;
}

void clear_all(AMediaFormat *format) {
    for (size_t index = 0; index < format->count; index++) {
        clear_value(&format->entries[index]);
        free(format->entries[index].name);
    }
    format->count = 0;
}

/// Appends `text` to the growing string at `*out` of `*size` bytes; false without memory.
bool append(char **out, size_t *size, const char *text) {
    const size_t used = *out != nullptr ? strlen(*out) : 0, more = strlen(text);
    if (used + more + 1 > *size) {
        const size_t grown_size = (used + more + 1) * 2;
        auto *grown = static_cast<char *>(realloc(*out, grown_size));
        if (grown == nullptr) return false;
        if (*out == nullptr) grown[0] = 0;
        *out = grown, *size = grown_size;
    }
    memcpy(*out + used, text, more + 1);
    return true;
}

}  // namespace

extern "C" {

AMediaFormat *AMediaFormat_new() {
    return static_cast<AMediaFormat *>(calloc(1, sizeof(AMediaFormat)));
}

media_status_t AMediaFormat_delete(AMediaFormat *format) {
    if (format == nullptr) return AMEDIA_OK;
    clear_all(format);
    free(format->entries);
    free(format->description);
    free(format);
    return AMEDIA_OK;
}

const char *AMediaFormat_toString(AMediaFormat *format) {
    if (format == nullptr) return nullptr;
    char *text = nullptr;
    size_t size = 0;
    char part[96];
    bool ok = append(&text, &size, "");
    for (size_t index = 0; index < format->count && ok; index++) {
        const Entry &entry = format->entries[index];
        if (index > 0) ok = append(&text, &size, ", ");
        ok = ok && append(&text, &size, entry.name) && append(&text, &size, ": ");
        switch (entry.kind) {
            case MINATO_MEDIA_FORMAT_INT32: snprintf(part, sizeof part, "int32(%d)", (int)entry.integer); break;
            case MINATO_MEDIA_FORMAT_INT64: snprintf(part, sizeof part, "int64(%lld)", (long long)entry.integer); break;
            case MINATO_MEDIA_FORMAT_SIZE:
                snprintf(part, sizeof part, "size_t(%llu)", (unsigned long long)entry.integer);
                break;
            case MINATO_MEDIA_FORMAT_FLOAT: snprintf(part, sizeof part, "float(%f)", entry.real); break;
            case MINATO_MEDIA_FORMAT_DOUBLE: snprintf(part, sizeof part, "double(%f)", entry.real); break;
            case MINATO_MEDIA_FORMAT_RECT:
                snprintf(part, sizeof part, "rect(%d, %d, %d, %d)", entry.rect[0], entry.rect[1], entry.rect[2],
                         entry.rect[3]);
                break;
            case MINATO_MEDIA_FORMAT_BUFFER: snprintf(part, sizeof part, "data(%zu)", entry.size); break;
            default: part[0] = 0; break;
        }
        if (entry.kind == MINATO_MEDIA_FORMAT_STRING) {
            ok = ok && append(&text, &size, "string(") && append(&text, &size, entry.text) &&
                 append(&text, &size, ")");
        } else {
            ok = ok && append(&text, &size, part);
        }
    }
    if (!ok) {
        free(text);
        return nullptr;
    }
    free(format->description);
    format->description = text;
    return text;
}

bool AMediaFormat_getInt32(AMediaFormat *format, const char *name, int32_t *out) {
    const Entry *entry = find(format, name, MINATO_MEDIA_FORMAT_INT32);
    if (entry == nullptr || out == nullptr) return false;
    *out = static_cast<int32_t>(entry->integer);
    return true;
}

bool AMediaFormat_getInt64(AMediaFormat *format, const char *name, int64_t *out) {
    const Entry *entry = find(format, name, MINATO_MEDIA_FORMAT_INT64);
    if (entry == nullptr || out == nullptr) return false;
    *out = entry->integer;
    return true;
}

bool AMediaFormat_getFloat(AMediaFormat *format, const char *name, float *out) {
    const Entry *entry = find(format, name, MINATO_MEDIA_FORMAT_FLOAT);
    if (entry == nullptr || out == nullptr) return false;
    *out = static_cast<float>(entry->real);
    return true;
}

bool AMediaFormat_getDouble(AMediaFormat *format, const char *name, double *out) {
    const Entry *entry = find(format, name, MINATO_MEDIA_FORMAT_DOUBLE);
    if (entry == nullptr || out == nullptr) return false;
    *out = entry->real;
    return true;
}

bool AMediaFormat_getSize(AMediaFormat *format, const char *name, size_t *out) {
    const Entry *entry = find(format, name, MINATO_MEDIA_FORMAT_SIZE);
    if (entry == nullptr || out == nullptr) return false;
    *out = static_cast<size_t>(entry->integer);
    return true;
}

bool AMediaFormat_getBuffer(AMediaFormat *format, const char *name, void **data, size_t *size) {
    const Entry *entry = find(format, name, MINATO_MEDIA_FORMAT_BUFFER);
    if (entry == nullptr || data == nullptr || size == nullptr) return false;
    *data = entry->bytes;
    *size = entry->size;
    return true;
}

bool AMediaFormat_getString(AMediaFormat *format, const char *name, const char **out) {
    const Entry *entry = find(format, name, MINATO_MEDIA_FORMAT_STRING);
    if (entry == nullptr || out == nullptr) return false;
    *out = entry->text;
    return true;
}

bool AMediaFormat_getRect(AMediaFormat *format, const char *name, int32_t *left, int32_t *top, int32_t *right,
                          int32_t *bottom) {
    const Entry *entry = find(format, name, MINATO_MEDIA_FORMAT_RECT);
    if (entry == nullptr || left == nullptr || top == nullptr || right == nullptr || bottom == nullptr) return false;
    *left = entry->rect[0], *top = entry->rect[1], *right = entry->rect[2], *bottom = entry->rect[3];
    return true;
}

void AMediaFormat_setInt32(AMediaFormat *format, const char *name, int32_t value) {
    if (Entry *entry = put(format, name, MINATO_MEDIA_FORMAT_INT32)) entry->integer = value;
}

void AMediaFormat_setInt64(AMediaFormat *format, const char *name, int64_t value) {
    if (Entry *entry = put(format, name, MINATO_MEDIA_FORMAT_INT64)) entry->integer = value;
}

void AMediaFormat_setFloat(AMediaFormat *format, const char *name, float value) {
    if (Entry *entry = put(format, name, MINATO_MEDIA_FORMAT_FLOAT)) entry->real = value;
}

void AMediaFormat_setDouble(AMediaFormat *format, const char *name, double value) {
    if (Entry *entry = put(format, name, MINATO_MEDIA_FORMAT_DOUBLE)) entry->real = value;
}

void AMediaFormat_setSize(AMediaFormat *format, const char *name, size_t value) {
    if (Entry *entry = put(format, name, MINATO_MEDIA_FORMAT_SIZE)) entry->integer = static_cast<int64_t>(value);
}

void AMediaFormat_setString(AMediaFormat *format, const char *name, const char *value) {
    if (value == nullptr) return;
    char *copy = strdup(value);
    if (copy == nullptr) return;
    Entry *entry = put(format, name, MINATO_MEDIA_FORMAT_STRING);
    if (entry == nullptr) {
        free(copy);
        return;
    }
    entry->text = copy;
}

void AMediaFormat_setBuffer(AMediaFormat *format, const char *name, const void *data, size_t size) {
    if (data == nullptr && size > 0) return;
    auto *copy = static_cast<uint8_t *>(malloc(size > 0 ? size : 1));
    if (copy == nullptr) return;
    if (size > 0) memcpy(copy, data, size);
    Entry *entry = put(format, name, MINATO_MEDIA_FORMAT_BUFFER);
    if (entry == nullptr) {
        free(copy);
        return;
    }
    entry->bytes = copy;
    entry->size = size;
}

void AMediaFormat_setRect(AMediaFormat *format, const char *name, int32_t left, int32_t top, int32_t right,
                          int32_t bottom) {
    if (Entry *entry = put(format, name, MINATO_MEDIA_FORMAT_RECT)) {
        entry->rect[0] = left, entry->rect[1] = top, entry->rect[2] = right, entry->rect[3] = bottom;
    }
}

void AMediaFormat_clear(AMediaFormat *format) {
    if (format != nullptr) clear_all(format);
}

media_status_t AMediaFormat_copy(AMediaFormat *to, AMediaFormat *from) {
    if (to == nullptr || from == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
    if (to == from) return AMEDIA_OK;
    clear_all(to);
    for (size_t index = 0; index < from->count; index++) {
        const Entry &source = from->entries[index];
        Entry *entry = put(to, source.name, source.kind);
        if (entry == nullptr) return AMEDIA_ERROR_UNKNOWN;
        entry->integer = source.integer, entry->real = source.real;
        memcpy(entry->rect, source.rect, sizeof entry->rect);
        if (source.text != nullptr && (entry->text = strdup(source.text)) == nullptr) return AMEDIA_ERROR_UNKNOWN;
        if (source.bytes != nullptr) {
            entry->bytes = static_cast<uint8_t *>(malloc(source.size > 0 ? source.size : 1));
            if (entry->bytes == nullptr) return AMEDIA_ERROR_UNKNOWN;
            if (source.size > 0) memcpy(entry->bytes, source.bytes, source.size);
            entry->size = source.size;
        }
    }
    return AMEDIA_OK;
}

size_t minato_AMediaFormat_count(AMediaFormat *format) {
    return format != nullptr ? format->count : 0;
}

int minato_AMediaFormat_entry(AMediaFormat *format, size_t index, const char **name, int *kind) {
    if (format == nullptr || index >= format->count || name == nullptr || kind == nullptr) return -1;
    *name = format->entries[index].name;
    *kind = format->entries[index].kind;
    return 0;
}

}  // extern "C"
