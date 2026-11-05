// The written part of MediaProjection (services batch B): the screen capture an app was allowed. Minato offers none, so
// no object of this class is ever made by the framework; DisplayManagerGlobal asks a projection for its binder when a
// virtual display is made from one, and a projection an app made itself has none.
class MediaProjection {
    /** @hide */
    public android.media.projection.IMediaProjection getProjection() {
        return null;
    }
}
