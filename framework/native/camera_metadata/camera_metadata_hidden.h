/* Ours: the declarations of libcamera_metadata's vendor ID functions that system/media keeps in its private
   camera/include/camera_metadata_hidden.h, which is not fetched (ADR-0073 step a). camera_metadata.c defines them;
   the signatures are its definitions'. Minato has no vendor tags, so only the plain lookups are called. */
#ifndef MINATO_CAMERA_METADATA_HIDDEN_H
#define MINATO_CAMERA_METADATA_HIDDEN_H

#include <stdint.h>
#include <system/camera_metadata.h>
#include <system/camera_vendor_tags.h>

#ifdef __cplusplus
extern "C" {
#endif

const char *get_local_camera_metadata_section_name_vendor_id(uint32_t tag, metadata_vendor_id_t id);
const char *get_local_camera_metadata_tag_name_vendor_id(uint32_t tag, metadata_vendor_id_t id);
int get_local_camera_metadata_tag_type_vendor_id(uint32_t tag, metadata_vendor_id_t id);
int set_camera_metadata_vendor_cache_ops(const struct vendor_tag_cache_ops *query_cache_ops);
void set_camera_metadata_vendor_id(camera_metadata_t *meta, metadata_vendor_id_t id);
metadata_vendor_id_t get_camera_metadata_vendor_id(const camera_metadata_t *meta);

#ifdef __cplusplus
}
#endif

#endif
