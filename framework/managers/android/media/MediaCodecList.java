// The written part of MediaCodecList (the Unity test APK gap sort, Formate's category 2, android.media as absent hardware),
// which tools/managergen joins with the API's methods. Minato has no codecs to list: the list can be made and counts
// none. getCodecInfos and getCodecInfoAt wait for MediaCodecInfo, which comes with the codec classes (the sort's
// category 3m): its class cannot be loaded here yet, so they log once and answer null.
class MediaCodecList {
    public MediaCodecList(int kind) {
    }

    public static int getCodecCount() {
        return 0;
    }
}
