/* What graphics/host's video decoders read from a stream's parameter sets (ADR-0060 e2b3): how many decoded frames
   may come before an earlier one in presentation order, so that frames are handed out in that order as Android's
   decoders hand them out. */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* An H.264 SPS NAL unit (header byte included, no start code): VUI's max_num_reorder_frames, else 0 for Baseline,
   else the level's DPB in frames (A.3.1), at most 16; -1 for one it cannot read. */
int minato_avc_reorder_depth(const uint8_t *sps, size_t size);

/* An HEVC SPS NAL unit (two header bytes included, no start code): sps_max_num_reorder_pics of its highest sub-layer,
   at most 16; -1 for one it cannot read. */
int minato_hevc_reorder_depth(const uint8_t *sps, size_t size);

/* The ID of an H.264 SPS or PPS NAL unit (header byte included): seq_parameter_set_id (0 to 31) or
   pic_parameter_set_id (0 to 255); -1 for another unit or one it cannot read. */
int minato_avc_parameter_set_id(const uint8_t *nal, size_t size);

/* The ID of an HEVC VPS, SPS or PPS NAL unit (two header bytes included): vps_video_parameter_set_id (0 to 15),
   sps_seq_parameter_set_id (0 to 15) or pps_pic_parameter_set_id (0 to 63); -1 for another unit or one it cannot
   read. */
int minato_hevc_parameter_set_id(const uint8_t *nal, size_t size);
