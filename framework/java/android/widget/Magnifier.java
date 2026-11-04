package android.widget;

import android.annotation.FloatRange;
import android.annotation.IntRange;
import android.annotation.NonNull;
import android.annotation.Nullable;
import android.annotation.Px;
import android.graphics.Point;
import android.graphics.drawable.Drawable;
import android.view.View;

/**
 * Android 15's Magnifier without its picture. The magnifier shows a zoomed copy of a view's content in a window of its
 * own, which it draws with hwui's SimpleRenderer from a PixelCopy of the surface; the views here draw in software
 * (decision 2) and have no such renderer, so a magnifier that shows keeps its geometry (size, zoom, position the
 * callers read) and draws nothing: a named departure, felt only while a finger drags a text selection's handle or
 * a cursor. Editor and the apps' own magnifiers get every method and answer of Android's and never an error.
 */
public final class Magnifier {
    /** The source bound extends as far as the surface of the view's window. */
    public static final int SOURCE_BOUND_MAX_IN_SURFACE = 0;
    /** The source bound extends as far as the visible part of the view. */
    public static final int SOURCE_BOUND_MAX_VISIBLE = 1;

    private final View mView;
    private final int mWidth;
    private final int mHeight;
    private final int mSourceWidth;
    private final int mSourceHeight;
    private float mZoom;
    private final float mElevation;
    private final float mCornerRadius;
    private final Drawable mOverlay;
    private final int mDefaultHorizontalSourceToMagnifierOffset;
    private final int mDefaultVerticalSourceToMagnifierOffset;
    private final boolean mClippingEnabled;
    private final Point mPosition = new Point();
    private final Point mSourcePosition = new Point();
    private Callback mCallback;
    private boolean mShowing;

    /** Creates a magnifier with the default parameters of the first Android versions. */
    public Magnifier(@NonNull View view) {
        this(createBuilderWithOldMagnifierDefaults(view));
    }

    static Builder createBuilderWithOldMagnifierDefaults(final View view) {
        final Builder params = new Builder(view);
        final float density = view.getContext().getResources().getDisplayMetrics().density;
        params.mWidth = Math.round(100 * density);
        params.mHeight = Math.round(48 * density);
        params.mInitialZoom = 1.25f;
        params.mElevation = 4 * density;
        params.mCornerRadius = 2 * density;
        params.mHorizontalDefaultSourceToMagnifierOffset = 0;
        params.mVerticalDefaultSourceToMagnifierOffset = Math.round(-42 * density);
        params.mClippingEnabled = true;
        return params;
    }

    private Magnifier(@NonNull Builder params) {
        mView = params.mView;
        mWidth = params.mWidth;
        mHeight = params.mHeight;
        mZoom = params.mInitialZoom;
        mSourceWidth = params.mSourceWidth > 0 ? params.mSourceWidth : Math.round(mWidth / mZoom);
        mSourceHeight = params.mSourceHeight > 0 ? params.mSourceHeight : Math.round(mHeight / mZoom);
        mElevation = params.mElevation;
        mCornerRadius = params.mCornerRadius;
        mOverlay = params.mOverlay;
        mDefaultHorizontalSourceToMagnifierOffset = params.mHorizontalDefaultSourceToMagnifierOffset;
        mDefaultVerticalSourceToMagnifierOffset = params.mVerticalDefaultSourceToMagnifierOffset;
        mClippingEnabled = params.mClippingEnabled;
    }

    /** Shows the magnifier on the screen, centered on the given source point of the view. */
    public void show(@FloatRange(from = 0) float sourceCenterX, @FloatRange(from = 0) float sourceCenterY) {
        show(sourceCenterX, sourceCenterY, sourceCenterX + mDefaultHorizontalSourceToMagnifierOffset,
                sourceCenterY + mDefaultVerticalSourceToMagnifierOffset);
    }

    /** Shows the magnifier on the screen at the given place, magnifying the given source point. */
    public void show(@FloatRange(from = 0) float sourceCenterX, @FloatRange(from = 0) float sourceCenterY,
            float magnifierCenterX, float magnifierCenterY) {
        final int[] origin = new int[2];
        mView.getLocationInWindow(origin);
        mSourcePosition.set(Math.round(origin[0] + sourceCenterX - mSourceWidth / 2f),
                Math.round(origin[1] + sourceCenterY - mSourceHeight / 2f));
        mPosition.set(Math.round(origin[0] + magnifierCenterX - mWidth / 2f),
                Math.round(origin[1] + magnifierCenterY - mHeight / 2f));
        mShowing = true;
        complete();
    }

    /** Dismisses the magnifier from the screen. */
    public void dismiss() {
        mShowing = false;
        complete();
    }

    /** Asks the magnifier to update its content; nothing is drawn. */
    public void update() {
        complete();
    }

    private void complete() {
        if (mCallback != null) {
            mCallback.onOperationComplete();
        }
    }

    /** The width of the magnifier window, in pixels. */
    public int getWidth() {
        return mWidth;
    }

    /** The height of the magnifier window, in pixels. */
    public int getHeight() {
        return mHeight;
    }

    /** The width of the source of the magnifier, in pixels. */
    public int getSourceWidth() {
        return mSourceWidth;
    }

    /** The height of the source of the magnifier, in pixels. */
    public int getSourceHeight() {
        return mSourceHeight;
    }

    /** Sets the zoom of the magnifier. */
    public void setZoom(@FloatRange(from = 0f) float zoom) {
        if (!(zoom > 0f)) {
            throw new IllegalArgumentException("The zoom must be positive.");
        }
        mZoom = zoom;
    }

    /** The zoom of the magnifier. */
    public float getZoom() {
        return mZoom;
    }

    /** The elevation of the magnifier window, in pixels. */
    public float getElevation() {
        return mElevation;
    }

    /** The corner radius of the magnifier window, in pixels. */
    public float getCornerRadius() {
        return mCornerRadius;
    }

    /** The horizontal offset between the source and the magnifier, in pixels. */
    public int getDefaultHorizontalSourceToMagnifierOffset() {
        return mDefaultHorizontalSourceToMagnifierOffset;
    }

    /** The vertical offset between the source and the magnifier, in pixels. */
    public int getDefaultVerticalSourceToMagnifierOffset() {
        return mDefaultVerticalSourceToMagnifierOffset;
    }

    /** The overlay drawn over the magnified content, if any. */
    @Nullable
    public Drawable getOverlay() {
        return mOverlay;
    }

    /** Whether the magnifier window is clipped inside the screen. */
    public boolean isClippingEnabled() {
        return mClippingEnabled;
    }

    /** The position of the magnifier window on the screen, as of the last show. */
    @Nullable
    public Point getPosition() {
        return mShowing ? new Point(mPosition) : null;
    }

    /** The position of the magnified source on the screen, as of the last show. */
    @Nullable
    public Point getSourcePosition() {
        return mShowing ? new Point(mSourcePosition) : null;
    }

    /** @hide Editor's: whether the cursor is drawn over the content; there is no content. */
    public void setDrawCursor(boolean enable, Drawable cursorDrawable) {
    }

    /** @hide Editor's: the content's height and the zoom, for the fish-eye factors; there is no content. */
    public void updateSourceFactors(final int sourceHeight, final float zoom) {
        mZoom = zoom;
    }

    /** Sets a callback that is told when a show, an update or a dismiss is done. */
    public void setOnOperationCompleteCallback(@Nullable Callback callback) {
        mCallback = callback;
    }

    /** A callback for the completion of the magnifier's operations. */
    public interface Callback {
        /** Called when a show, an update or a dismiss has completed. */
        void onOperationComplete();
    }

    /** The builder of magnifiers. */
    public static final class Builder {
        private final View mView;
        private @Px @IntRange(from = 0) int mWidth;
        private @Px @IntRange(from = 0) int mHeight;
        private int mSourceWidth;
        private int mSourceHeight;
        private @FloatRange(from = 0f) float mInitialZoom;
        private @FloatRange(from = 0f) float mElevation;
        private @FloatRange(from = 0f) float mCornerRadius;
        private Drawable mOverlay;
        private int mHorizontalDefaultSourceToMagnifierOffset;
        private int mVerticalDefaultSourceToMagnifierOffset;
        private boolean mClippingEnabled;

        /** Creates a builder for a magnifier of a view. */
        public Builder(@NonNull View view) {
            mView = java.util.Objects.requireNonNull(view);
            final float density = view.getContext().getResources().getDisplayMetrics().density;
            mWidth = Math.round(100 * density);
            mHeight = Math.round(48 * density);
            mInitialZoom = 1.25f;
            mElevation = 4 * density;
            mCornerRadius = 2 * density;
            mClippingEnabled = true;
        }

        /** Sets the size of the magnifier window, in pixels. */
        @NonNull
        public Builder setSize(@Px @IntRange(from = 0) int width, @Px @IntRange(from = 0) int height) {
            mWidth = width;
            mHeight = height;
            return this;
        }

        /** @hide Sets the size of the source, in pixels. */
        @NonNull
        public Builder setSourceSize(int width, int height) {
            mSourceWidth = width;
            mSourceHeight = height;
            return this;
        }

        /** Sets the zoom the magnifier starts with. */
        @NonNull
        public Builder setInitialZoom(@FloatRange(from = 0f) float zoom) {
            mInitialZoom = zoom;
            return this;
        }

        /** Sets the elevation of the magnifier window, in pixels. */
        @NonNull
        public Builder setElevation(@Px @FloatRange(from = 0) float elevation) {
            mElevation = elevation;
            return this;
        }

        /** Sets the corner radius of the magnifier window, in pixels. */
        @NonNull
        public Builder setCornerRadius(@Px @FloatRange(from = 0) float cornerRadius) {
            mCornerRadius = cornerRadius;
            return this;
        }

        /** Sets an overlay drawn over the magnified content. */
        @NonNull
        public Builder setOverlay(@Nullable Drawable overlay) {
            mOverlay = overlay;
            return this;
        }

        /** Sets the offset between the source and the magnifier, in pixels. */
        @NonNull
        public Builder setDefaultSourceToMagnifierOffset(@Px int horizontalOffset, @Px int verticalOffset) {
            mHorizontalDefaultSourceToMagnifierOffset = horizontalOffset;
            mVerticalDefaultSourceToMagnifierOffset = verticalOffset;
            return this;
        }

        /** Sets whether the magnifier window is clipped inside the screen. */
        @NonNull
        public Builder setClippingEnabled(boolean clip) {
            mClippingEnabled = clip;
            return this;
        }

        /** @hide Sets the style of the magnified content to the fish eye; nothing is drawn. */
        @NonNull
        public Builder setFishEyeStyle() {
            return this;
        }

        /** @hide Sets the bounds the source may extend to. */
        @NonNull
        public Builder setSourceBounds(int left, int top, int right, int bottom) {
            return this;
        }

        /** Builds the magnifier. */
        @NonNull
        public Magnifier build() {
            return new Magnifier(this);
        }
    }
}
