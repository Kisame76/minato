// ADR-0060 e2: an Ogg Opus reader of Minato's for libmediandk's AMediaExtractor, in place of Android's OggExtractor
// with its MyOpusExtractor, answering as they do [verified at android-15.0.0_r1: OggExtractor.cpp; the answers
// checked against the emulator by media-extractor-ogg-test]:
// - A source whose first bytes are "OggS" is Ogg. Its first packet must be OpusHead (19 bytes or more, version 0 or
//   1) and its second OpusTags with a comment list that fits it, else it has no track. Ogg Vorbis (an identification
//   header first) is read as MyVorbisExtractor reads it, see below; one whose headers do not pass has no track.
// - The track: audio/opus at 48000 Hz with OpusHead's channels, OpusHead as csd-0, the pre-skip and an 80 ms seek
//   pre-roll in nanoseconds as csd-1 and csd-2; with the source's size, the duration from the last page's granule
//   position and a table of contents of every page (at most 512 entries, thinned evenly as OggExtractor thins it).
//   Without a size there is neither, and a seek fails, as OggExtractor's approximate seek of Opus (bit rate 0) does.
// - Packets in order, page by page by their lacing values. A packet's time is its granule position, less the
//   pre-skip, at 48000 Hz: the first packet of a page starts at the page's granule position less the page's samples
//   (the granule positions' difference to the page before; on the first data page less the samples before the first
//   packet, as OggExtractor finds them), each next one after the frames of the one before by its table of contents.
//   As OggExtractor's getNumSamplesInPacket, a packet of code 3 takes its frame count from its third byte, not its
//   second (RFC 6716 puts it in the second), and none below three bytes; the times follow Android's.
//   A first data page whose granule position is 2^63 or more gives its first packet again and again, as OggExtractor's
//   start position then stays below 0.
// - A seek goes 80 ms before its time, to the first page of the table whose time is not before that, and on from
//   its first packet (a packet that page continues is read from there, as OggExtractor does).
// - Ogg Vorbis (gap 13 of the compatibility plan) [verified at android-15.0.0_r1: OggExtractor.cpp; the answers
//   checked against the emulator by media-extractor-vorbis-test]: three headers (identification, comment, setup)
//   in order; the track is audio/vorbis at the identification header's rate and channels, the identification header as
//   csd-0 and the setup header as csd-1, the nominal bit rate, the duration from the last page's granule position
//   (or, where there is none, from the size and the bit rate) and a table of contents of the pages as for Opus, with
//   no pre-roll. Each packet is followed by 4 bytes, the number of samples its page holds if it is the page's first
//   packet and -1 otherwise, which Android's decoder reads. A packet's time follows OggExtractor's: the page's
//   granule position less its samples for a page's first packet, then half the mean of the block sizes of the packet
//   and the one before for every next one (the block size from the packet's mode, vorbis_codec.h).
// The source is the app's and untrusted: a packet is at most 16 MiB (OggExtractor's limit), a malformed page ends
// the reading with its error. It is part of libmediandk, built as the rest of runtime/android: bionic's C library
// alone, without a C++ library or exceptions, so memory comes from malloc and a failed allocation is kNoMemory.
#ifndef MINATO_OGG_EXTRACTOR_H
#define MINATO_OGG_EXTRACTOR_H

#include <stddef.h>
#include <stdint.h>

#include "mp4_extractor.h"

namespace minato::ogg {

// What a source is.
enum class Kind {
    kNotOgg,   // no "OggS" at its start
    kVorbis,   // Ogg Vorbis: one track
    kNoTrack,  // Ogg, but neither Vorbis nor Opus as OggExtractor reads them: no track
    kOpus,     // Ogg Opus: one track
};

// The track's format, as MyOpusExtractor's or MyVorbisExtractor's meta has it.
struct Format {
    const uint8_t *head;  // OpusHead or Vorbis's identification header, csd-0
    size_t head_size;
    uint32_t channels;
    int64_t delay_ns, pre_roll_ns;  // Opus: csd-1 and csd-2
    int64_t duration_us;            // -1 without the source's size
    bool vorbis;                    // Ogg Vorbis, whose rate, bit rate and setup header (csd-1) follow
    uint32_t sample_rate;
    int32_t bit_rate;               // the nominal one, 0 where the header has none
    const uint8_t *setup;
    size_t setup_size;
};

struct Packet {
    uint8_t *data;  // the reader's, valid until its next call
    size_t size;
    int64_t time_us;
};

struct Reader;

// Reads the start of `source` (whose size is unknown unless `sized`): its kind, and for kOpus and
// kVorbis the reader in `*out`,
// which `close` ends. Below 0 for kNoMemory.
int open(const mp4::Source &source, bool sized, Kind *kind, Reader **out);
void close(Reader *reader);

const Format &format(const Reader *reader);

// The next packet: 0, or kEndOfStream or a failure (mp4's status values).
int next(Reader *reader, Packet *packet);

// A seek to `time_us`: 0, or kUnknown where it cannot (no table of contents) or fails.
int seek(Reader *reader, int64_t time_us);

}  // namespace minato::ogg

#endif  // MINATO_OGG_EXTRACTOR_H
