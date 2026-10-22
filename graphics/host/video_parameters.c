/* The reorder depth of an H.264 or HEVC stream from its SPS, and its parameter sets' IDs (ADR-0060 e2b3,
   video_parameters.h). The bytes are the guest's, so every read is bounded: emulation prevention bytes go into a
   copy of at most 4096 bytes, each bit is read inside it, an Exp-Golomb code has at most 31 leading zeros, and each
   count the syntax loops over is checked against the standard's limit before the loop. A parse that fails answers
   -1. */
#include "video_parameters.h"

#include <stdint.h>
#include <string.h>

enum { max_sps = 4096, max_depth = 16 };

struct bits {
    uint8_t bytes[max_sps];
    size_t size, at;  // `at` in bits
    int failed;
};

/* Copies the NAL unit's payload after its header, without emulation prevention bytes (00 00 03). */
static int load(struct bits *bits, const uint8_t *nal, size_t size, size_t header) {
    if (nal == NULL || size <= header || size > max_sps) return 0;
    bits->size = 0, bits->at = 0, bits->failed = 0;
    int zeros = 0;
    for (size_t index = header; index < size; index++) {
        if (zeros >= 2 && nal[index] == 3) {
            zeros = 0;
            continue;
        }
        bits->bytes[bits->size++] = nal[index];
        zeros = nal[index] == 0 ? zeros + 1 : 0;
    }
    return 1;
}

static uint32_t u(struct bits *bits, unsigned count) {
    uint32_t value = 0;
    for (unsigned n = 0; n < count; n++) {
        if (bits->at >= bits->size * 8) {
            bits->failed = 1;
            return 0;
        }
        value = (value << 1) | ((bits->bytes[bits->at / 8] >> (7 - bits->at % 8)) & 1u);
        bits->at++;
    }
    return value;
}

static void skip(struct bits *bits, size_t count) {
    if (count > bits->size * 8 - bits->at) {
        bits->failed = 1;
        bits->at = bits->size * 8;
        return;
    }
    bits->at += count;
}

static uint32_t ue(struct bits *bits) {
    unsigned zeros = 0;
    while (!bits->failed && u(bits, 1) == 0) {
        if (++zeros > 31) {
            bits->failed = 1;
            return 0;
        }
    }
    if (bits->failed) return 0;
    const uint64_t value = ((uint64_t)1 << zeros) - 1 + u(bits, zeros);
    if (value > UINT32_MAX) {
        bits->failed = 1;
        return 0;
    }
    return (uint32_t)value;
}

static int32_t se(struct bits *bits) {
    const uint32_t code = ue(bits);
    return code % 2 != 0 ? (int32_t)((code + 1) / 2) : -(int32_t)(code / 2);
}

/* ue with an upper limit; beyond it the parse fails. */
static uint32_t ue_max(struct bits *bits, uint32_t limit) {
    const uint32_t value = ue(bits);
    if (value > limit) bits->failed = 1;
    return value;
}

/* H.264's scaling_list (7.3.2.1.1.1): only read past. */
static void avc_scaling_list(struct bits *bits, int size) {
    int32_t last = 8, next = 8;
    for (int j = 0; j < size && !bits->failed; j++) {
        if (next != 0) {
            const int32_t delta = se(bits);
            if (delta < -128 || delta > 127) {
                bits->failed = 1;
                return;
            }
            next = (last + delta + 256) % 256;
        }
        last = next == 0 ? last : next;
    }
}

/* H.264's hrd_parameters (E.1.2): only read past. */
static void avc_hrd(struct bits *bits) {
    const uint32_t count = ue_max(bits, 31) + 1;
    u(bits, 8);
    for (uint32_t n = 0; n < count && !bits->failed; n++) {
        ue(bits), ue(bits), u(bits, 1);
    }
    u(bits, 20);
}

/* H.264's MaxDpbMbs by level_idc (Table A-1); 0 for a level it does not have. */
static uint32_t avc_max_dpb_mbs(uint32_t level) {
    switch (level) {
        case 9: case 10: return 396;
        case 11: return 900;
        case 12: case 13: case 20: return 2376;
        case 21: return 4752;
        case 22: case 30: return 8100;
        case 31: return 18000;
        case 32: return 20480;
        case 40: case 41: return 32768;
        case 42: return 34816;
        case 50: return 110400;
        case 51: case 52: return 184320;
        case 60: case 61: case 62: return 696320;
        default: return 0;
    }
}

int minato_avc_reorder_depth(const uint8_t *sps, size_t size) {
    static __thread struct bits bits;
    if (!load(&bits, sps, size, 1) || (sps[0] & 0x1f) != 7) return -1;
    const uint32_t profile = u(&bits, 8);
    u(&bits, 8);  // the constraint flags
    const uint32_t level = u(&bits, 8);
    ue_max(&bits, 31);  // seq_parameter_set_id
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 || profile == 83 ||
        profile == 86 || profile == 118 || profile == 128 || profile == 138 || profile == 139 || profile == 134 ||
        profile == 135) {
        const uint32_t chroma = ue_max(&bits, 3);
        if (chroma == 3) u(&bits, 1);
        ue_max(&bits, 6), ue_max(&bits, 6), u(&bits, 1);
        if (u(&bits, 1)) {  // seq_scaling_matrix_present_flag
            for (int list = 0; list < (chroma != 3 ? 8 : 12) && !bits.failed; list++) {
                if (u(&bits, 1)) avc_scaling_list(&bits, list < 6 ? 16 : 64);
            }
        }
    }
    ue_max(&bits, 12);  // log2_max_frame_num_minus4
    const uint32_t poc_type = ue_max(&bits, 2);
    if (poc_type == 0) {
        ue_max(&bits, 12);
    } else if (poc_type == 1) {
        u(&bits, 1), se(&bits), se(&bits);
        const uint32_t cycle = ue_max(&bits, 255);
        for (uint32_t n = 0; n < cycle && !bits.failed; n++) se(&bits);
    }
    ue_max(&bits, 16), u(&bits, 1);  // max_num_ref_frames, gaps_in_frame_num_value_allowed_flag
    const uint32_t width_mbs = ue_max(&bits, 1023) + 1, height_units = ue_max(&bits, 1023) + 1;
    const uint32_t frame_mbs_only = u(&bits, 1);
    if (!frame_mbs_only) u(&bits, 1);
    u(&bits, 1);
    if (u(&bits, 1)) ue(&bits), ue(&bits), ue(&bits), ue(&bits);  // the cropping
    int depth = -1;
    if (u(&bits, 1)) {  // vui_parameters_present_flag (E.1.1)
        if (u(&bits, 1) && u(&bits, 8) == 255) u(&bits, 32);
        if (u(&bits, 1)) u(&bits, 1);
        if (u(&bits, 1)) {
            u(&bits, 4);
            if (u(&bits, 1)) u(&bits, 24);
        }
        if (u(&bits, 1)) ue(&bits), ue(&bits);
        if (u(&bits, 1)) skip(&bits, 65);
        const uint32_t nal_hrd = u(&bits, 1);
        if (nal_hrd) avc_hrd(&bits);
        const uint32_t vcl_hrd = u(&bits, 1);
        if (vcl_hrd) avc_hrd(&bits);
        if (nal_hrd || vcl_hrd) u(&bits, 1);
        u(&bits, 1);
        if (u(&bits, 1)) {  // bitstream_restriction_flag
            u(&bits, 1), ue(&bits), ue(&bits), ue(&bits), ue(&bits);
            depth = (int)ue_max(&bits, max_depth);  // max_num_reorder_frames
            ue_max(&bits, max_depth);
        }
    }
    if (bits.failed) return -1;
    if (depth >= 0) return depth;
    if (profile == 66) return 0;  // Baseline has no B slices: frames come in decode order
    // Without a restriction, a decoder holds as many frames as the level's DPB has room for (A.3.1 h).
    const uint32_t mbs = avc_max_dpb_mbs(level);
    const uint64_t frame = (uint64_t)width_mbs * height_units * (frame_mbs_only ? 1 : 2);
    if (mbs == 0 || frame == 0) return max_depth;
    const uint64_t frames = mbs / frame;
    return frames > max_depth ? max_depth : (int)frames;
}

/* HEVC's profile_tier_level (7.3.3) with its sub-layers: only read past. */
static void hevc_profile_tier_level(struct bits *bits, uint32_t sub_layers) {
    skip(bits, 96);  // the general profile, tier and level
    uint32_t profile_present[8] = {0}, level_present[8] = {0};
    for (uint32_t n = 0; n < sub_layers; n++) {
        profile_present[n] = u(bits, 1);
        level_present[n] = u(bits, 1);
    }
    if (sub_layers > 0) skip(bits, 2 * (8 - sub_layers));
    for (uint32_t n = 0; n < sub_layers; n++) {
        if (profile_present[n]) skip(bits, 88);
        if (level_present[n]) skip(bits, 8);
    }
}

int minato_hevc_reorder_depth(const uint8_t *sps, size_t size) {
    static __thread struct bits bits;
    if (!load(&bits, sps, size, 2) || ((sps[0] >> 1) & 0x3f) != 33) return -1;
    u(&bits, 4);  // sps_video_parameter_set_id
    const uint32_t sub_layers = u(&bits, 3);  // sps_max_sub_layers_minus1
    if (sub_layers > 6) return -1;
    u(&bits, 1);
    hevc_profile_tier_level(&bits, sub_layers);
    ue_max(&bits, 15);  // sps_seq_parameter_set_id
    if (ue_max(&bits, 3) == 3) u(&bits, 1);  // chroma_format_idc, separate_colour_plane_flag
    ue(&bits), ue(&bits);  // the picture's size
    if (u(&bits, 1)) ue(&bits), ue(&bits), ue(&bits), ue(&bits);  // the conformance window
    ue_max(&bits, 8), ue_max(&bits, 8), ue_max(&bits, 12);  // bit depths, log2_max_pic_order_cnt_lsb_minus4
    const uint32_t all_layers = u(&bits, 1);  // sps_sub_layer_ordering_info_present_flag
    uint32_t depth = 0;
    for (uint32_t n = all_layers ? 0 : sub_layers; n <= sub_layers && !bits.failed; n++) {
        ue_max(&bits, max_depth);
        depth = ue_max(&bits, max_depth);  // sps_max_num_reorder_pics: the highest sub-layer's last
        ue(&bits);
    }
    return bits.failed ? -1 : (int)depth;
}

int minato_avc_parameter_set_id(const uint8_t *nal, size_t size) {
    static __thread struct bits bits;
    if (!load(&bits, nal, size, 1)) return -1;
    uint32_t id;
    if ((nal[0] & 0x1f) == 7) {
        u(&bits, 24);  // profile_idc, the constraint flags, level_idc
        id = ue_max(&bits, 31);
    } else if ((nal[0] & 0x1f) == 8) {
        id = ue_max(&bits, 255);
    } else {
        return -1;
    }
    return bits.failed ? -1 : (int)id;
}

int minato_hevc_parameter_set_id(const uint8_t *nal, size_t size) {
    static __thread struct bits bits;
    if (!load(&bits, nal, size, 2)) return -1;
    const unsigned kind = (nal[0] >> 1) & 0x3f;
    uint32_t id;
    if (kind == 32) {
        id = u(&bits, 4);
    } else if (kind == 33) {
        u(&bits, 4);
        const uint32_t sub_layers = u(&bits, 3);
        if (sub_layers > 6) return -1;
        u(&bits, 1);
        hevc_profile_tier_level(&bits, sub_layers);
        id = ue_max(&bits, 15);
    } else if (kind == 34) {
        id = ue_max(&bits, 63);
    } else {
        return -1;
    }
    return bits.failed ? -1 : (int)id;
}
