package com.android.internal.app;

import android.app.ActionBar;
import android.app.ActionBar.LayoutParams;
import android.app.ActionBar.OnMenuVisibilityListener;
import android.app.ActionBar.OnNavigationListener;
import android.app.ActionBar.Tab;
import android.content.Context;
import android.graphics.drawable.Drawable;
import android.view.View;
import android.widget.SpinnerAdapter;

/**
 * Compile-only stub (ADR-0039 step c2c3), never in framework.jar: Activity makes it for a window with
 * FEATURE_ACTION_BAR (initWindowDecorActionBar), with the action bar decor, and Dialog for its window with that feature.
 */
public class WindowDecorActionBar extends ActionBar {
    public WindowDecorActionBar(android.app.Activity activity) {
        throw new RuntimeException("stub");
    }

    public WindowDecorActionBar(android.app.Dialog dialog) {
        throw new RuntimeException("stub");
    }

    public void setDefaultDisplayHomeAsUpEnabled(boolean enable) {
        throw new RuntimeException("stub");
    }

    public void setCustomView(View view) {
        throw new RuntimeException("stub");
    }

    public void setCustomView(View view, LayoutParams layoutParams) {
        throw new RuntimeException("stub");
    }

    public void setCustomView(int resId) {
        throw new RuntimeException("stub");
    }

    public void setIcon(int resId) {
        throw new RuntimeException("stub");
    }

    public void setIcon(Drawable icon) {
        throw new RuntimeException("stub");
    }

    public void setLogo(int resId) {
        throw new RuntimeException("stub");
    }

    public void setLogo(Drawable logo) {
        throw new RuntimeException("stub");
    }

    public void setListNavigationCallbacks(SpinnerAdapter adapter, OnNavigationListener callback) {
        throw new RuntimeException("stub");
    }

    public void setSelectedNavigationItem(int position) {
        throw new RuntimeException("stub");
    }

    public int getSelectedNavigationIndex() {
        throw new RuntimeException("stub");
    }

    public int getNavigationItemCount() {
        throw new RuntimeException("stub");
    }

    public void setTitle(CharSequence title) {
        throw new RuntimeException("stub");
    }

    public void setTitle(int resId) {
        throw new RuntimeException("stub");
    }

    public void setSubtitle(CharSequence subtitle) {
        throw new RuntimeException("stub");
    }

    public void setSubtitle(int resId) {
        throw new RuntimeException("stub");
    }

    public void setDisplayOptions(int options) {
        throw new RuntimeException("stub");
    }

    public void setDisplayOptions(int options, int mask) {
        throw new RuntimeException("stub");
    }

    public void setDisplayUseLogoEnabled(boolean useLogo) {
        throw new RuntimeException("stub");
    }

    public void setDisplayShowHomeEnabled(boolean showHome) {
        throw new RuntimeException("stub");
    }

    public void setDisplayHomeAsUpEnabled(boolean showHomeAsUp) {
        throw new RuntimeException("stub");
    }

    public void setDisplayShowTitleEnabled(boolean showTitle) {
        throw new RuntimeException("stub");
    }

    public void setDisplayShowCustomEnabled(boolean showCustom) {
        throw new RuntimeException("stub");
    }

    public void setBackgroundDrawable(Drawable d) {
        throw new RuntimeException("stub");
    }

    public View getCustomView() {
        throw new RuntimeException("stub");
    }

    public CharSequence getTitle() {
        throw new RuntimeException("stub");
    }

    public CharSequence getSubtitle() {
        throw new RuntimeException("stub");
    }

    public int getNavigationMode() {
        throw new RuntimeException("stub");
    }

    public void setNavigationMode(int mode) {
        throw new RuntimeException("stub");
    }

    public int getDisplayOptions() {
        throw new RuntimeException("stub");
    }

    public Tab newTab() {
        throw new RuntimeException("stub");
    }

    public void addTab(Tab tab) {
        throw new RuntimeException("stub");
    }

    public void addTab(Tab tab, boolean setSelected) {
        throw new RuntimeException("stub");
    }

    public void addTab(Tab tab, int position) {
        throw new RuntimeException("stub");
    }

    public void addTab(Tab tab, int position, boolean setSelected) {
        throw new RuntimeException("stub");
    }

    public void removeTab(Tab tab) {
        throw new RuntimeException("stub");
    }

    public void removeTabAt(int position) {
        throw new RuntimeException("stub");
    }

    public void removeAllTabs() {
        throw new RuntimeException("stub");
    }

    public void selectTab(Tab tab) {
        throw new RuntimeException("stub");
    }

    public Tab getSelectedTab() {
        throw new RuntimeException("stub");
    }

    public Tab getTabAt(int index) {
        throw new RuntimeException("stub");
    }

    public int getTabCount() {
        throw new RuntimeException("stub");
    }

    public int getHeight() {
        throw new RuntimeException("stub");
    }

    public void show() {
        throw new RuntimeException("stub");
    }

    public void hide() {
        throw new RuntimeException("stub");
    }

    public boolean isShowing() {
        throw new RuntimeException("stub");
    }

    public void addOnMenuVisibilityListener(OnMenuVisibilityListener listener) {
        throw new RuntimeException("stub");
    }

    public void removeOnMenuVisibilityListener(OnMenuVisibilityListener listener) {
        throw new RuntimeException("stub");
    }
}
