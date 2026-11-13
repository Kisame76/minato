// The written part of WallpaperManager, which tools/managergen joins with
// the API's methods. Ours under AOSP's name: AOSP's asks the wallpaper service of system_server and keeps a bitmap
// decoder and the wallpaper engine's callbacks, none of which Minato has, and no launcher or lock screen draws a
// wallpaper on Minato. The answers are those of apk-runner-35 (Android 15) for an app without wallpaper permissions:
// wallpapers are supported and may be set, the wallpaper is an image wallpaper (no WallpaperInfo), the app's own colors
// come from the built-in image, and reading the current wallpaper or changing it throws SecurityException. Named limit:
// nothing ever shows a wallpaper, so an app that holds MANAGE_EXTERNAL_STORAGE reads the built-in image as the current
// one, and a permitted set call is accepted and ignored; live wallpapers (WallpaperService) have no engine.

import android.content.Context;
import android.content.res.Resources;
import android.graphics.drawable.Drawable;
import android.os.ParcelFileDescriptor;

class WallpaperManager {
    private Context mContext;

    /** @hide */
    public WallpaperManager(Context context) {
        mContext = context;
    }

    /** The app's wallpaper manager, as AOSP's getInstance gives it: the context's service. */
    public static WallpaperManager getInstance(Context context) {
        return (WallpaperManager) context.getSystemService(Context.WALLPAPER_SERVICE);
    }

    public boolean isWallpaperSupported() {
        return true;
    }

    public boolean isSetWallpaperAllowed() {
        return true;
    }

    public WallpaperInfo getWallpaperInfo() {
        return null;
    }

    public WallpaperInfo getWallpaperInfo(int which) {
        return null;
    }

    public int getDesiredMinimumWidth() {
        return mContext.getResources().getDisplayMetrics().widthPixels;
    }

    public int getDesiredMinimumHeight() {
        return mContext.getResources().getDisplayMetrics().heightPixels;
    }

    /** The system wallpaper has an ID; no lock-screen wallpaper of its own is set (apk-runner-35 gives -1). */
    public int getWallpaperId(int which) {
        return which == FLAG_LOCK ? -1 : 1;
    }

    /** The built-in image, framework-res's default wallpaper. */
    public Drawable getBuiltInDrawable() {
        return Resources.getSystem().getDrawable(com.android.internal.R.drawable.default_wallpaper, null);
    }

    public Drawable getBuiltInDrawable(int which) {
        return getBuiltInDrawable();
    }

    public WallpaperColors getWallpaperColors(int which) {
        return WallpaperColors.fromDrawable(getBuiltInDrawable());
    }

    /** Whether the app may read the current wallpaper: MANAGE_EXTERNAL_STORAGE or READ_WALLPAPER_INTERNAL. */
    private void requireReadPermission() {
        final int granted = android.content.pm.PackageManager.PERMISSION_GRANTED;
        if (mContext.checkSelfPermission("android.permission.MANAGE_EXTERNAL_STORAGE") != granted
                && mContext.checkSelfPermission("android.permission.READ_WALLPAPER_INTERNAL") != granted) {
            throw new SecurityException("Permission denial: reading the wallpaper requires MANAGE_EXTERNAL_STORAGE or "
                    + "READ_WALLPAPER_INTERNAL");
        }
    }

    private void requireSetPermission() {
        if (mContext.checkSelfPermission("android.permission.SET_WALLPAPER")
                != android.content.pm.PackageManager.PERMISSION_GRANTED) {
            throw new SecurityException("Permission denial: setting the wallpaper requires SET_WALLPAPER");
        }
    }

    public Drawable getDrawable() {
        requireReadPermission();
        return getBuiltInDrawable();
    }

    public Drawable peekDrawable() {
        requireReadPermission();
        return getBuiltInDrawable();
    }

    public ParcelFileDescriptor getWallpaperFile(int which) {
        requireReadPermission();
        return null;
    }

    public void setBitmap(android.graphics.Bitmap bitmap) throws java.io.IOException {
        requireSetPermission();
    }

    public void clear() throws java.io.IOException {
        requireSetPermission();
    }
}
