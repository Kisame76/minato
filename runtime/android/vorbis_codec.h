// Gap 13 of the compatibility plan: Ogg Vorbis for libmediandk's extractor and decoder, as packets, over stb_vorbis
// (Sean Barrett, MIT or public domain; third_party/sources.json) in its push-data mode. A Vorbis stream is its three
// headers (identification, comment, setup) and then audio packets; Android's OggExtractor hands the identification
// and the setup header out as csd-0 and csd-1, its decoder (C2SoftVorbisDec, libvorbis) takes them first and then
// one packet per input buffer, and gives each packet's frames after the overlap with the packet before.
// Here the headers and every packet are wrapped in an Ogg page of their own for stb_vorbis, which reads packets only
// from pages; the pages carry no granule position or checksum, which push-data mode does not look at.
#ifndef MINATO_VORBIS_CODEC_H
#define MINATO_VORBIS_CODEC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct apk_vorbis apk_vorbis;

// A decoder over the identification header (its packet, with "\x01vorbis") and the setup header (with "\x05vorbis");
// NULL for a header stb_vorbis refuses and for no memory.
apk_vorbis *apk_vorbis_open(const uint8_t *ident, size_t ident_size, const uint8_t *setup, size_t setup_size);
void apk_vorbis_close(apk_vorbis *vorbis);

int apk_vorbis_channels(const apk_vorbis *vorbis);
int apk_vorbis_rate(const apk_vorbis *vorbis);

// The block size, in samples, of an audio packet by its mode (what libvorbis' vorbis_packet_blocksize gives): below 0
// for a header packet, a packet without a mode or a mode the setup header does not have.
int apk_vorbis_block_size(const apk_vorbis *vorbis, const uint8_t *packet, size_t size);

// An audio packet's frames after its overlap with the one before, as 16-bit samples interleaved into `out`, which
// holds `capacity` frames: the frames (0 for the first packet after a start or a reset), or below 0 for a packet that
// stb_vorbis refuses (-1), one that does not fit a page (-2) and a `capacity` that is too small (-3).
int apk_vorbis_decode(apk_vorbis *vorbis, const uint8_t *packet, size_t size, int16_t *out, size_t capacity);

// The decoder as new for the packets after a seek: the next packet starts a stream again (no overlap).
void apk_vorbis_reset(apk_vorbis *vorbis);

#ifdef __cplusplus
}
#endif

#endif  // MINATO_VORBIS_CODEC_H
