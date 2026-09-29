/* The parity benchmark's scene, its part without an API (scene.h). */
#include "scene.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const double pi = 3.14159265358979323846, two_pi = 6.28318530717958647692;

/* ---- sin and cos of the scene's own: a Taylor series to x^19 on [-pi, pi], off by less than 3e-8 */

static double sine(double x) {
    const double r = x - two_pi * floor(x / two_pi + 0.5), s = r * r;
    return r * (1 + s * (-1.0 / 6 + s * (1.0 / 120 + s * (-1.0 / 5040 + s * (1.0 / 362880 + s * (-1.0 / 39916800 +
               s * (1.0 / 6227020800.0 + s * (-1.0 / 1307674368000.0 + s * (1.0 / 355687428096000.0)))))))));
}

static double cosine(double x) {
    return sine(x + pi / 2);
}

static uint32_t hash(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

/* A hash's top 24 bits as a number in [0, 1). */
static double unit(uint32_t value) {
    return (double)(value >> 8) / 16777216.0;
}

/* ---- bench.conf */

void bench_conf_defaults(struct bench_conf *conf) {
    memset(conf, 0, sizeof *conf);
    strcpy(conf->run, "default");
    conf->rate = 60;
    conf->load = BENCH_LIGHT;
    conf->seconds = 10;
    conf->check = -1;
}

/* A whole number from `text` between `low` and `high`, else -1. */
static long number(const char *text, long low, long high) {
    char *end = NULL;
    const long value = strtol(text, &end, 10);
    return end != text && *end == '\0' && value >= low && value <= high ? value : -1;
}

int bench_conf_parse(const char *text, struct bench_conf *conf, char *error, size_t size) {
    static const char name_characters[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_";
    for (const char *line = text; *line != '\0';) {
        size_t length = strcspn(line, "\n");
        const char *next = line + length + (line[length] == '\n' ? 1 : 0);
        if (length > 0 && line[length - 1] == '\r') --length;
        char entry[128];
        if (length >= sizeof entry) {
            snprintf(error, size, "a line of %zu characters", length);
            return -1;
        }
        memcpy(entry, line, length);
        entry[length] = '\0';
        line = next;
        if (length == 0) continue;
        char *value = strchr(entry, '=');
        if (value == NULL) {
            snprintf(error, size, "not key=value: %s", entry);
            return -1;
        }
        *value++ = '\0';
        const size_t value_length = strlen(value);
        long parsed = 0;
        if (strcmp(entry, "run") == 0 && value_length > 0 && value_length < sizeof conf->run &&
            strspn(value, name_characters) == value_length) {
            memcpy(conf->run, value, value_length + 1);
        } else if (strcmp(entry, "rate") == 0 && (strcmp(value, "60") == 0 || strcmp(value, "120") == 0)) {
            conf->rate = value[0] == '6' ? 60 : 120;
        } else if (strcmp(entry, "load") == 0 && (strcmp(value, "light") == 0 || strcmp(value, "heavy") == 0)) {
            conf->load = value[0] == 'h' ? BENCH_HEAVY : BENCH_LIGHT;
        } else if (strcmp(entry, "seconds") == 0 && (parsed = number(value, 1, 3600)) > 0) {
            conf->seconds = (int)parsed;
        } else if (strcmp(entry, "check") == 0 && (parsed = number(value, 0, 1000000)) >= 0) {
            conf->check = (int)parsed;
        } else {
            snprintf(error, size, "not taken: %s=%s", entry, value);
            return -1;
        }
    }
    return 0;
}

/* ---- The loads */

int bench_meshes(enum bench_api api, enum bench_load load) {
    // Decision 7, calibrated on the iPhone 15 Pro Max (ADR-0037 e): the heavy N gives the native build about
    // 90 FPS at 120 Hz (GLES 89, Vulkan 86 in the warm window), and the light N, a quarter, holds 120 Hz.
    static const int meshes[2][2] = {{2048, 8192}, {2048, 8192}};
    return meshes[api == BENCH_VULKAN][load == BENCH_HEAVY];
}

const char *bench_load_name(enum bench_load load) {
    return load == BENCH_HEAVY ? "heavy" : "light";
}

float bench_time(int frame) {
    return (float)((double)frame / 120.0);
}

/* ---- The sphere and its texture */

void bench_sphere(struct bench_vertex *vertices, uint16_t *indices) {
    const int rings = BENCH_RINGS, segments = BENCH_SEGMENTS;
    for (int ring = 0; ring <= rings; ++ring) {
        const double theta = pi * ring / rings;
        for (int segment = 0; segment <= segments; ++segment) {
            const double phi = two_pi * segment / segments;
            const float point[3] = {(float)(sine(theta) * cosine(phi)), (float)cosine(theta),
                                    (float)(sine(theta) * sine(phi))};
            struct bench_vertex *vertex = &vertices[ring * (segments + 1) + segment];
            memcpy(vertex->position, point, sizeof point);
            memcpy(vertex->normal, point, sizeof point);
            vertex->uv[0] = (float)((double)segment / segments);
            vertex->uv[1] = (float)((double)ring / rings);
        }
    }
    for (int ring = 0; ring < rings; ++ring) {
        for (int segment = 0; segment < segments; ++segment) {
            const int a = ring * (segments + 1) + segment, b = a + segments + 1;
            const uint16_t quad[6] = {(uint16_t)a, (uint16_t)(a + 1), (uint16_t)b,
                                      (uint16_t)(a + 1), (uint16_t)(b + 1), (uint16_t)b};
            memcpy(indices + 6 * (ring * segments + segment), quad, sizeof quad);
        }
    }
}

size_t bench_texture_bytes(void) {
    size_t bytes = 0;
    for (unsigned level = 0; level < BENCH_TEXTURE_LEVELS; ++level) {
        const size_t side = (size_t)BENCH_TEXTURE_SIZE >> level;
        bytes += 4 * side * side;
    }
    return bytes;
}

void bench_texture(uint8_t *pixels) {
    const uint32_t size = BENCH_TEXTURE_SIZE;
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            uint8_t *texel = pixels + 4 * (y * size + x);
            const uint32_t noise = hash(y * size + x) & 31, checker = ((x >> 5) ^ (y >> 5)) & 1;
            const uint32_t stripe = ((x + y) >> 3) & 1;
            texel[0] = (uint8_t)(checker ? 235 - noise : 70 + noise);
            texel[1] = (uint8_t)(checker ? 210 - noise : 110 + 40 * stripe + noise);
            texel[2] = (uint8_t)(checker ? 190 - noise / 2 : 150 - noise);
            texel[3] = 255;
        }
    }
    // Each level the rounded mean of four texels of the one above.
    size_t above = 0;
    for (uint32_t side = size / 2, above_side = size; side >= 1; above_side = side, side /= 2) {
        const size_t level = above + 4 * (size_t)above_side * above_side, row = 4 * (size_t)above_side;
        for (uint32_t y = 0; y < side; ++y) {
            for (uint32_t x = 0; x < side; ++x) {
                for (uint32_t channel = 0; channel < 4; ++channel) {
                    const size_t top = above + 2 * y * row + 8 * x + channel;
                    const uint32_t sum =
                        (uint32_t)pixels[top] + pixels[top + 4] + pixels[top + row] + pixels[top + row + 4];
                    pixels[level + 4 * ((size_t)y * side + x) + channel] = (uint8_t)((sum + 2) / 4);
                }
            }
        }
        above = level;
    }
}

/* ---- The meshes' motion, from the frame index alone */

void bench_mesh(int index, int frame, float aspect, struct bench_mesh *mesh) {
    const uint32_t id = 8 * (uint32_t)index;
    const double t = (double)frame / 120.0;
    // A place in the view between 6 and 30 units away, an orbit around it, a spin and a size.
    const double depth = 6 + 24 * unit(hash(id));
    const double half_height = depth * 0.5773502691896258 * 0.92, half_width = half_height * aspect;
    const double u = 2 * unit(hash(id + 1)) - 1, v = 2 * unit(hash(id + 2)) - 1;
    const double orbit = 0.2 + 0.6 * unit(hash(id + 3)), speed = 0.4 + 1.2 * unit(hash(id + 4));
    const double phase = two_pi * unit(hash(id + 5)), spin = 0.5 + 2 * unit(hash(id + 6));
    const double scale = 0.25 + 0.35 * unit(hash(id + 7));
    const double angle = phase + speed * t;
    const double position[3] = {u * half_width + orbit * cosine(angle), v * half_height + orbit * sine(angle), -depth};
    const double yaw = phase + spin * t, pitch = 0.7 * spin * t;
    const double cy = cosine(yaw), sy = sine(yaw), cp = cosine(pitch), sp = sine(pitch);
    // The rotation Ry(yaw) Rx(pitch) by columns.
    const double rotation[3][3] = {{cy, 0, -sy}, {sy * sp, cp, cy * sp}, {sy * cp, -sp, cy * cp}};
    // The projection: 60 degrees high, from 1 to 60 units, as GL's glFrustum gives it.
    const double near = 1, far = 60, focal = 1.7320508075688772;
    const double a = (far + near) / (near - far), b = 2 * far * near / (near - far);
    for (int column = 0; column < 4; ++column) {
        const double model[4] = {column < 3 ? rotation[column][0] * scale : position[0],
                                 column < 3 ? rotation[column][1] * scale : position[1],
                                 column < 3 ? rotation[column][2] * scale : position[2], column < 3 ? 0 : 1};
        mesh->mvp[4 * column + 0] = (float)(focal / aspect * model[0]);
        mesh->mvp[4 * column + 1] = (float)(focal * model[1]);
        mesh->mvp[4 * column + 2] = (float)(a * model[2] + b * model[3]);
        mesh->mvp[4 * column + 3] = (float)-model[2];
    }
    for (int column = 0; column < 3; ++column) {
        for (int row = 0; row < 3; ++row) mesh->normal[4 * column + row] = (float)rotation[column][row];
        mesh->normal[4 * column + 3] = 0;
    }
    const uint32_t tint = hash(id + 0x51ed270bU);
    for (int channel = 0; channel < 3; ++channel) {
        mesh->color[channel] = (float)(0.35 + 0.65 * (double)((tint >> (8 * channel)) & 255) / 255);
    }
    mesh->color[3] = 1;
}

/* ---- The frames' records */

int bench_frames_reserve(struct bench_frames *frames, int capacity) {
    if (capacity <= frames->capacity) return 0;
    struct bench_frame *rows = realloc(frames->rows, (size_t)capacity * sizeof *rows);
    if (rows == NULL) return -1;
    frames->rows = rows;
    frames->capacity = capacity;
    return 0;
}

int bench_frames_add(struct bench_frames *frames, int64_t tick, int64_t cpu) {
    if (frames->count == frames->capacity &&
        bench_frames_reserve(frames, frames->capacity > 0 ? 2 * frames->capacity : 1024) != 0) {
        return -1;
    }
    frames->rows[frames->count++] = (struct bench_frame){tick, cpu};
    return 0;
}

int bench_frames_write(const struct bench_frames *frames, const char *path) {
    FILE *file = fopen(path, "w");
    if (file == NULL) return -1;
    int result = fputs("index,tick_ns,cpu_ns\n", file) < 0 ? -1 : 0;
    for (int index = 0; index < frames->count && result == 0; ++index) {
        if (fprintf(file, "%d,%lld,%lld\n", index, (long long)frames->rows[index].tick,
                    (long long)frames->rows[index].cpu) < 0) {
            result = -1;
        }
    }
    return fclose(file) == 0 ? result : -1;
}

void bench_frames_free(struct bench_frames *frames) {
    free(frames->rows);
    memset(frames, 0, sizeof *frames);
}

int64_t bench_cpu_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
    return (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}

uint32_t bench_crc32(uint32_t crc, const uint8_t *bytes, size_t size) {
    static uint32_t table[256];
    if (table[1] == 0) {
        for (uint32_t entry = 0; entry < 256; ++entry) {
            uint32_t value = entry;
            for (int bit = 0; bit < 8; ++bit) value = value & 1 ? 0xedb88320U ^ (value >> 1) : value >> 1;
            table[entry] = value;
        }
    }
    crc = ~crc;
    for (size_t index = 0; index < size; ++index) crc = table[(crc ^ bytes[index]) & 255] ^ (crc >> 8);
    return ~crc;
}
