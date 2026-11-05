/*
 * Copyright (C) 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package android.window;

import android.annotation.NonNull;
import android.annotation.Nullable;
import android.content.Context;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;

import java.time.Duration;
import java.time.Instant;

/**
 * Ours, under AOSP's name and public surface: the view the system's starting window hands to an app's
 * {@link SplashScreen.OnExitAnimationListener}. Minato shows no system starting window, so the listener is never
 * called with one; the class is here so that the apps and libraries that name it (androidx.core.splashscreen)
 * load and link. A view made by hand has no icon and no animation, as one whose theme sets none.
 */
public final class SplashScreenView extends FrameLayout {
    /** @hide */
    public SplashScreenView(@NonNull Context context) {
        super(context);
    }

    /** The duration of the icon's animation, or null when the splash screen has none. */
    @Nullable
    public Duration getIconAnimationDuration() {
        return null;
    }

    /** When the icon's animation started, or null when the splash screen has none. */
    @Nullable
    public Instant getIconAnimationStart() {
        return null;
    }

    /** The view of the splash screen's icon, or null when it has none. */
    @Nullable
    public View getIconView() {
        return null;
    }

    /** Removes the view from the window's content. */
    public void remove() {
        final ViewGroup parent = (ViewGroup) getParent();
        if (parent != null) {
            parent.removeView(this);
        }
    }
}
