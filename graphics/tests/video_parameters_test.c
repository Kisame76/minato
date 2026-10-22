/* graphics.video_parameters (ADR-0060 e2b3): the reorder depth graphics/host's video decoders read from a stream's
   SPS (video_parameters.c), the guest's bytes: Minato's own streams' H.264 SPSs (media-codec-test's, Baseline 0, Main
   at level 1.3 with 20 x 12 macroblocks 2376 / 240 = 9 frames of DPB), one with a VUI's max_num_reorder_frames,
   VideoToolbox's HEVC SPSs without and with reordering, and hostile bytes: every truncation, random bytes, runs of
   zeros and ones, a wrong NAL type, NULL and sizes above the limit, each answering -1 or a depth of 0 to 16 and never
   reading past them; the parameter sets' IDs to the standard's last and no further, and the sets a slice and a PPS
   name, of made units, our Main stream's first slice and their hostile variants. Exits 1 after the cases when one
   failed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "video_parameters.h"

static int failures;

static void check(int ok, const char *what, int got) {
    if (!ok) {
        fprintf(stderr, "FAILED  %s: %d\n", what, got);
        failures++;
    }
}

static const uint8_t avc_baseline[] = {0x27, 0x42, 0x00, 0x0d, 0xab, 0x40, 0xa0, 0xcf, 0xce, 0x80};
static const uint8_t avc_main[] = {0x27, 0x4d, 0x00, 0x0d, 0xab, 0x28, 0x28, 0x33, 0xf3, 0xa0};
static const uint8_t hevc_plain[] = {0x42, 0x01, 0x01, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0xb0, 0x00, 0x00, 0x03,
    0x00, 0x00, 0x03, 0x00, 0x3c, 0xa0, 0x0a, 0x08, 0x0c, 0x1f, 0x3e, 0x20, 0x5e, 0xe4, 0x59, 0x14, 0xbf, 0xf2, 0xe7,
    0xf1, 0x3f, 0xa2};
static const uint8_t hevc_reorder[] = {0x42, 0x01, 0x01, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0xb0, 0x00, 0x00, 0x03,
    0x00, 0x00, 0x03, 0x00, 0x3c, 0xa0, 0x0a, 0x08, 0x0c, 0x1f, 0x3e, 0x20, 0x57, 0xb9, 0x16, 0x54, 0x40};

/* A bit writer for SPSs made here at level 3.1, 40 x 23 macroblocks: `profile` 77 (Main) or 100 (High, with its
   chroma and bit depth fields and a scaling matrix of one list), with a VUI whose bitstream restriction gives
   max_num_reorder_frames `reorder` (or none for a reorder of -1), so that each path is read. */
struct writer {
    uint8_t bytes[64];
    size_t at;  // in bits
};

static void put(struct writer *w, uint32_t value, unsigned count) {
    for (unsigned n = count; n > 0; n--) {
        if ((value >> (n - 1)) & 1u) w->bytes[w->at / 8] |= (uint8_t)(0x80u >> (w->at % 8));
        w->at++;
    }
}

static void put_ue(struct writer *w, uint32_t value) {
    const uint32_t code = value + 1;
    unsigned bits = 0;
    while ((code >> bits) > 1) bits++;
    put(w, 0, bits);
    put(w, code, bits + 1);
}

static size_t made_sps_with_id(uint8_t *out, uint32_t profile, int reorder, uint32_t id, int fields);

static size_t made_sps(uint8_t *out, uint32_t profile, int reorder) {
    return made_sps_with_id(out, profile, reorder, 0, 0);
}

/* With `fields`, frame_mbs_only_flag is 0: the 23 map units are field pairs, 46 macroblock rows a frame. */
static size_t made_sps_with_id(uint8_t *out, uint32_t profile, int reorder, uint32_t id, int fields) {
    struct writer w;
    memset(&w, 0, sizeof w);
    put(&w, 0x27, 8);  // NAL header: an SPS
    put(&w, profile, 8), put(&w, 0, 8), put(&w, 31, 8);  // the profile, no constraints, level 3.1
    put_ue(&w, id);  // seq_parameter_set_id
    if (profile == 100) {
        put_ue(&w, 1), put_ue(&w, 0), put_ue(&w, 0), put(&w, 0, 1);  // 4:2:0, 8 bits, no lossless
        put(&w, 1, 1);  // seq_scaling_matrix_present_flag
        put(&w, 1, 1);  // the first list: one delta, se -8, makes the next scale 0, which ends it
        put_ue(&w, 16);
        for (int list = 1; list < 8; list++) put(&w, 0, 1);  // the other lists absent
    }
    put_ue(&w, 0), put_ue(&w, 0), put_ue(&w, 0);  // log2_max_frame_num_minus4, POC type 0, its lsb
    put_ue(&w, 4), put(&w, 0, 1);  // max_num_ref_frames, gaps
    put_ue(&w, 39), put_ue(&w, 22);  // the size in macroblocks and map units
    if (fields) {
        put(&w, 0, 1), put(&w, 0, 1);  // frame_mbs_only_flag 0, mb_adaptive_frame_field_flag
    } else {
        put(&w, 1, 1);  // frame_mbs_only_flag
    }
    put(&w, 1, 1), put(&w, 0, 1);  // direct_8x8_inference_flag, no cropping
    put(&w, reorder >= 0 ? 1 : 0, 1);  // vui_parameters_present_flag
    if (reorder >= 0) {
        put(&w, 0, 1), put(&w, 0, 1), put(&w, 0, 1), put(&w, 0, 1);  // no aspect, overscan, signal, chroma location
        put(&w, 1, 1), put(&w, 1001, 32), put(&w, 60000, 32), put(&w, 1, 1);  // timing
        put(&w, 0, 1), put(&w, 0, 1), put(&w, 0, 1);  // no HRDs, pic_struct
        put(&w, 1, 1), put(&w, 1, 1);  // bitstream_restriction_flag, motion vectors over picture boundaries
        put_ue(&w, 2), put_ue(&w, 1), put_ue(&w, 16), put_ue(&w, 16);
        put_ue(&w, (uint32_t)reorder), put_ue(&w, 4);  // max_num_reorder_frames, max_dec_frame_buffering
    }
    put(&w, 1, 1);  // the RBSP's stop bit
    const size_t size = (w.at + 7) / 8;
    memcpy(out, w.bytes, size);
    return size;
}

/* An HEVC SPS made here with two sub-layers (the second's level present), so that profile_tier_level's sub-layer
   fields are read past: with every sub-layer's ordering (reorder 1, then 3) or the highest's alone (2). */
static size_t made_hevc_sps_with_id(uint8_t *out, int every_layer, uint32_t id);

static size_t made_hevc_sps(uint8_t *out, int every_layer) {
    return made_hevc_sps_with_id(out, every_layer, 0);
}

static size_t made_hevc_sps_with_id(uint8_t *out, int every_layer, uint32_t id) {
    struct writer w;
    memset(&w, 0, sizeof w);
    put(&w, 0x42, 8), put(&w, 0x01, 8);  // NAL header: an SPS
    put(&w, 0, 4), put(&w, 1, 3), put(&w, 1, 1);  // VPS 0, max_sub_layers_minus1 1, temporal ID nesting
    put(&w, 0x01600000, 32), put(&w, 0, 32), put(&w, 0, 24), put(&w, 93, 8);  // general profile, tier and level 3.1
    put(&w, 0, 1), put(&w, 1, 1);  // the sub-layer: no profile, a level
    put(&w, 0, 14);  // reserved, up to eight sub-layers
    put(&w, 90, 8);  // its level
    put_ue(&w, id), put_ue(&w, 1), put_ue(&w, 320), put_ue(&w, 192), put(&w, 0, 1);  // ID, 4:2:0, size, no window
    put_ue(&w, 0), put_ue(&w, 0), put_ue(&w, 4);  // bit depths, log2_max_pic_order_cnt_lsb_minus4
    put(&w, every_layer ? 1 : 0, 1);  // sps_sub_layer_ordering_info_present_flag
    if (every_layer) put_ue(&w, 4), put_ue(&w, 1), put_ue(&w, 0);
    put_ue(&w, 5), put_ue(&w, every_layer ? 3 : 2), put_ue(&w, 0);
    put(&w, 1, 1);
    const size_t size = (w.at + 7) / 8;
    memcpy(out, w.bytes, size);
    return size;
}

/* A parameter set made here of its header bytes, an ID in `bits` bits (0: Exp-Golomb) and a stop bit. */
static size_t made_set(uint8_t *out, const uint8_t *header, size_t header_size, uint32_t id, unsigned bits) {
    struct writer w;
    memset(&w, 0, sizeof w);
    for (size_t index = 0; index < header_size; index++) put(&w, header[index], 8);
    if (bits == 0) {
        put_ue(&w, id), put_ue(&w, 0);  // the ID, then an SPS's ID or other fields
    } else {
        put(&w, id, bits);
    }
    put(&w, 1, 1);
    const size_t size = (w.at + 7) / 8;
    memcpy(out, w.bytes, size);
    return size;
}

/* The parameter sets' IDs: of SPSs and PPSs made here, and Minato's own streams' (0), and -1 for other units. */
static void parameterSetIdsAreRead(void) {
    uint8_t set[64];
    static const uint8_t avc_pps_header[] = {0x28}, hevc_vps_header[] = {0x40, 0x01}, hevc_pps_header[] = {0x44, 0x01};
    for (uint32_t id = 0; id < 32; id += 7) {
        check(minato_avc_parameter_set_id(set, made_sps_with_id(set, 77, -1, id, 0)) == (int)id, "an H.264 SPS's ID",
              minato_avc_parameter_set_id(set, made_sps_with_id(set, 77, -1, id, 0)));
        check(minato_avc_parameter_set_id(set, made_set(set, avc_pps_header, 1, id * 8, 0)) == (int)(id * 8),
              "an H.264 PPS's ID", minato_avc_parameter_set_id(set, made_set(set, avc_pps_header, 1, id * 8, 0)));
        check(minato_hevc_parameter_set_id(set, made_set(set, hevc_pps_header, 2, id * 2, 0)) == (int)(id * 2),
              "an HEVC PPS's ID", minato_hevc_parameter_set_id(set, made_set(set, hevc_pps_header, 2, id * 2, 0)));
    }
    for (uint32_t id = 0; id < 16; id += 5) {
        check(minato_hevc_parameter_set_id(set, made_set(set, hevc_vps_header, 2, id, 4)) == (int)id,
              "an HEVC VPS's ID", minato_hevc_parameter_set_id(set, made_set(set, hevc_vps_header, 2, id, 4)));
        check(minato_hevc_parameter_set_id(set, made_hevc_sps_with_id(set, 1, id)) == (int)id, "an HEVC SPS's ID",
              minato_hevc_parameter_set_id(set, made_hevc_sps_with_id(set, 1, id)));
    }
    check(minato_avc_parameter_set_id(avc_main, sizeof avc_main) == 0, "Main's SPS", 0);
    check(minato_hevc_parameter_set_id(hevc_reorder, sizeof hevc_reorder) == 0, "VideoToolbox's HEVC SPS", 0);
    static const uint8_t slice[] = {0x25, 0x88, 0x84};
    check(minato_avc_parameter_set_id(slice, sizeof slice) == -1, "an H.264 slice", 0);
    check(minato_avc_parameter_set_id(made_set(set, avc_pps_header, 1, 256, 0) ? set : NULL, 4) == -1,
          "a PPS's ID above 255", 0);
    check(minato_avc_parameter_set_id(set, made_set(set, avc_pps_header, 1, 256, 0)) == -1, "a PPS's ID of 256", 0);
    check(minato_avc_parameter_set_id(NULL, 4) == -1 && minato_hevc_parameter_set_id(NULL, 4) == -1, "NULL", 0);
    // The standard's last IDs, and the next ones refused: no set of a larger ID reaches the decoder's lists.
    check(minato_avc_parameter_set_id(set, made_sps_with_id(set, 77, -1, 31, 0)) == 31, "an H.264 SPS of ID 31", 0);
    check(minato_avc_parameter_set_id(set, made_sps_with_id(set, 77, -1, 32, 0)) == -1, "an H.264 SPS of ID 32", 0);
    check(minato_avc_parameter_set_id(set, made_set(set, avc_pps_header, 1, 255, 0)) == 255, "a PPS of ID 255", 0);
    check(minato_hevc_parameter_set_id(set, made_hevc_sps_with_id(set, 1, 15)) == 15, "an HEVC SPS of ID 15", 0);
    check(minato_hevc_parameter_set_id(set, made_hevc_sps_with_id(set, 1, 16)) == -1, "an HEVC SPS of ID 16", 0);
    check(minato_hevc_parameter_set_id(set, made_set(set, hevc_pps_header, 2, 63, 0)) == 63, "an HEVC PPS of 63", 0);
    check(minato_hevc_parameter_set_id(set, made_set(set, hevc_pps_header, 2, 64, 0)) == -1, "an HEVC PPS of 64", 0);
    for (size_t length = 0; length < sizeof avc_main; length++) {
        const int got = minato_avc_parameter_set_id(avc_main, length);
        check(got == -1 || got == 0, "a cut SPS's ID", got);
    }
}

/* A slice or PPS made here: its header bytes, then Exp-Golomb codes `first` and `second` (a slice's first_mb_in_slice
   and slice_type, or a PPS's own ID and its SPS's) and `third` (a slice's PPS, -1 for none), and payload bytes. With
   `flags` > 0, HEVC's slice: that many flag bits of 1 before the PPS's ID. */
static size_t made_unit(uint8_t *out, const uint8_t *header, size_t header_size, unsigned flags, uint32_t first,
                        uint32_t second, int third) {
    struct writer w;
    memset(&w, 0, sizeof w);
    for (size_t index = 0; index < header_size; index++) put(&w, header[index], 8);
    if (flags > 0) {
        put(&w, (1u << flags) - 1, flags);
        put_ue(&w, first);
    } else {
        put_ue(&w, first), put_ue(&w, second);
        if (third >= 0) put_ue(&w, (uint32_t)third);
    }
    put(&w, 0xa5c3, 16), put(&w, 1, 1);  // some of the slice's data, the stop bit
    const size_t size = (w.at + 7) / 8;
    memcpy(out, w.bytes, size);
    return size;
}

/* Every prefix and every byte change of a unit, in its own allocation: -1 or an answer up to `limit`. */
static void hostile_ids(const uint8_t *unit, size_t size, int (*id)(const uint8_t *, size_t), int limit) {
    for (size_t length = 0; length <= size; length++) {
        uint8_t *copy = malloc(length > 0 ? length : 1);
        memcpy(copy, unit, length);
        const int got = id(length > 0 ? copy : NULL, length);
        check(got >= -1 && got <= limit, "a cut unit's ID", got);
        free(copy);
    }
    uint8_t *copy = malloc(size);
    for (size_t index = 0; index < size; index++) {
        for (int value = 0; value < 256; value += 15) {
            memcpy(copy, unit, size);
            copy[index] = (uint8_t)value;
            const int got = id(copy, size);
            check(got >= -1 && got <= limit, "a changed unit's ID", got);
        }
    }
    free(copy);
}

/* The active SPS: a slice's PPS and a PPS's SPS, H.264's and HEVC's, of units made here and Minato's own Main
   stream's first slice (PPS 0), within the standard's IDs; -1 for other units, larger IDs and cut bytes. */
static void slicesAndPpsNameTheirSets(void) {
    uint8_t unit[64];
    static const uint8_t avc_idr[] = {0x65}, avc_slice[] = {0x41}, avc_pps[] = {0x68}, avc_sps[] = {0x67};
    static const uint8_t hevc_idr[] = {0x26, 0x01}, hevc_trail[] = {0x02, 0x01}, hevc_pps[] = {0x44, 0x01};
    for (uint32_t id = 0; id <= 255; id += 51) {
        check(minato_avc_slice_pps_id(unit, made_unit(unit, avc_idr, 1, 0, 0, 7, (int)id)) == (int)id,
              "an H.264 IDR slice's PPS", minato_avc_slice_pps_id(unit, made_unit(unit, avc_idr, 1, 0, 0, 7, (int)id)));
        check(minato_avc_slice_pps_id(unit, made_unit(unit, avc_slice, 1, 0, 99, 5, (int)id)) == (int)id,
              "an H.264 slice's PPS", minato_avc_slice_pps_id(unit, made_unit(unit, avc_slice, 1, 0, 99, 5, (int)id)));
        check(minato_avc_pps_sps_id(unit, made_unit(unit, avc_pps, 1, 0, id, id % 32, -1)) == (int)(id % 32),
              "an H.264 PPS's SPS", minato_avc_pps_sps_id(unit, made_unit(unit, avc_pps, 1, 0, id, id % 32, -1)));
    }
    for (uint32_t id = 0; id <= 63; id += 21) {
        // An IDR slice has no_output_of_prior_pics_flag after first_slice_segment_in_pic_flag, a trailing one not.
        check(minato_hevc_slice_pps_id(unit, made_unit(unit, hevc_idr, 2, 2, id, 0, -1)) == (int)id,
              "an HEVC IDR slice's PPS", minato_hevc_slice_pps_id(unit, made_unit(unit, hevc_idr, 2, 2, id, 0, -1)));
        check(minato_hevc_slice_pps_id(unit, made_unit(unit, hevc_trail, 2, 1, id, 0, -1)) == (int)id,
              "an HEVC slice's PPS", minato_hevc_slice_pps_id(unit, made_unit(unit, hevc_trail, 2, 1, id, 0, -1)));
        check(minato_hevc_pps_sps_id(unit, made_unit(unit, hevc_pps, 2, 0, id, id % 16, -1)) == (int)(id % 16),
              "an HEVC PPS's SPS", minato_hevc_pps_sps_id(unit, made_unit(unit, hevc_pps, 2, 0, id, id % 16, -1)));
    }
    // Beyond the standard's IDs, and a slice type above 9.
    check(minato_avc_slice_pps_id(unit, made_unit(unit, avc_idr, 1, 0, 0, 7, 256)) == -1, "a slice's PPS 256", 0);
    check(minato_avc_slice_pps_id(unit, made_unit(unit, avc_idr, 1, 0, 0, 10, 0)) == -1, "a slice type 10", 0);
    check(minato_avc_pps_sps_id(unit, made_unit(unit, avc_pps, 1, 0, 256, 0, -1)) == -1, "a PPS of ID 256", 0);
    check(minato_avc_pps_sps_id(unit, made_unit(unit, avc_pps, 1, 0, 0, 32, -1)) == -1, "a PPS's SPS 32", 0);
    check(minato_hevc_slice_pps_id(unit, made_unit(unit, hevc_trail, 2, 1, 64, 0, -1)) == -1, "a slice's PPS 64", 0);
    check(minato_hevc_pps_sps_id(unit, made_unit(unit, hevc_pps, 2, 0, 64, 0, -1)) == -1, "an HEVC PPS of 64", 0);
    check(minato_hevc_pps_sps_id(unit, made_unit(unit, hevc_pps, 2, 0, 0, 16, -1)) == -1, "an HEVC PPS's SPS 16", 0);
    // Other units: an SPS, a PPS as a slice, a slice as a PPS, HEVC's VPS and its parameter sets' types.
    check(minato_avc_slice_pps_id(avc_main, sizeof avc_main) == -1, "an SPS as a slice", 0);
    check(minato_avc_slice_pps_id(unit, made_unit(unit, avc_pps, 1, 0, 0, 0, -1)) == -1, "a PPS as a slice", 0);
    check(minato_avc_pps_sps_id(unit, made_unit(unit, avc_idr, 1, 0, 0, 7, 0)) == -1, "a slice as a PPS", 0);
    check(minato_avc_pps_sps_id(unit, made_unit(unit, avc_sps, 1, 0, 0, 0, -1)) == -1, "an SPS as a PPS", 0);
    check(minato_hevc_slice_pps_id(hevc_reorder, sizeof hevc_reorder) == -1, "an HEVC SPS as a slice", 0);
    check(minato_hevc_pps_sps_id(unit, made_unit(unit, hevc_trail, 2, 1, 0, 0, -1)) == -1, "a slice as a PPS", 0);
    for (unsigned kind = 10; kind < 64; kind++) {  // reserved, IRAP 22 and 23, parameter sets, SEI and the others
        if (kind >= 16 && kind <= 21) continue;
        const uint8_t header[2] = {(uint8_t)(kind << 1), 0x01};
        check(minato_hevc_slice_pps_id(unit, made_unit(unit, header, 2, 2, 0, 0, -1)) == -1, "no HEVC slice",
              (int)kind);
    }
    // The first slice of Minato's Main stream, an IDR slice of PPS 0, cut after its first bytes.
    static const uint8_t main_idr[] = {0x25, 0x88, 0x84, 0x00, 0x33, 0xff};
    check(minato_avc_slice_pps_id(main_idr, sizeof main_idr) == 0, "Main's IDR slice", 0);
    check(minato_avc_slice_pps_id(NULL, 4) == -1 && minato_avc_pps_sps_id(NULL, 4) == -1, "NULL", 0);
    check(minato_hevc_slice_pps_id(NULL, 4) == -1 && minato_hevc_pps_sps_id(NULL, 4) == -1, "NULL", 0);
    // A slice of the largest unit's size: its header alone is read.
    static uint8_t large[1 << 20];
    memset(large, 0x5a, sizeof large);
    memcpy(large, unit, made_unit(unit, avc_idr, 1, 0, 0, 7, 3));
    const int large_pps = minato_avc_slice_pps_id(large, sizeof large);
    check(large_pps == 3, "a slice of 1 MiB", large_pps);
    hostile_ids(unit, made_unit(unit, avc_idr, 1, 0, 0, 7, 200), minato_avc_slice_pps_id, 255);
    hostile_ids(unit, made_unit(unit, avc_pps, 1, 0, 200, 20, -1), minato_avc_pps_sps_id, 31);
    hostile_ids(unit, made_unit(unit, hevc_idr, 2, 2, 50, 0, -1), minato_hevc_slice_pps_id, 63);
    hostile_ids(unit, made_unit(unit, hevc_pps, 2, 0, 50, 12, -1), minato_hevc_pps_sps_id, 15);
    hostile_ids(main_idr, sizeof main_idr, minato_avc_slice_pps_id, 255);
}

/* Every prefix and every single-byte change of the SPS: an answer of -1 to 16, read inside the bytes alone (each
   copy is its own allocation, so AddressSanitizer catches a read past it). With `ends`, the SPS's last field is in its
   last byte, so a prefix without the last two bytes cannot be read: -1, which a read past the prefix would change. */
static void hostile_variants(const uint8_t *sps, size_t size, int (*depth)(const uint8_t *, size_t), int ends) {
    for (size_t length = 0; length <= size; length++) {
        uint8_t *copy = malloc(length > 0 ? length : 1);
        memcpy(copy, sps, length);
        const int got = depth(length > 0 ? copy : NULL, length);
        check(got >= -1 && got <= 16, "a truncated SPS", got);
        if (ends && length + 2 <= size) check(got == -1, "an SPS cut before its last fields", got);
        free(copy);
    }
    uint8_t *copy = malloc(size);
    for (size_t index = 1; index < size; index++) {
        for (int value = 0; value < 256; value += 17) {
            memcpy(copy, sps, size);
            copy[index] = (uint8_t)value;
            const int got = depth(copy, size);
            check(got >= -1 && got <= 16, "a changed SPS", got);
        }
    }
    free(copy);
}

int main(void) {
    check(minato_avc_reorder_depth(avc_baseline, sizeof avc_baseline) == 0, "Baseline",
          minato_avc_reorder_depth(avc_baseline, sizeof avc_baseline));
    check(minato_avc_reorder_depth(avc_main, sizeof avc_main) == 9, "Main at level 1.3",
          minato_avc_reorder_depth(avc_main, sizeof avc_main));
    check(minato_hevc_reorder_depth(hevc_plain, sizeof hevc_plain) == 0, "HEVC without reordering",
          minato_hevc_reorder_depth(hevc_plain, sizeof hevc_plain));
    const int hevc_depth = minato_hevc_reorder_depth(hevc_reorder, sizeof hevc_reorder);
    check(hevc_depth >= 1 && hevc_depth <= 16, "HEVC with reordering", hevc_depth);
    uint8_t made[64];
    for (uint32_t profile = 77; profile <= 100; profile += 23) {
        for (int reorder = 0; reorder <= 3; reorder++) {
            const int got = minato_avc_reorder_depth(made, made_sps(made, profile, reorder));
            check(got == reorder, profile == 77 ? "Main's VUI max_num_reorder_frames" : "High's", got);
        }
        // Without a VUI: level 3.1's 18000 macroblocks of DPB / (40 x 23) = 19, at most 16; with fields,
        // / (40 x 46) = 9.
        const int fielded = minato_avc_reorder_depth(made, made_sps_with_id(made, profile, -1, 0, 1));
        check(fielded == 9, "fields at level 3.1 without a VUI", fielded);
        const int got = minato_avc_reorder_depth(made, made_sps(made, profile, -1));
        check(got == 16, profile == 77 ? "Main at level 3.1 without a VUI" : "High at level 3.1 without one", got);
        hostile_variants(made, made_sps(made, profile, 2), minato_avc_reorder_depth, 1);
    }
    for (int every = 0; every <= 1; every++) {
        const int got = minato_hevc_reorder_depth(made, made_hevc_sps(made, every));
        check(got == (every ? 3 : 2), every ? "HEVC's highest sub-layer of all" : "HEVC's highest sub-layer alone",
              got);
        hostile_variants(made, made_hevc_sps(made, every), minato_hevc_reorder_depth, 1);
    }
    check(minato_avc_reorder_depth(hevc_plain, sizeof hevc_plain) == -1, "an HEVC SPS as H.264's", 0);
    check(minato_hevc_reorder_depth(avc_main, sizeof avc_main) == -1, "an H.264 SPS as HEVC's", 0);
    check(minato_avc_reorder_depth(NULL, 10) == -1 && minato_hevc_reorder_depth(NULL, 10) == -1, "NULL", 0);
    hostile_variants(avc_baseline, sizeof avc_baseline, minato_avc_reorder_depth, 1);
    hostile_variants(avc_main, sizeof avc_main, minato_avc_reorder_depth, 1);
    hostile_variants(hevc_plain, sizeof hevc_plain, minato_hevc_reorder_depth, 0);  // read only to its ordering
    hostile_variants(hevc_reorder, sizeof hevc_reorder, minato_hevc_reorder_depth, 0);
    static uint8_t big[8192];
    for (int pattern = 0; pattern < 3; pattern++) {
        memset(big, pattern == 0 ? 0 : pattern == 1 ? 0xff : 0x80, sizeof big);
        big[0] = 0x27, big[1] = 0x42;  // an H.264 SPS's header, then the pattern
        for (size_t size = 1; size <= sizeof big; size *= 2) {
            const int avc = minato_avc_reorder_depth(big, size);
            check(avc >= -1 && avc <= 16, "a run of one byte as H.264", avc);
            big[0] = 0x42, big[1] = 0x01;
            const int hevc = minato_hevc_reorder_depth(big, size);
            check(hevc >= -1 && hevc <= 16, "a run of one byte as HEVC", hevc);
            big[0] = 0x27, big[1] = 0x42;
        }
    }
    check(minato_avc_reorder_depth(big, 4097) == -1, "an SPS above 4096 bytes", 0);
    uint32_t seed = 7;
    uint8_t noise[256];
    for (int round = 0; round < 20000; round++) {
        for (size_t index = 0; index < sizeof noise; index++) {
            seed = seed * 1103515245u + 12345u;
            noise[index] = (uint8_t)(seed >> 16);
        }
        const size_t size = 1 + seed % sizeof noise;
        noise[0] = round % 2 ? 0x27 : 0x42, noise[1] = round % 2 ? noise[1] : 0x01;
        const int got = round % 2 ? minato_avc_reorder_depth(noise, size) : minato_hevc_reorder_depth(noise, size);
        check(got >= -1 && got <= 16, "random bytes", got);
    }
    parameterSetIdsAreRead();
    slicesAndPpsNameTheirSets();
    printf("graphics.video_parameters: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
