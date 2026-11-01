package com.android.internal.minato.webkit;

import android.content.Context;
import android.content.pm.PackageManager;
import android.os.Build;
import android.webkit.WebSettings;

/**
 * A WebView's settings (ADR-0056 decision 1): every one kept with the default Android 15's WebView gives it, as its
 * documentation says and webview-start-test logs on the AVD. Step c) pushes the ones the host uses at load; until then
 * they are only kept. The defaults that depend on the app: file access is off from target 30, third-party cookies and
 * mixed content are refused from target 21, and network loads are blocked for an app without the INTERNET permission,
 * as Chromium's AwSettings has them.
 */
@SuppressWarnings("deprecation")
final class WebSettingsImpl extends WebSettings {
    private boolean mNavDump;
    private boolean mSupportZoom = true;
    private boolean mMediaPlaybackRequiresUserGesture = true;
    private boolean mBuiltInZoomControls;
    private boolean mDisplayZoomControls = true;
    private boolean mAllowFileAccess;
    private boolean mAllowContentAccess = true;
    private boolean mLoadWithOverviewMode;
    private boolean mEnableSmoothTransition;
    private boolean mUseWebViewBackgroundForOverscroll;
    private boolean mSaveFormData;
    private boolean mSavePassword;
    private int mTextZoom = 100;
    private boolean mAcceptThirdPartyCookies;
    private ZoomDensity mDefaultZoom = ZoomDensity.MEDIUM;
    private boolean mLightTouchEnabled;
    private int mUserAgent;
    private boolean mUseWideViewPort;
    private boolean mSupportMultipleWindows;
    private LayoutAlgorithm mLayoutAlgorithm = LayoutAlgorithm.NARROW_COLUMNS;
    private String mStandardFontFamily = "sans-serif";
    private String mFixedFontFamily = "monospace";
    private String mSansSerifFontFamily = "sans-serif";
    private String mSerifFontFamily = "serif";
    private String mCursiveFontFamily = "cursive";
    private String mFantasyFontFamily = "fantasy";
    private int mMinimumFontSize = 8;
    private int mMinimumLogicalFontSize = 8;
    private int mDefaultFontSize = 16;
    private int mDefaultFixedFontSize = 13;
    private boolean mLoadsImagesAutomatically = true;
    private boolean mBlockNetworkImage;
    private boolean mBlockNetworkLoads;
    private final boolean mHasInternetPermission;
    private boolean mJavaScriptEnabled;
    private boolean mAllowUniversalAccessFromFileURLs;
    private boolean mAllowFileAccessFromFileURLs;
    private PluginState mPluginState = PluginState.OFF;
    private String mDatabasePath = "";
    private boolean mDatabaseEnabled;
    private boolean mDomStorageEnabled;
    private boolean mGeolocationEnabled = true;
    private boolean mJavaScriptCanOpenWindowsAutomatically;
    private String mDefaultTextEncodingName = "UTF-8";
    private String mUserAgentString;
    private final String mDefaultUserAgent;
    private int mCacheMode = LOAD_DEFAULT;
    private int mMixedContentMode;
    private boolean mVideoOverlayForEmbeddedEncryptedVideoEnabled;
    private boolean mOffscreenPreRaster;
    private boolean mSafeBrowsingEnabled = true;
    private int mDisabledActionModeMenuItems = MENU_ITEM_NONE;
    private int mForceDark = FORCE_DARK_AUTO;
    private boolean mAlgorithmicDarkeningAllowed;

    WebSettingsImpl(Context context) {
        int target = context.getApplicationInfo().targetSdkVersion;
        mAllowFileAccess = target < Build.VERSION_CODES.R;
        mAcceptThirdPartyCookies = target < Build.VERSION_CODES.LOLLIPOP;
        mMixedContentMode = target < Build.VERSION_CODES.LOLLIPOP ? MIXED_CONTENT_ALWAYS_ALLOW
                : MIXED_CONTENT_NEVER_ALLOW;
        mHasInternetPermission = context.checkSelfPermission(android.Manifest.permission.INTERNET)
                == PackageManager.PERMISSION_GRANTED;
        mBlockNetworkLoads = !mHasInternetPermission;
        mDefaultUserAgent = WebViewPackage.userAgent();
        mUserAgentString = mDefaultUserAgent;
    }

    @Override public synchronized void setNavDump(boolean enabled) { mNavDump = enabled; }
    @Override public synchronized boolean getNavDump() { return mNavDump; }
    @Override public synchronized void setSupportZoom(boolean support) { mSupportZoom = support; }
    @Override public synchronized boolean supportZoom() { return mSupportZoom; }
    @Override public synchronized void setMediaPlaybackRequiresUserGesture(boolean require) {
        mMediaPlaybackRequiresUserGesture = require;
    }
    @Override public synchronized boolean getMediaPlaybackRequiresUserGesture() {
        return mMediaPlaybackRequiresUserGesture;
    }
    @Override public synchronized void setBuiltInZoomControls(boolean enabled) { mBuiltInZoomControls = enabled; }
    @Override public synchronized boolean getBuiltInZoomControls() { return mBuiltInZoomControls; }
    @Override public synchronized void setDisplayZoomControls(boolean enabled) { mDisplayZoomControls = enabled; }
    @Override public synchronized boolean getDisplayZoomControls() { return mDisplayZoomControls; }
    @Override public synchronized void setAllowFileAccess(boolean allow) { mAllowFileAccess = allow; }
    @Override public synchronized boolean getAllowFileAccess() { return mAllowFileAccess; }
    @Override public synchronized void setAllowContentAccess(boolean allow) { mAllowContentAccess = allow; }
    @Override public synchronized boolean getAllowContentAccess() { return mAllowContentAccess; }
    @Override public synchronized void setLoadWithOverviewMode(boolean overview) { mLoadWithOverviewMode = overview; }
    @Override public synchronized boolean getLoadWithOverviewMode() { return mLoadWithOverviewMode; }
    @Override public synchronized void setEnableSmoothTransition(boolean enable) { mEnableSmoothTransition = enable; }
    @Override public synchronized boolean enableSmoothTransition() { return mEnableSmoothTransition; }
    @Override public synchronized void setUseWebViewBackgroundForOverscrollBackground(boolean view) {
        mUseWebViewBackgroundForOverscroll = view;
    }
    @Override public synchronized boolean getUseWebViewBackgroundForOverscrollBackground() {
        return mUseWebViewBackgroundForOverscroll;
    }
    @Override public synchronized void setSaveFormData(boolean save) { mSaveFormData = save; }
    @Override public synchronized boolean getSaveFormData() { return mSaveFormData; }
    @Override public synchronized void setSavePassword(boolean save) { mSavePassword = save; }
    @Override public synchronized boolean getSavePassword() { return mSavePassword; }
    @Override public synchronized void setTextZoom(int textZoom) { mTextZoom = textZoom; }
    @Override public synchronized int getTextZoom() { return mTextZoom; }
    @Override public synchronized void setAcceptThirdPartyCookies(boolean accept) { mAcceptThirdPartyCookies = accept; }
    @Override public synchronized boolean getAcceptThirdPartyCookies() { return mAcceptThirdPartyCookies; }
    @Override public synchronized void setDefaultZoom(ZoomDensity zoom) { mDefaultZoom = zoom; }
    @Override public synchronized ZoomDensity getDefaultZoom() { return mDefaultZoom; }
    @Override public synchronized void setLightTouchEnabled(boolean enabled) { mLightTouchEnabled = enabled; }
    @Override public synchronized boolean getLightTouchEnabled() { return mLightTouchEnabled; }
    @Override public synchronized void setUserAgent(int ua) { mUserAgent = ua; }
    @Override public synchronized int getUserAgent() { return mUserAgent; }
    @Override public synchronized void setUseWideViewPort(boolean use) { mUseWideViewPort = use; }
    @Override public synchronized boolean getUseWideViewPort() { return mUseWideViewPort; }
    @Override public synchronized void setSupportMultipleWindows(boolean support) { mSupportMultipleWindows = support; }
    @Override public synchronized boolean supportMultipleWindows() { return mSupportMultipleWindows; }
    @Override public synchronized void setLayoutAlgorithm(LayoutAlgorithm l) { mLayoutAlgorithm = l; }
    @Override public synchronized LayoutAlgorithm getLayoutAlgorithm() { return mLayoutAlgorithm; }
    @Override public synchronized void setStandardFontFamily(String font) { mStandardFontFamily = font; }
    @Override public synchronized String getStandardFontFamily() { return mStandardFontFamily; }
    @Override public synchronized void setFixedFontFamily(String font) { mFixedFontFamily = font; }
    @Override public synchronized String getFixedFontFamily() { return mFixedFontFamily; }
    @Override public synchronized void setSansSerifFontFamily(String font) { mSansSerifFontFamily = font; }
    @Override public synchronized String getSansSerifFontFamily() { return mSansSerifFontFamily; }
    @Override public synchronized void setSerifFontFamily(String font) { mSerifFontFamily = font; }
    @Override public synchronized String getSerifFontFamily() { return mSerifFontFamily; }
    @Override public synchronized void setCursiveFontFamily(String font) { mCursiveFontFamily = font; }
    @Override public synchronized String getCursiveFontFamily() { return mCursiveFontFamily; }
    @Override public synchronized void setFantasyFontFamily(String font) { mFantasyFontFamily = font; }
    @Override public synchronized String getFantasyFontFamily() { return mFantasyFontFamily; }
    @Override public synchronized void setMinimumFontSize(int size) { mMinimumFontSize = clampFontSize(size); }
    @Override public synchronized int getMinimumFontSize() { return mMinimumFontSize; }
    @Override public synchronized void setMinimumLogicalFontSize(int size) {
        mMinimumLogicalFontSize = clampFontSize(size);
    }
    @Override public synchronized int getMinimumLogicalFontSize() { return mMinimumLogicalFontSize; }
    @Override public synchronized void setDefaultFontSize(int size) { mDefaultFontSize = clampFontSize(size); }
    @Override public synchronized int getDefaultFontSize() { return mDefaultFontSize; }
    @Override public synchronized void setDefaultFixedFontSize(int size) {
        mDefaultFixedFontSize = clampFontSize(size);
    }
    @Override public synchronized int getDefaultFixedFontSize() { return mDefaultFixedFontSize; }
    @Override public synchronized void setLoadsImagesAutomatically(boolean flag) { mLoadsImagesAutomatically = flag; }
    @Override public synchronized boolean getLoadsImagesAutomatically() { return mLoadsImagesAutomatically; }
    @Override public synchronized void setBlockNetworkImage(boolean flag) { mBlockNetworkImage = flag; }
    @Override public synchronized boolean getBlockNetworkImage() { return mBlockNetworkImage; }

    /** As AwSettings': an app without the INTERNET permission cannot unblock network loads. */
    @Override public synchronized void setBlockNetworkLoads(boolean flag) {
        if (!flag && !mHasInternetPermission) {
            throw new SecurityException("Permission denied - application missing INTERNET permission");
        }
        mBlockNetworkLoads = flag;
    }
    @Override public synchronized boolean getBlockNetworkLoads() { return mBlockNetworkLoads; }
    @Override public synchronized void setJavaScriptEnabled(boolean flag) { mJavaScriptEnabled = flag; }
    @Override public synchronized boolean getJavaScriptEnabled() { return mJavaScriptEnabled; }
    @Override public synchronized void setAllowUniversalAccessFromFileURLs(boolean flag) {
        mAllowUniversalAccessFromFileURLs = flag;
    }
    @Override public synchronized boolean getAllowUniversalAccessFromFileURLs() {
        return mAllowUniversalAccessFromFileURLs;
    }
    @Override public synchronized void setAllowFileAccessFromFileURLs(boolean flag) {
        mAllowFileAccessFromFileURLs = flag;
    }
    @Override public synchronized boolean getAllowFileAccessFromFileURLs() { return mAllowFileAccessFromFileURLs; }
    @Override public synchronized void setPluginsEnabled(boolean flag) {
        mPluginState = flag ? PluginState.ON : PluginState.OFF;
    }
    @Override public synchronized boolean getPluginsEnabled() { return mPluginState == PluginState.ON; }
    @Override public synchronized void setPluginState(PluginState state) { mPluginState = state; }
    @Override public synchronized PluginState getPluginState() { return mPluginState; }
    @Override public synchronized void setDatabasePath(String databasePath) { mDatabasePath = databasePath; }
    @Override public synchronized String getDatabasePath() { return mDatabasePath; }
    @Override public synchronized void setGeolocationDatabasePath(String databasePath) {}
    @Override public synchronized void setDatabaseEnabled(boolean flag) { mDatabaseEnabled = flag; }
    @Override public synchronized boolean getDatabaseEnabled() { return mDatabaseEnabled; }
    @Override public synchronized void setDomStorageEnabled(boolean flag) { mDomStorageEnabled = flag; }
    @Override public synchronized boolean getDomStorageEnabled() { return mDomStorageEnabled; }
    @Override public synchronized void setGeolocationEnabled(boolean flag) { mGeolocationEnabled = flag; }
    @Override public synchronized void setJavaScriptCanOpenWindowsAutomatically(boolean flag) {
        mJavaScriptCanOpenWindowsAutomatically = flag;
    }
    @Override public synchronized boolean getJavaScriptCanOpenWindowsAutomatically() {
        return mJavaScriptCanOpenWindowsAutomatically;
    }
    @Override public synchronized void setDefaultTextEncodingName(String encoding) {
        if (encoding != null) {
            mDefaultTextEncodingName = encoding;
        }
    }
    @Override public synchronized String getDefaultTextEncodingName() { return mDefaultTextEncodingName; }

    /** As AwSettings': null or empty restores the default. */
    @Override public synchronized void setUserAgentString(String ua) {
        mUserAgentString = ua == null || ua.isEmpty() ? mDefaultUserAgent : ua;
    }
    @Override public synchronized String getUserAgentString() { return mUserAgentString; }
    @Override public void setNeedInitialFocus(boolean flag) {}
    @Override public synchronized void setRenderPriority(RenderPriority priority) {}
    @Override public synchronized void setCacheMode(int mode) { mCacheMode = mode; }
    @Override public synchronized int getCacheMode() { return mCacheMode; }
    @Override public synchronized void setMixedContentMode(int mode) { mMixedContentMode = mode; }
    @Override public synchronized int getMixedContentMode() { return mMixedContentMode; }
    @Override public synchronized void setVideoOverlayForEmbeddedEncryptedVideoEnabled(boolean flag) {
        mVideoOverlayForEmbeddedEncryptedVideoEnabled = flag;
    }
    @Override public synchronized boolean getVideoOverlayForEmbeddedEncryptedVideoEnabled() {
        return mVideoOverlayForEmbeddedEncryptedVideoEnabled;
    }
    @Override public synchronized void setOffscreenPreRaster(boolean enabled) { mOffscreenPreRaster = enabled; }
    @Override public synchronized boolean getOffscreenPreRaster() { return mOffscreenPreRaster; }
    @Override public synchronized void setSafeBrowsingEnabled(boolean enabled) { mSafeBrowsingEnabled = enabled; }
    @Override public synchronized boolean getSafeBrowsingEnabled() { return mSafeBrowsingEnabled; }
    @Override public synchronized void setDisabledActionModeMenuItems(int menuItems) {
        mDisabledActionModeMenuItems = menuItems;
    }
    @Override public synchronized int getDisabledActionModeMenuItems() { return mDisabledActionModeMenuItems; }
    @Override public synchronized void setForceDark(int forceDark) { mForceDark = forceDark; }
    @Override public synchronized int getForceDark() { return mForceDark; }
    @Override public synchronized void setAlgorithmicDarkeningAllowed(boolean allow) {
        mAlgorithmicDarkeningAllowed = allow;
    }
    @Override public synchronized boolean isAlgorithmicDarkeningAllowed() { return mAlgorithmicDarkeningAllowed; }

    /** As AwSettings': font sizes between 1 and 72. */
    private static int clampFontSize(int size) {
        return Math.max(1, Math.min(72, size));
    }
}
