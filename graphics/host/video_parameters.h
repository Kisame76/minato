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

/* The active parameter sets (7.4.1.2.1, 7.4.2.4.2): the PPS an H.264 slice (NAL types 1 to 5) names,
   pic_parameter_set_id (0 to 255), and the SPS an H.264 PPS names, seq_parameter_set_id (0 to 31); -1 for another unit
   or one it cannot read. Only a slice's first bytes are read, so a slice of any size is fine. */
int minato_avc_slice_pps_id(const uint8_t *nal, size_t size);
int minato_avc_pps_sps_id(const uint8_t *nal, size_t size);

/* HEVC's: the PPS a slice segment (NAL types 0 to 9 and 16 to 21) names, slice_pic_parameter_set_id (0 to 63), and the
   SPS a PPS names, pps_seq_parameter_set_id (0 to 15); -1 for another unit or one it cannot read. */
int minato_hevc_slice_pps_id(const uint8_t *nal, size_t size);
int minato_hevc_pps_sps_id(const uint8_t *nal, size_t size);
