// Minato's Ogg Opus reader (ogg_extractor.h): OggExtractor's MyOggExtractor and MyOpusExtractor, step by step.
#include "ogg_extractor.h"

#include <stdlib.h>
#include <string.h>

namespace minato::ogg {
namespace {

constexpr int64_t kRate = 48000;
constexpr int64_t kPreRollUs = 80000;
constexpr size_t kMaxPacket = 16u << 20;
constexpr size_t kFindBuffer = 2048;
constexpr size_t kMaxToc = 8192 / 16;  // OggExtractor's kMaxTOCSize over its TOCEntry's 16 bytes

struct Page {
    uint64_t granule;
    uint32_t serial, number;
    uint8_t flags;
    uint8_t segments;
    uint8_t lace[255];
};

struct TocEntry {
    int64_t offset;
    int64_t time_us;
};

uint32_t le32(const uint8_t *bytes) {
    return static_cast<uint32_t>(bytes[0]) | static_cast<uint32_t>(bytes[1]) << 8 |
           static_cast<uint32_t>(bytes[2]) << 16 | static_cast<uint32_t>(bytes[3]) << 24;
}

uint64_t le64(const uint8_t *bytes) { return le32(bytes) | static_cast<uint64_t>(le32(bytes + 4)) << 32; }

}  // namespace

struct Reader {
    const mp4::Source *source;
    bool sized;
    // MyOggExtractor's reading.
    int64_t offset;           // the current page's
    uint64_t cur_granule;     // the next packet's
    uint64_t prev_granule;    // the page before the current one's
    int64_t page_size;        // the current page's bytes, 0 before it is read
    bool first_in_page;
    int32_t page_samples;
    size_t next_lace;
    Page page;
    int64_t first_data_offset;
    // MyOpusExtractor's.
    uint16_t codec_delay;
    int64_t start_granule;  // -1 until the first data page is read
    // The packet buffer.
    uint8_t *data;
    size_t size, capacity;
    bool has_valid;  // the packet is its page's first: `valid` its page's samples
    int32_t valid;
    uint8_t *head;   // OpusHead's copy
    Format format;
    TocEntry *toc;
    size_t toc_count;
};

namespace {

// readAt of the source, as DataSourceHelper's.
ssize_t read_at(const Reader *reader, int64_t offset, void *data, size_t size) {
    if (offset < 0) return -1;
    return reader->source->read(reader->source->context, static_cast<uint64_t>(offset), data, size);
}

// MyOggExtractor::readPage: the page's bytes, or below 0.
int64_t read_page(const Reader *reader, int64_t offset, Page *page) {
    uint8_t header[27];
    const ssize_t n = read_at(reader, offset, header, sizeof header);
    if (n < static_cast<ssize_t>(sizeof header)) {
        if (n == 0 || n == mp4::kEndOfStream) return mp4::kEndOfStream;
        return n < 0 ? mp4::kUnknown : mp4::kIo;
    }
    if (memcmp(header, "OggS", 4) != 0) return mp4::kMalformed;
    if (header[4] != 0) return mp4::kUnsupported;  // its version
    page->flags = header[5];
    if ((page->flags & ~7) != 0) return mp4::kMalformed;
    page->granule = le64(header + 6);
    page->serial = le32(header + 14);
    page->number = le32(header + 18);
    page->segments = header[26];
    if (read_at(reader, offset + 27, page->lace, page->segments) < static_cast<ssize_t>(page->segments)) {
        return mp4::kIo;
    }
    int64_t total = 0;
    for (size_t i = 0; i < page->segments; i++) total += page->lace[i];
    return 27 + page->segments + total;
}

// MyOggExtractor::findNextPage: the first "OggS" from `start`, by reads of 2048 bytes.
int find_next_page(const Reader *reader, int64_t start, int64_t *page_offset) {
    *page_offset = start;
    char buffer[kFindBuffer];
    for (;;) {
        const ssize_t n = read_at(reader, *page_offset, buffer, sizeof buffer);
        if (n < 4) {
            *page_offset = 0;
            return n < 0 ? static_cast<int>(n) : mp4::kEndOfStream;
        }
        for (ssize_t i = 0; i < n - 3; i++) {
            const auto *found = static_cast<const char *>(memchr(&buffer[i], 'O', static_cast<size_t>(n - 3 - i)));
            if (found == nullptr) break;
            i = found - buffer;
            if (memcmp("OggS", &buffer[i], 4) == 0) {
                *page_offset += i;
                return 0;
            }
        }
        *page_offset += n - 3;
    }
}

// MyOggExtractor::findPrevGranulePosition: the granule position of the page before the one at `page_offset`, found
// by reads of 2048 bytes backwards and then the pages forward from it.
int find_prev_granule(const Reader *reader, int64_t page_offset, uint64_t *granule) {
    *granule = 0;
    if (page_offset == 0) return mp4::kUnknown;
    int64_t prev = page_offset;
    int64_t first_after = page_offset + 3, next_offset = page_offset;
    char buffer[kFindBuffer];
    while (prev == page_offset) {
        ssize_t desired = sizeof buffer;
        if (first_after >= desired) {
            next_offset = first_after - desired;
        } else {
            next_offset = 0;
            desired = static_cast<ssize_t>(first_after);
        }
        const ssize_t n = read_at(reader, next_offset, buffer, static_cast<size_t>(desired));
        if (n < 4) break;
        for (ssize_t i = n - 4; i >= 0; i--) {
            // memrchr over the first i bytes, as OggExtractor's (an 'O' at i itself is not looked at).
            const char *found = nullptr;
            for (ssize_t at = i - 1; at >= 0; at--) {
                if (buffer[at] == 'O') {
                    found = &buffer[at];
                    break;
                }
            }
            if (found == nullptr) break;
            i = found - buffer;
            if (memcmp("OggS", &buffer[i], 4) == 0) {
                prev = next_offset + i;
                break;
            }
        }
        if (next_offset == 0) break;
        first_after = next_offset + 3;
    }
    if (prev == page_offset) return mp4::kUnknown;
    uint8_t flags = 0;
    for (;;) {
        Page page;
        const int64_t n = read_page(reader, prev, &page);
        if (n <= 0) return (flags & 4) != 0 ? 0 : static_cast<int>(n);
        flags = page.flags;
        prev += n;
        *granule = page.granule;
        if (prev == page_offset) return 0;
    }
}

// MyOggExtractor::seekToOffset.
int seek_to_offset(Reader *reader, int64_t offset) {
    if (reader->first_data_offset >= 0 && offset < reader->first_data_offset) offset = reader->first_data_offset;
    int64_t page_offset = 0;
    const int found = find_next_page(reader, offset, &page_offset);
    if (found != 0) return found;
    find_prev_granule(reader, page_offset, &reader->prev_granule);
    reader->offset = page_offset;
    reader->page_size = 0;
    reader->first_in_page = true;
    reader->page_samples = 0;
    reader->page.segments = 0;
    reader->next_lace = 0;
    return 0;
}

bool reserve(Reader *reader, size_t size) {
    if (size <= reader->capacity) return true;
    auto *grown = static_cast<uint8_t *>(realloc(reader->data, size));
    if (grown == nullptr) return false;
    reader->data = grown;
    reader->capacity = size;
    return true;
}

// MyOggExtractor::_readNextPacket without Vorbis's times: the next packet into the reader's buffer, its page's
// samples with it where it is the page's first.
int read_packet(Reader *reader) {
    bool have = false;  // a packet started on the page before
    reader->size = 0;
    reader->has_valid = false;
    for (;;) {
        size_t i, packet_size = 0;
        bool full = false;
        for (i = reader->next_lace; i < reader->page.segments; i++) {
            const uint8_t lace = reader->page.lace[i];
            packet_size += lace;
            if (lace < 255) {
                full = true;
                i++;
                break;
            }
        }
        if (reader->next_lace < reader->page.segments) {
            int64_t data_offset = reader->offset + 27 + reader->page.segments;
            for (size_t j = 0; j < reader->next_lace; j++) data_offset += reader->page.lace[j];
            const size_t full_size = packet_size + (have ? reader->size : 0);
            if (full_size > kMaxPacket) return mp4::kMalformed;  // b/36592202
            if (!reserve(reader, full_size + 1)) return mp4::kNoMemory;
            const size_t kept = have ? reader->size : 0;
            if (!have) reader->has_valid = false;
            const ssize_t n = read_at(reader, data_offset, reader->data + kept, packet_size);
            if (n < static_cast<ssize_t>(packet_size)) return mp4::kIo;
            reader->size = full_size;
            have = true;
            reader->next_lace = i;
            if (full) {
                if (reader->first_in_page) {
                    reader->has_valid = true;
                    reader->valid = reader->page_samples;
                    reader->first_in_page = false;
                }
                return 0;
            }
        }
        reader->offset += reader->page_size;
        const uint8_t flags = reader->page.flags;
        const int64_t n = read_page(reader, reader->offset, &reader->page);
        if (n <= 0) {
            if ((flags & 4) != 0) return mp4::kEndOfStream;
            return static_cast<int>(n);
        }
        reader->page_samples = reader->page.granule >= reader->prev_granule
                                       ? static_cast<int32_t>(reader->page.granule - reader->prev_granule)
                                       : 0;
        reader->first_in_page = true;
        reader->prev_granule = reader->page.granule;
        reader->page_size = n;
        reader->next_lace = 0;
        if (have && (reader->page.flags & 1) == 0) {
            // The page does not continue the packet: it was whole.
            reader->has_valid = true;
            reader->valid = reader->page_samples;
            reader->first_in_page = false;
            return 0;
        }
    }
}

// MyOpusExtractor::getNumSamplesInPacket, a code 3 packet's count from its third byte as Android reads it.
uint32_t packet_samples(const uint8_t *data, size_t size) {
    if (size < 1) return 0;
    static const uint32_t frame_us[32] = {10000, 20000, 40000, 60000, 10000, 20000, 40000, 60000,
                                          10000, 20000, 40000, 60000, 10000, 20000, 10000, 20000,
                                          2500,  5000,  10000, 20000, 2500,  5000,  10000, 20000,
                                          2500,  5000,  10000, 20000, 2500,  5000,  10000, 20000};
    const uint8_t toc = data[0];
    const uint32_t code = toc & 3u;
    const uint32_t frames = code == 0 ? 1 : code < 3 ? 2 : size < 3 ? 0 : data[2] & 0x3fu;
    return static_cast<uint32_t>(static_cast<uint64_t>(frame_us[toc >> 3]) * frames * kRate) / 1000000;
}

// MyOpusExtractor::getTimeUsOfGranule.
int64_t granule_time_us(const Reader *reader, uint64_t granule) {
    const uint64_t pcm = granule > reader->codec_delay ? granule - reader->codec_delay : 0;
    if (pcm > static_cast<uint64_t>(INT64_MAX / 1000000)) return INT64_MAX;
    return static_cast<int64_t>(pcm * 1000000 / kRate);
}

// MyOpusExtractor::verifyOpusComments's checks of OpusTags: its vendor string and comments within the packet.
bool opus_tags(const uint8_t *data, size_t size) {
    const int64_t comment_size = static_cast<int64_t>(size) + 1;  // with OggExtractor's framing bit
    if (size < 8 || memcmp(data, "OpusTags", 8) != 0) return false;
    // oggpack_read past the end gives -1 (all bits set), as Tremor's.
    const auto word = [&](int64_t at) -> int32_t {
        return at + 4 <= static_cast<int64_t>(size) ? static_cast<int32_t>(le32(data + at)) : -1;
    };
    int64_t at = 8, framing = 8;
    const int32_t vendor = word(at);
    at += 4, framing += 4;
    if (vendor < 0 || vendor > comment_size - 8) return false;
    framing += vendor, at += vendor;
    const int32_t count = word(at);
    at += 4, framing += 4;
    if (count < 0 || count > ((comment_size - (at < comment_size ? at : comment_size)) >> 2)) return false;
    for (int32_t i = 0; i < count; i++) {
        const int32_t length = word(at);
        at += 4, framing += 4;
        if (length < 0 || length > comment_size - (at < comment_size ? at : comment_size)) return false;
        framing += length, at += length;
    }
    return framing >= 0 && framing < comment_size;
}

// MyOggExtractor::buildTableOfContents: every page's offset and time, thinned evenly to kMaxToc entries.
void build_toc(Reader *reader) {
    size_t count = 0;
    Page page;
    int64_t size;
    for (int64_t offset = reader->first_data_offset; (size = read_page(reader, offset, &page)) > 0; offset += size) {
        count++;
    }
    const size_t kept = count > kMaxToc ? kMaxToc : count;
    reader->toc = static_cast<TocEntry *>(malloc((kept > 0 ? kept : 1) * sizeof(TocEntry)));
    if (reader->toc == nullptr) return;  // no table: seeks fail
    const size_t denominator = count > kMaxToc ? count - kMaxToc : 0;
    size_t accumulated = 0;
    int64_t offset = reader->first_data_offset;
    for (size_t index = 0; index < count && (size = read_page(reader, offset, &page)) > 0; index++) {
        accumulated += denominator;
        if (denominator > 0 && accumulated >= count) {
            accumulated -= count;
        } else if (reader->toc_count < kept) {
            reader->toc[reader->toc_count++] = TocEntry{offset, granule_time_us(reader, page.granule)};
        }
        offset += size;
    }
}

// MyOggExtractor::init for Opus: its two headers, then the duration and table of contents with the source's size.
int init(Reader *reader) {
    for (int header = 0; header < 2; header++) {
        const int read = read_packet(reader);
        if (read != 0) return read;
        if (header == 0) {
            if (reader->size < 19 || memcmp(reader->data, "OpusHead", 8) != 0 || reader->data[8] > 1) {
                return mp4::kMalformed;
            }
            reader->head = static_cast<uint8_t *>(malloc(reader->size));
            if (reader->head == nullptr) return mp4::kNoMemory;
            memcpy(reader->head, reader->data, reader->size);
            reader->format.head = reader->head;
            reader->format.head_size = reader->size;
            reader->format.channels = reader->data[9];
            reader->codec_delay = static_cast<uint16_t>(reader->data[10] | reader->data[11] << 8);
            reader->format.delay_ns = int64_t{reader->codec_delay} * 1000000000 / kRate;
            reader->format.pre_roll_ns = kPreRollUs * 1000;
        } else if (!opus_tags(reader->data, reader->size)) {
            return mp4::kMalformed;
        }
    }
    reader->first_data_offset = reader->offset + reader->page_size;
    reader->format.duration_us = -1;
    uint64_t last = 0;
    if (reader->sized && find_prev_granule(reader, static_cast<int64_t>(reader->source->length), &last) == 0) {
        reader->format.duration_us = granule_time_us(reader, last);
        build_toc(reader);
    }
    return 0;
}

}  // namespace

int open(const mp4::Source &source, bool sized, Kind *kind, Reader **out) {
    *out = nullptr;
    *kind = Kind::kNotOgg;
    char magic[4];
    if (source.read(source.context, 0, magic, 4) < 4 || memcmp(magic, "OggS", 4) != 0) return 0;
    auto *reader = static_cast<Reader *>(calloc(1, sizeof(Reader)));
    if (reader == nullptr) return mp4::kNoMemory;
    reader->source = &source;
    reader->sized = sized;
    reader->first_data_offset = -1;
    reader->start_granule = -1;
    // OggExtractor tries Vorbis first: a first packet that is Vorbis's identification header.
    if (seek_to_offset(reader, 0) == 0 && read_packet(reader) == 0 && reader->size >= 7 && reader->data[0] == 1 &&
        memcmp(reader->data + 1, "vorbis", 6) == 0) {
        *kind = Kind::kVorbis;
        close(reader);
        return 0;
    }
    reader->size = 0;
    const int answer = seek_to_offset(reader, 0) == 0 ? init(reader) : mp4::kUnknown;
    if (answer == mp4::kNoMemory) {
        close(reader);
        return answer;
    }
    if (answer != 0) {
        *kind = Kind::kNoTrack;
        close(reader);
        return 0;
    }
    *kind = Kind::kOpus;
    *out = reader;
    return 0;
}

void close(Reader *reader) {
    if (reader == nullptr) return;
    free(reader->data);
    free(reader->head);
    free(reader->toc);
    free(reader);
}

const Format &format(const Reader *reader) { return reader->format; }

// MyOpusExtractor::readNextPacket.
int next(Reader *reader, Packet *packet) {
    if (reader->offset <= reader->first_data_offset && reader->start_granule < 0) {
        // The first packet need not start at 0: the first data page's granule position less its packets' samples.
        uint32_t samples = 0;
        uint64_t granule = 0;
        for (;;) {
            const int read = read_packet(reader);
            if (read != 0 && read != mp4::kEndOfStream) return read;
            if (read == mp4::kEndOfStream || reader->page.number > 2) break;
            granule = reader->page.granule;
            samples += packet_samples(reader->data, reader->size);
        }
        reader->start_granule = granule > samples ? static_cast<int64_t>(granule - samples) : 0;
        seek_to_offset(reader, 0);
    }
    const int read = read_packet(reader);
    if (read != 0) return read;
    if (reader->has_valid) {
        int32_t page_samples = reader->valid;
        if (reader->offset == reader->first_data_offset) {
            page_samples = static_cast<int32_t>(page_samples - reader->start_granule);
        }
        __builtin_sub_overflow(reader->page.granule, page_samples, &reader->cur_granule);
    }
    packet->data = reader->data;
    packet->size = reader->size;
    packet->time_us = granule_time_us(reader, reader->cur_granule);
    reader->cur_granule += packet_samples(reader->data, reader->size);
    return 0;
}

// MyOggExtractor::seekToTime.
int seek(Reader *reader, int64_t time_us) {
    time_us -= kPreRollUs;
    if (time_us < 0) time_us = 0;
    if (reader->toc_count == 0) return mp4::kUnknown;  // the approximate seek of a bit rate of 0
    size_t left = 0, right = reader->toc_count;
    while (left < right) {
        const size_t center = left + (right - left) / 2;
        const TocEntry &entry = reader->toc[center];
        if (time_us < entry.time_us) {
            right = center;
        } else if (time_us > entry.time_us) {
            left = center + 1;
        } else {
            left = center;
            break;
        }
    }
    if (left == reader->toc_count) left--;
    return seek_to_offset(reader, reader->toc[left].offset) == 0 ? 0 : mp4::kUnknown;
}

}  // namespace minato::ogg
