package android.view;

import android.animation.LayoutTransition;
import android.graphics.Matrix;
import android.graphics.Point;
import android.graphics.Rect;
import android.graphics.RenderNode;
import android.view.View.AttachInfo;
import android.view.Window.OnContentApplyWindowInsetsListener;
import android.view.accessibility.AccessibilityNodeInfo;
import android.window.WindowOnBackInvokedDispatcher;
import java.util.List;

/**
 * Compile-only stub (ADR-0039 step c2c2), never in framework.jar: the window's root, which View, ViewGroup and
 * ViewAnimationHostBridge reach through an attached view's AttachInfo. A detached view has none, so none of this runs
 * before step c2c3, whose small ViewRootImpl (ADR-0041 decision 2) replaces it with these members.
 */
public abstract class ViewRootImpl implements ViewParent, AttachedSurfaceControl {
    public static final int INTERMITTENT_STATE_NOT_INTERMITTENT = 1;

    public static final int INTERMITTENT_STATE_INTERMITTENT = 0;

    Thread mThread;

    public WindowManager.LayoutParams mWindowAttributes;

    public boolean mIsAnimating;

    Choreographer mChoreographer;

    protected ViewFrameInfo mViewFrameInfo;

    public Surface mSurface;

    int mCurScrollY;

    public ImeFocusController getImeFocusController() {
        throw new RuntimeException("stub");
    }

    public HandwritingInitiator getHandwritingInitiator() {
        throw new RuntimeException("stub");
    }

    public void registerAnimatingRenderNode(RenderNode animator) {
        throw new RuntimeException("stub");
    }

    public void registerVectorDrawableAnimator(NativeVectorDrawableAnimator animator) {
        throw new RuntimeException("stub");
    }

    public void requestTransitionStart(LayoutTransition transition) {
        throw new RuntimeException("stub");
    }

    public void notifyRendererOfExpensiveFrame() {
        throw new RuntimeException("stub");
    }

    void transformMatrixToGlobal(Matrix m) {
        throw new RuntimeException("stub");
    }

    void transformMatrixToLocal(Matrix m) {
        throw new RuntimeException("stub");
    }

    WindowInsets getWindowInsets(boolean forceConstruct) {
        throw new RuntimeException("stub");
    }

    public InsetsController getInsetsController() {
        throw new RuntimeException("stub");
    }

    void addSensitiveContentAppProtection() {
        throw new RuntimeException("stub");
    }

    void removeSensitiveContentAppProtection() {
        throw new RuntimeException("stub");
    }

    public void dispatchCompatFakeFocus() {
        throw new RuntimeException("stub");
    }

    boolean isInLayout() {
        throw new RuntimeException("stub");
    }

    boolean requestLayoutDuringLayout(final View view) {
        throw new RuntimeException("stub");
    }

    void updateSystemGestureExclusionRectsForView(View view) {
        throw new RuntimeException("stub");
    }

    void updateKeepClearRectsForView(View view) {
        throw new RuntimeException("stub");
    }

    public View getAccessibilityFocusedHost() {
        throw new RuntimeException("stub");
    }

    void setAccessibilityFocus(View view, AccessibilityNodeInfo node) {
        throw new RuntimeException("stub");
    }

    boolean hasPointerCapture() {
        throw new RuntimeException("stub");
    }

    void requestPointerCapture(boolean enabled) {
        throw new RuntimeException("stub");
    }

    public static boolean isViewDescendantOf(View child, View parent) {
        throw new RuntimeException("stub");
    }

    boolean ensureTouchMode(boolean inTouchMode) {
        throw new RuntimeException("stub");
    }

    public void refreshPointerIcon() {
        throw new RuntimeException("stub");
    }

    void setLocalDragState(Object obj) {
        throw new RuntimeException("stub");
    }

    public void getLastTouchPoint(Point outLocation) {
        throw new RuntimeException("stub");
    }

    public int getLastTouchSource() {
        throw new RuntimeException("stub");
    }

    public int getLastTouchDeviceId() {
        throw new RuntimeException("stub");
    }

    public int getLastTouchPointerId() {
        throw new RuntimeException("stub");
    }

    public void setDragFocus(View newDragTarget, DragEvent event) {
        throw new RuntimeException("stub");
    }

    void setDragStartedViewForAccessibility(View view) {
        throw new RuntimeException("stub");
    }

    void getDisplayFrame(Rect outFrame) {
        throw new RuntimeException("stub");
    }

    void getWindowVisibleDisplayFrame(Rect outFrame) {
        throw new RuntimeException("stub");
    }

    void applyViewBoundsSandboxingIfNeeded(final Rect inOutRect) {
        throw new RuntimeException("stub");
    }

    public void applyViewLocationSandboxingIfNeeded( int[] outLocation) {
        throw new RuntimeException("stub");
    }

    public void dispatchInvalidateDelayed(View view, long delayMilliseconds) {
        throw new RuntimeException("stub");
    }

    public void dispatchInvalidateRectDelayed(AttachInfo.InvalidateInfo info, long delayMilliseconds) {
        throw new RuntimeException("stub");
    }

    public void dispatchInvalidateOnAnimation(View view) {
        throw new RuntimeException("stub");
    }

    public void dispatchInvalidateRectOnAnimation(AttachInfo.InvalidateInfo info) {
        throw new RuntimeException("stub");
    }

    public void cancelInvalidate(View view) {
        throw new RuntimeException("stub");
    }

    public boolean probablyHasInput() {
        throw new RuntimeException("stub");
    }

    public SurfaceControl getSurfaceControl() {
        throw new RuntimeException("stub");
    }

    public WindowOnBackInvokedDispatcher getOnBackInvokedDispatcher() {
        throw new RuntimeException("stub");
    }

    public void votePreferredFrameRateCategory(int frameRateCategory, int reason, View view) {
        throw new RuntimeException("stub");
    }

    public void addThreadedRendererView(View view) {
        throw new RuntimeException("stub");
    }

    public void removeThreadedRendererView(View view) {
        throw new RuntimeException("stub");
    }

    int intermittentUpdateState() {
        throw new RuntimeException("stub");
    }

    public boolean shouldCheckFrameRateCategory() {
        throw new RuntimeException("stub");
    }

    public boolean shouldCheckFrameRate(boolean isDirect) {
        throw new RuntimeException("stub");
    }

    public void votePreferredFrameRate(float frameRate, int frameRateCompatibility) {
        throw new RuntimeException("stub");
    }

    void recordViewPercentage(float percentage) {
        throw new RuntimeException("stub");
    }
}
