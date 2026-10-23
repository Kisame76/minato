// libOpenMAXAL.so (the compatibility plan's gap 14): OpenMAX AL has no engine in Minato, so that an app that links the
// library loads, as it does on a phone, and finds the engine unsupported. xaCreateEngine answers
// XA_RESULT_FEATURE_UNSUPPORTED and leaves no object behind, the two queries list no interface, and the 41 interface
// IDs are exported as the header declares them.
//
// The IDs are unique to this library but are not Khronos's UUIDs, which Android 15 holds in libwilhelm.so (as it does
// OpenSL ES's, see opensles.cpp): no engine ever hands out an interface for an app to ask for by ID, so only the
// pointers are compared. A named limit of the step.
#include <OMXAL/OpenMAXAL.h>
#include <stddef.h>
#include <stdint.h>

#define MINATO_XA_IIDS(X) \
    X(ANDROIDBUFFERQUEUESOURCE) \
    X(AUDIODECODERCAPABILITIES) \
    X(AUDIOENCODER) \
    X(AUDIOENCODERCAPABILITIES) \
    X(AUDIOIODEVICECAPABILITIES) \
    X(CAMERA) \
    X(CAMERACAPABILITIES) \
    X(CONFIGEXTENSION) \
    X(DEVICEVOLUME) \
    X(DYNAMICINTERFACEMANAGEMENT) \
    X(DYNAMICSOURCE) \
    X(ENGINE) \
    X(EQUALIZER) \
    X(IMAGECONTROLS) \
    X(IMAGEDECODERCAPABILITIES) \
    X(IMAGEEFFECTS) \
    X(IMAGEENCODER) \
    X(IMAGEENCODERCAPABILITIES) \
    X(LED) \
    X(METADATAEXTRACTION) \
    X(METADATAINSERTION) \
    X(METADATATRAVERSAL) \
    X(NULL) \
    X(OBJECT) \
    X(OUTPUTMIX) \
    X(PLAY) \
    X(PLAYBACKRATE) \
    X(PREFETCHSTATUS) \
    X(RADIO) \
    X(RDS) \
    X(RECORD) \
    X(SEEK) \
    X(SNAPSHOT) \
    X(STREAMINFORMATION) \
    X(THREADSYNC) \
    X(VIBRA) \
    X(VIDEODECODERCAPABILITIES) \
    X(VIDEOENCODER) \
    X(VIDEOENCODERCAPABILITIES) \
    X(VIDEOPOSTPROCESSING) \
    X(VOLUME)

#define MINATO_INDEX(name) k_##name,
enum { MINATO_XA_IIDS(MINATO_INDEX) kInterfaces };

// time_low spells "MNTO" and node[5] counts, which keeps every ID distinct from Khronos's and from the others.
#define MINATO_VALUE(name) {0x4d4e544f, 0, 0, 0, {0, 0, 0, 0, 0, k_##name}},
static const struct XAInterfaceID_ kIids[kInterfaces] = {MINATO_XA_IIDS(MINATO_VALUE)};

#define MINATO_EXPORT(name) \
    __attribute__((visibility("default"))) const XAInterfaceID XA_IID_##name = &kIids[k_##name];
MINATO_XA_IIDS(MINATO_EXPORT)

__attribute__((visibility("default"))) XAresult XAAPIENTRY xaCreateEngine(
    XAObjectItf *pEngine, XAuint32 numOptions, const XAEngineOption *pEngineOptions, XAuint32 numInterfaces,
    const XAInterfaceID *pInterfaceIds, const XAboolean *pInterfaceRequired) {
  (void)numOptions;
  (void)pEngineOptions;
  (void)numInterfaces;
  (void)pInterfaceIds;
  (void)pInterfaceRequired;
  if (pEngine == NULL) return XA_RESULT_PARAMETER_INVALID;
  *pEngine = NULL;
  return XA_RESULT_FEATURE_UNSUPPORTED;
}

__attribute__((visibility("default"))) XAresult XAAPIENTRY xaQueryNumSupportedEngineInterfaces(
    XAuint32 *pNumSupportedInterfaces) {
  if (pNumSupportedInterfaces == NULL) return XA_RESULT_PARAMETER_INVALID;
  *pNumSupportedInterfaces = 0;
  return XA_RESULT_SUCCESS;
}

__attribute__((visibility("default"))) XAresult XAAPIENTRY xaQuerySupportedEngineInterfaces(
    XAuint32 index, XAInterfaceID *pInterfaceId) {
  (void)index;
  if (pInterfaceId == NULL) return XA_RESULT_PARAMETER_INVALID;
  *pInterfaceId = NULL;
  return XA_RESULT_PARAMETER_INVALID;
}
