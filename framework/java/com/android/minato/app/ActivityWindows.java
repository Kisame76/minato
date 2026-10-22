package com.android.minato.app;

import java.util.ArrayList;
import java.util.List;

/**
 * Which window each of the task's activities has (ADR-0041 gap 4 (b3), the plan's decision 3), in plain Java so that
 * the host's JVM tests it. As Android 15's ensureActivitiesVisible: from the top down every activity that is not
 * finishing is visible until the first one that occludes the ones below it, as ActivityRecord.occludesParent has it
 * (its theme has neither windowIsTranslucent nor windowIsFloating, or setTranslucent made it opaque). The lowest
 * visible activity has the session's window and its input, unless it floats or has a requested window already: then
 * it has a requested window, at its frame for a floating one, the session's window stays empty, and the input still
 * reaches it. So an activity keeps its window while it stays visible, as Android keeps it, and its SurfaceViews keep
 * their surfaces: a translucent activity that setTranslucent made opaque, or whose activity below finished, goes to
 * the session's window only when it shows again. Every other visible activity has a requested window of its own, the
 * rest have none.
 *
 * <p>Each visible activity has a key, the group of its windows in decision 1's order. Its requested windows keep the
 * group they were made with, so a key stays while the keys still grow from the bottom up; an activity that comes
 * above one with a larger key, as a reorder to the front brings it, gets a new one, and its windows are made again.
 */
public final class ActivityWindows {
    /** No window: the activity is not visible, and its windows are taken away. */
    public static final int HIDDEN = 0;
    /** The session's window, the lowest visible activity's. */
    public static final int SESSION = 1;
    /** A requested window of type 1 with the activity's key as its group. */
    public static final int REQUESTED = 2;

    /** One activity of the task, which the caller describes before each update. */
    public static final class Slot {
        /** The app or the system finishes it. */
        public boolean finishing;
        /** Its theme has windowIsTranslucent. */
        public boolean translucentTheme;
        /** Its theme has windowIsFloating: its window lies at WindowLayout's frame. */
        public boolean floatingTheme;
        /** setTranslucent's: null before any call, else whether the activity was made translucent. */
        public Boolean converted;
        int mMode = HIDDEN;
        int mKey;

        /** HIDDEN, SESSION or REQUESTED, as the last update gave it. */
        public int mode() {
            return mMode;
        }

        /** The group of its windows: above 0, larger above; 0 before it was first visible. */
        public int key() {
            return mKey;
        }

        /** As ActivityRecord.occludesParent: the activities below it are not visible. */
        public boolean occludes() {
            return converted != null ? !converted : !(translucentTheme || floatingTheme);
        }
    }

    private int mKeys;
    private Slot mSession;
    private Slot mInput;

    /**
     * The pass over the task's activities, bottom up: each one's mode and key, and those whose mode or key changed,
     * bottom up, whose windows the caller takes away and makes again.
     */
    public List<Slot> update(List<Slot> bottomUp) {
        int lowest = bottomUp.size();
        for (int i = bottomUp.size() - 1; i >= 0; i--) {
            final Slot slot = bottomUp.get(i);
            if (slot.finishing) {
                continue;
            }
            lowest = i;
            if (slot.occludes()) {
                break;
            }
        }
        mSession = null;
        mInput = null;
        final List<Slot> changed = new ArrayList<>();
        int lastKey = 0;
        for (int i = 0; i < bottomUp.size(); i++) {
            final Slot slot = bottomUp.get(i);
            final int mode = i < lowest || slot.finishing ? HIDDEN
                    : i == lowest && !slot.floatingTheme && slot.mMode != REQUESTED ? SESSION : REQUESTED;
            int key = slot.mKey;
            if (mode != HIDDEN) {
                if (key <= lastKey) {
                    key = ++mKeys;
                }
                lastKey = key;
                if (i == lowest) {
                    mInput = slot;
                    mSession = mode == SESSION ? slot : null;
                }
            }
            if (mode != slot.mMode || key != slot.mKey) {
                slot.mMode = mode;
                slot.mKey = key;
                changed.add(slot);
            }
        }
        return changed;
    }

    /** The activity with the session's window after the last update, or null. */
    public Slot session() {
        return mSession;
    }

    /** The lowest visible activity, which the session's input reaches, or null. */
    public Slot input() {
        return mInput;
    }
}
