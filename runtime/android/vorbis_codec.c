// Ours: vorbis_codec.h over stb_vorbis's push-data mode.
#include "vorbis_codec.h"

#include <stdlib.h>
#include <string.h>

// Third-party C inside this file: no warnings for it, and only the push-data API, from memory, with no stdio.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Weverything"
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PULLDATA_API
#define STB_VORBIS_NO_INTEGER_CONVERSION
#include "stb_vorbis.c"
#pragma GCC diagnostic pop

struct apk_vorbis {
    stb_vorbis *decoder;
    uint8_t *headers;  // the headers' pages
    size_t headers_size;
    uint8_t *page;     // the page of the packet being decoded
    size_t page_capacity;
    int channels, rate;
};

enum { kMaxLacing = 255, kMaxPacket = 255 * 255 - 1 };

// Appends `packet` as Ogg pages to `out` (whose room is the caller's): segments of 255 bytes and a last one below
// 255, a page of at most 255 segments, a continuation flag on the pages after the first. Returns the new end.
static uint8_t *append_pages(uint8_t *out, const uint8_t *packet, size_t size, uint32_t serial, uint32_t *sequence,
                             int begin_of_stream) {
    size_t done = 0;
    int first = 1;
    for (;;) {
        size_t segments = 0, bytes = 0;
        uint8_t lacing[kMaxLacing];
        while (segments < kMaxLacing) {
            const size_t left = size - done - bytes;
            const uint8_t lace = left >= 255 ? 255 : (uint8_t)left;
            lacing[segments++] = lace;
            bytes += lace;
            if (lace < 255) break;
        }
        const int last = lacing[segments - 1] < 255;
        memcpy(out, "OggS", 4);
        out[4] = 0;
        out[5] = (uint8_t)((first ? 0 : 1) | (begin_of_stream && first ? 2 : 0));
        memset(out + 6, 0xff, 8);  // no granule position
        out[14] = (uint8_t)serial, out[15] = (uint8_t)(serial >> 8), out[16] = (uint8_t)(serial >> 16);
        out[17] = (uint8_t)(serial >> 24);
        out[18] = (uint8_t)*sequence, out[19] = (uint8_t)(*sequence >> 8), out[20] = (uint8_t)(*sequence >> 16);
        out[21] = (uint8_t)(*sequence >> 24);
        (*sequence)++;
        memset(out + 22, 0, 4);  // no checksum
        out[26] = (uint8_t)segments;
        memcpy(out + 27, lacing, segments);
        memcpy(out + 27 + segments, packet + done, bytes);
        out += 27 + segments + bytes;
        done += bytes;
        first = 0;
        if (last) return out;
    }
}

// The bytes of `size` packet bytes as pages, at most.
static size_t pages_size(size_t size) {
    const size_t segments = size / 255 + 1;
    const size_t pages = (segments + kMaxLacing - 1) / kMaxLacing;
    return size + segments + pages * 27;
}

static int open_decoder(apk_vorbis *vorbis) {
    int used = 0, error = 0;
    vorbis->decoder = stb_vorbis_open_pushdata(vorbis->headers, (int)vorbis->headers_size, &used, &error, NULL);
    if (vorbis->decoder == NULL) return 0;
    vorbis->channels = vorbis->decoder->channels;
    vorbis->rate = (int)vorbis->decoder->sample_rate;
    return 1;
}

apk_vorbis *apk_vorbis_open(const uint8_t *ident, size_t ident_size, const uint8_t *setup, size_t setup_size) {
    static const uint8_t comment[] = {3, 'v', 'o', 'r', 'b', 'i', 's', 0, 0, 0, 0, 0, 0, 0, 0, 1};
    if (ident == NULL || setup == NULL || ident_size > kMaxPacket || setup_size > 4 * (size_t)kMaxPacket) return NULL;
    apk_vorbis *vorbis = (apk_vorbis *)calloc(1, sizeof *vorbis);
    if (vorbis == NULL) return NULL;
    const size_t total = pages_size(ident_size) + pages_size(sizeof comment) + pages_size(setup_size);
    vorbis->headers = (uint8_t *)malloc(total);
    if (vorbis->headers == NULL) {
        free(vorbis);
        return NULL;
    }
    uint32_t sequence = 0;
    uint8_t *end = append_pages(vorbis->headers, ident, ident_size, 1, &sequence, 1);
    end = append_pages(end, comment, sizeof comment, 1, &sequence, 0);
    end = append_pages(end, setup, setup_size, 1, &sequence, 0);
    vorbis->headers_size = (size_t)(end - vorbis->headers);
    if (!open_decoder(vorbis)) {
        free(vorbis->headers);
        free(vorbis);
        return NULL;
    }
    return vorbis;
}

void apk_vorbis_close(apk_vorbis *vorbis) {
    if (vorbis == NULL) return;
    if (vorbis->decoder != NULL) stb_vorbis_close(vorbis->decoder);
    free(vorbis->headers);
    free(vorbis->page);
    free(vorbis);
}

int apk_vorbis_channels(const apk_vorbis *vorbis) { return vorbis->channels; }

int apk_vorbis_rate(const apk_vorbis *vorbis) { return vorbis->rate; }

int apk_vorbis_block_size(const apk_vorbis *vorbis, const uint8_t *packet, size_t size) {
    const stb_vorbis *f = vorbis->decoder;
    if (size < 1 || (packet[0] & 1) != 0) return -1;  // a header packet has its low bit set
    // The mode number follows the packet type bit: ilog(mode_count - 1) bits, least significant bit first.
    int bits = 0;
    for (int n = f->mode_count - 1; n > 0; n >>= 1) bits++;
    const unsigned mode = (packet[0] >> 1) & ((1u << bits) - 1);
    if (bits > 7 || (int)mode >= f->mode_count) return -1;
    return f->mode_config[mode].blockflag ? f->blocksize_1 : f->blocksize_0;
}

int apk_vorbis_decode(apk_vorbis *vorbis, const uint8_t *packet, size_t size, int16_t *out, size_t capacity) {
    if (size > kMaxPacket) return -2;
    const size_t room = pages_size(size);
    if (room > vorbis->page_capacity) {
        uint8_t *grown = (uint8_t *)realloc(vorbis->page, room);
        if (grown == NULL) return -1;
        vorbis->page = grown;
        vorbis->page_capacity = room;
    }
    uint8_t *page = vorbis->page;
    uint32_t sequence = 4;
    const uint8_t *end = append_pages(page, packet, size, 1, &sequence, 0);
    int channels = 0, samples = 0;
    float **output = NULL;
    const int used = stb_vorbis_decode_frame_pushdata(vorbis->decoder, page, (int)(end - page), &channels, &output,
                                                      &samples);
    if (used == 0 && samples == 0) return -1;
    if ((size_t)samples > capacity) return -3;
    for (int frame = 0; frame < samples; frame++) {
        for (int channel = 0; channel < channels; channel++) {
            float value = output[channel][frame] * 32768.0f;
            value = value > 32767.0f ? 32767.0f : value < -32768.0f ? -32768.0f : value;
            out[frame * channels + channel] = (int16_t)(value < 0 ? value - 0.5f : value + 0.5f);
        }
    }
    return samples;
}

// stb_vorbis' own flush leaves the decoder without output (its first-frame state is not set again), so the decoder is
// opened anew over the same headers; one it cannot open again (it opened before) stays as it was.
void apk_vorbis_reset(apk_vorbis *vorbis) {
    stb_vorbis *old = vorbis->decoder;
    if (!open_decoder(vorbis)) {
        vorbis->decoder = old;
        return;
    }
    stb_vorbis_close(old);
}
