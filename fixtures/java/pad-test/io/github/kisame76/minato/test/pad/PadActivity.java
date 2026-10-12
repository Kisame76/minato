package io.github.kisame76.minato.test.pad;

import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import com.google.android.gms.tasks.Task;
import com.google.android.play.core.assetpacks.AssetPackException;
import com.google.android.play.core.assetpacks.AssetPackLocation;
import com.google.android.play.core.assetpacks.AssetPackManager;
import com.google.android.play.core.assetpacks.AssetPackManagerFactory;
import com.google.android.play.core.assetpacks.AssetPackState;
import com.google.android.play.core.assetpacks.AssetPackStateUpdateListener;
import com.google.android.play.core.assetpacks.AssetPackStates;
import com.google.android.play.core.assetpacks.model.AssetPackErrorCode;
import com.google.android.play.core.assetpacks.model.AssetPackStatus;
import com.google.android.play.core.assetpacks.model.AssetPackStorageMethod;
import com.google.android.play.core.splitinstall.SplitInstallException;
import com.google.android.play.core.splitinstall.SplitInstallManager;
import com.google.android.play.core.splitinstall.SplitInstallManagerFactory;
import com.google.android.play.core.splitinstall.SplitInstallRequest;
import com.google.android.play.core.splitinstall.SplitInstallSessionState;
import com.google.android.play.core.splitinstall.SplitInstallStateUpdatedListener;
import com.google.android.play.core.splitinstall.model.SplitInstallErrorCode;
import com.google.android.play.core.splitinstall.model.SplitInstallSessionStatus;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.TreeSet;

/**
 * ADR-0050's test 3: asks Google's asset-delivery library for the app's fast-follow pack, then for its on-demand pack,
 * one at a time from the resumed activity, as fetch() wants the app in the foreground. The library's local-testing path
 * serves them from the slices in local_testing of the app's external files folder, which the meta-data
 * local_testing_dir names: on Android 15 as bundletool's build-apks --local-testing and install-apks put them there, in
 * Minato as the library lays out an app's kept packs (ADR-0050 decision 2). It logs with the tag pad-test what the
 * library reports, in an order the threads do not change: the app's folders, each pack's state and location before,
 * then per pack what its fetch's task gives, whether the states the listener saw came in their order without an error
 * and the last one, its location and its one asset's text; then each pack's state after, a fetch of a pack the app
 * does not have, the locations of all packs, the modules feature-delivery counts installed, what asking it to install
 * the on-demand feature module "extra" gives (ADR-0050 c2: the request's task, the session's states and the modules
 * installed after, whether the module's class loads), and "done". Which states
 * between the first and the last the listener sees differs from run to run on Android 15 too, so they, and a
 * failure's stack, have the tag pad-test-device and are only shown; a failure logs its class and the library's error
 * code.
 */
public class PadActivity extends Activity {
    static final String TAG = "pad-test";
    static final List<String> PACKS = Arrays.asList("fastfollow", "ondemand");
    /** How long a pack's fetch may take before the test goes on without it. */
    static final long TIMEOUT_MS = 30_000;

    private final Handler mHandler = new Handler(Looper.getMainLooper());
    /** Each pack's states as the listener reported them, status and error code, each once in a row. */
    private final Map<String, List<int[]>> mStates = new HashMap<>();
    private final AssetPackStateUpdateListener mListener =
            state -> mHandler.post(() -> step("onStateUpdate", () -> update(state)));
    private final Runnable mTimeout = () -> step("timeout", this::timeout);
    private AssetPackManager mManager;
    private boolean mStarted;
    private boolean mDone;
    private int mNext;
    /** The pack whose fetch runs, what its task gave, and whether it ended: the listener saw a last state. */
    private String mPack;
    private String mFetched;
    private boolean mEnded;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        // The app's own folders, plain and canonical, beside which the library extracts its packs.
        step("paths", () -> {
            File files = getFilesDir();
            Log.i(TAG, "filesDir " + files + ", canonical " + canonical(files));
            Log.i(TAG, "dataDir " + getDataDir() + ", applicationInfo dataDir " + getApplicationInfo().dataDir);
            File data = new File("/data/data/" + getPackageName());
            Log.i(TAG, "/data/data's folder of the app exists " + data.exists() + ", canonical " + canonical(data));
        });
        step("getInstance", () -> {
            mManager = AssetPackManagerFactory.getInstance(getApplicationContext());
            mManager.registerListener(mListener);
        });
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (mManager != null && !mStarted) {
            mStarted = true;
            // After the resume, so that the app is in the foreground when it asks.
            mHandler.post(() -> step("before", this::before));
        }
    }

    /** Each pack's state and location before any fetch, then the first fetch. */
    private void before() {
        mManager.getPackStates(PACKS).addOnCompleteListener(task -> step("getPackStates", () -> {
            for (String pack : PACKS) {
                Log.i(TAG, pack + " before: " + states(task, pack) + ", location " + location(pack));
            }
            next();
        }));
    }

    private void next() {
        if (mNext < PACKS.size()) {
            fetch(PACKS.get(mNext++));
        } else {
            after();
        }
    }

    private void fetch(String pack) {
        mPack = pack;
        mFetched = null;
        mEnded = false;
        mHandler.postDelayed(mTimeout, TIMEOUT_MS);
        mManager.fetch(Collections.singletonList(pack)).addOnCompleteListener(task -> step("fetch", () -> {
            mFetched = states(task, pack);
            AssetPackState state = task.isSuccessful() ? task.getResult().packStates().get(pack) : null;
            // A refused fetch, or one of a pack in its last state already, has no states to wait for.
            mEnded |= state == null || last(state.status());
            ended();
        }));
    }

    private void update(AssetPackState state) {
        List<int[]> states = mStates.get(state.name());
        if (states == null) {
            states = new ArrayList<>();
            mStates.put(state.name(), states);
        }
        int[] entry = {state.status(), state.errorCode()};
        if (states.isEmpty() || !Arrays.equals(states.get(states.size() - 1), entry)) {
            states.add(entry);
        }
        if (state.name().equals(mPack) && last(state.status())) {
            mEnded = true;
            ended();
        }
    }

    /** Logs the running pack's fetch once its task has given its result and the listener a last state. */
    private void ended() {
        if (mPack == null || mFetched == null || !mEnded) {
            return;
        }
        mHandler.removeCallbacks(mTimeout);
        String pack = mPack;
        mPack = null;
        Log.i(TAG, pack + " fetch: " + mFetched);
        Log.i(TAG, pack + " states: " + order(pack));
        Log.i(TAG + "-device", pack + " states: " + seen(pack));
        Log.i(TAG, pack + " location " + location(pack));
        AssetPackLocation location = mManager.getPackLocation(pack);
        if (location != null && location.assetsPath() != null) {
            File assets = new File(location.assetsPath());
            Log.i(TAG, pack + " assets " + list(assets) + ", " + pack + ".txt: " + text(new File(assets, pack + ".txt")));
        }
        next();
    }

    private void timeout() {
        String pack = mPack;
        mPack = null;
        Log.i(TAG, pack + " timeout: fetch " + mFetched + ", states " + seen(pack));
        next();
    }

    /** Each pack's state after the fetches, a fetch of a pack the app does not have, the locations and the modules. */
    private void after() {
        mManager.getPackStates(PACKS).addOnCompleteListener(task -> step("getPackStates", () -> {
            for (String pack : PACKS) {
                Log.i(TAG, pack + " after: " + states(task, pack));
            }
            mManager.fetch(Collections.singletonList("nopack")).addOnCompleteListener(missing -> step("nopack", () -> {
                Log.i(TAG, "nopack fetch: " + states(missing, "nopack"));
                Log.i(TAG, "locations " + new TreeSet<>(mManager.getPackLocations().keySet()));
                SplitInstallManager modules = SplitInstallManagerFactory.create(getApplicationContext());
                Log.i(TAG, "installed modules " + new TreeSet<>(modules.getInstalledModules()));
                install(modules);
            }));
        }));
    }

    /** The sessions' statuses and error codes the listener saw for the module, each once in a row. */
    private final List<int[]> mModuleStates = new ArrayList<>();
    private SplitInstallManager mModules;
    private boolean mModuleEnded;
    private String mRequest;

    private void install(SplitInstallManager modules) {
        mModules = modules;
        SplitInstallStateUpdatedListener listener = state -> mHandler.post(() -> step("onModuleState", () ->
                moduleUpdate(state)));
        modules.registerListener(listener);
        mHandler.postDelayed(() -> step("moduleTimeout", () -> {
            Log.i(TAG, "extra timeout: request " + mRequest + ", states " + moduleSeen());
            moduleEnd();
        }), TIMEOUT_MS);
        modules.startInstall(SplitInstallRequest.newBuilder().addModule("extra").build())
                .addOnCompleteListener(task -> step("startInstall", () -> {
                    mRequest = task.isSuccessful() ? "started" : "failed, " + moduleFailure(task.getException());
                    moduleEnded();
                }));
    }

    private void moduleUpdate(SplitInstallSessionState state) {
        int[] entry = {state.status(), state.errorCode()};
        if (mModuleStates.isEmpty() || !Arrays.equals(mModuleStates.get(mModuleStates.size() - 1), entry)) {
            mModuleStates.add(entry);
        }
        if (moduleLast(state.status())) {
            mModuleEnded = true;
            moduleEnded();
        }
    }

    /** Logs the module's install once the request's task has given its result and the listener a last status. */
    private void moduleEnded() {
        if (mRequest == null || !mModuleEnded && !mRequest.startsWith("failed")) {
            return;
        }
        moduleEnd();
    }

    private void moduleEnd() {
        if (mDone) {
            return;
        }
        Log.i(TAG, "extra request: " + mRequest);
        Log.i(TAG, "extra states: " + moduleOrder());
        Log.i(TAG + "-device", "extra states: " + moduleSeen());
        Log.i(TAG, "installed modules after " + new TreeSet<>(mModules.getInstalledModules()));
        try {
            Class<?> type = Class.forName("io.github.kisame76.minato.test.pad.extra.ExtraClass");
            Log.i(TAG, "extra class: " + type.getMethod("answer").invoke(null));
        } catch (Throwable t) {
            Log.i(TAG, "extra class: " + t.getClass().getName());
        }
        done();
    }

    private static boolean moduleLast(int status) {
        switch (status) {
            case SplitInstallSessionStatus.INSTALLED:
            case SplitInstallSessionStatus.FAILED:
            case SplitInstallSessionStatus.CANCELED:
            case SplitInstallSessionStatus.REQUIRES_USER_CONFIRMATION:
                return true;
            default:
                return false;
        }
    }

    private String moduleSeen() {
        List<String> seen = new ArrayList<>();
        for (int[] state : mModuleStates) {
            seen.add(moduleStatus(state[0]) + " " + moduleError(state[1]));
        }
        return seen.toString();
    }

    /** As order() does for a pack: whether the statuses came in order, an error among them, and the last one. */
    private String moduleOrder() {
        if (mModuleStates.isEmpty()) {
            return "none";
        }
        boolean inOrder = true;
        boolean errors = false;
        for (int i = 0; i < mModuleStates.size(); i++) {
            inOrder &= i == 0 || mModuleStates.get(i)[0] >= mModuleStates.get(i - 1)[0];
            errors |= mModuleStates.get(i)[1] != SplitInstallErrorCode.NO_ERROR;
        }
        int[] last = mModuleStates.get(mModuleStates.size() - 1);
        return "in order " + inOrder + ", an error " + errors + ", last status " + moduleStatus(last[0])
                + ", errorCode " + moduleError(last[1]);
    }

    private static String moduleFailure(Throwable t) {
        if (t instanceof SplitInstallException) {
            return "SplitInstallException " + moduleError(((SplitInstallException) t).getErrorCode());
        }
        return t == null ? "no exception" : t.getClass().getName();
    }

    private static String moduleStatus(int status) {
        switch (status) {
            case SplitInstallSessionStatus.UNKNOWN: return "UNKNOWN";
            case SplitInstallSessionStatus.PENDING: return "PENDING";
            case SplitInstallSessionStatus.REQUIRES_USER_CONFIRMATION: return "REQUIRES_USER_CONFIRMATION";
            case SplitInstallSessionStatus.DOWNLOADING: return "DOWNLOADING";
            case SplitInstallSessionStatus.DOWNLOADED: return "DOWNLOADED";
            case SplitInstallSessionStatus.INSTALLING: return "INSTALLING";
            case SplitInstallSessionStatus.INSTALLED: return "INSTALLED";
            case SplitInstallSessionStatus.FAILED: return "FAILED";
            case SplitInstallSessionStatus.CANCELING: return "CANCELING";
            case SplitInstallSessionStatus.CANCELED: return "CANCELED";
            default: return String.valueOf(status);
        }
    }

    private static String moduleError(int code) {
        switch (code) {
            case SplitInstallErrorCode.NO_ERROR: return "NO_ERROR";
            case SplitInstallErrorCode.ACTIVE_SESSIONS_LIMIT_EXCEEDED: return "ACTIVE_SESSIONS_LIMIT_EXCEEDED";
            case SplitInstallErrorCode.MODULE_UNAVAILABLE: return "MODULE_UNAVAILABLE";
            case SplitInstallErrorCode.INVALID_REQUEST: return "INVALID_REQUEST";
            case SplitInstallErrorCode.SESSION_NOT_FOUND: return "SESSION_NOT_FOUND";
            case SplitInstallErrorCode.API_NOT_AVAILABLE: return "API_NOT_AVAILABLE";
            case SplitInstallErrorCode.NETWORK_ERROR: return "NETWORK_ERROR";
            case SplitInstallErrorCode.ACCESS_DENIED: return "ACCESS_DENIED";
            case SplitInstallErrorCode.INCOMPATIBLE_WITH_EXISTING_SESSION: return "INCOMPATIBLE_WITH_EXISTING_SESSION";
            case SplitInstallErrorCode.SERVICE_DIED: return "SERVICE_DIED";
            case SplitInstallErrorCode.INSUFFICIENT_STORAGE: return "INSUFFICIENT_STORAGE";
            case SplitInstallErrorCode.SPLITCOMPAT_VERIFICATION_ERROR: return "SPLITCOMPAT_VERIFICATION_ERROR";
            case SplitInstallErrorCode.SPLITCOMPAT_EMULATION_ERROR: return "SPLITCOMPAT_EMULATION_ERROR";
            case SplitInstallErrorCode.SPLITCOMPAT_COPY_ERROR: return "SPLITCOMPAT_COPY_ERROR";
            case SplitInstallErrorCode.PLAY_STORE_NOT_FOUND: return "PLAY_STORE_NOT_FOUND";
            case SplitInstallErrorCode.APP_NOT_OWNED: return "APP_NOT_OWNED";
            case SplitInstallErrorCode.INTERNAL_ERROR: return "INTERNAL_ERROR";
            default: return String.valueOf(code);
        }
    }

    private void done() {
        if (!mDone) {
            mDone = true;
            Log.i(TAG, "done");
        }
    }

    /** Runs a step of the test; a failure logs and ends it, so that a run never waits for a line that cannot come. */
    private void step(String name, Runnable body) {
        if (mDone) {
            return;
        }
        try {
            body.run();
        } catch (Throwable t) {
            Log.i(TAG, name + " failed: " + failure(t));
            done();
        }
    }

    /** The pack's state in a task's AssetPackStates, or why the task failed. */
    private static String states(Task<AssetPackStates> task, String pack) {
        if (!task.isSuccessful()) {
            return "failed, " + failure(task.getException());
        }
        AssetPackState state = task.getResult().packStates().get(pack);
        return state == null ? "no state" : describe(state);
    }

    /** Each state the listener saw for the pack, in its order. */
    private String seen(String pack) {
        List<int[]> states = mStates.get(pack);
        if (states == null) {
            return "none";
        }
        List<String> seen = new ArrayList<>();
        for (int[] state : states) {
            seen.add(status(state[0]) + " " + error(state[1]));
        }
        return seen.toString();
    }

    /**
     * Whether the pack's states came in their order, PENDING to COMPLETED, each later one not before an earlier one,
     * whether any had an error, and the last one: what a run gives alike, as the states between can differ.
     */
    private String order(String pack) {
        List<int[]> states = mStates.get(pack);
        if (states == null) {
            return "none";
        }
        boolean inOrder = true;
        boolean errors = false;
        for (int i = 0; i < states.size(); i++) {
            inOrder &= i == 0 || states.get(i)[0] >= states.get(i - 1)[0];
            errors |= states.get(i)[1] != AssetPackErrorCode.NO_ERROR;
        }
        int[] last = states.get(states.size() - 1);
        return "in order " + inOrder + ", an error " + errors + ", last status " + status(last[0]) + ", errorCode "
                + error(last[1]);
    }

    private String location(String pack) {
        AssetPackLocation location = mManager.getPackLocation(pack);
        if (location == null) {
            return "none";
        }
        return "storage " + storage(location.packStorageMethod()) + ", assetsPath " + location.assetsPath() + ", path "
                + location.path();
    }

    private static String describe(AssetPackState state) {
        return "status " + status(state.status()) + ", errorCode " + error(state.errorCode());
    }

    /** Whether nothing follows this state without the user. */
    private static boolean last(int status) {
        switch (status) {
            case AssetPackStatus.COMPLETED:
            case AssetPackStatus.FAILED:
            case AssetPackStatus.CANCELED:
            case AssetPackStatus.REQUIRES_USER_CONFIRMATION:
            case AssetPackStatus.WAITING_FOR_WIFI:
                return true;
            default:
                return false;
        }
    }

    private static String status(int status) {
        switch (status) {
            case AssetPackStatus.UNKNOWN: return "UNKNOWN";
            case AssetPackStatus.PENDING: return "PENDING";
            case AssetPackStatus.DOWNLOADING: return "DOWNLOADING";
            case AssetPackStatus.TRANSFERRING: return "TRANSFERRING";
            case AssetPackStatus.COMPLETED: return "COMPLETED";
            case AssetPackStatus.FAILED: return "FAILED";
            case AssetPackStatus.CANCELED: return "CANCELED";
            case AssetPackStatus.WAITING_FOR_WIFI: return "WAITING_FOR_WIFI";
            case AssetPackStatus.NOT_INSTALLED: return "NOT_INSTALLED";
            case AssetPackStatus.REQUIRES_USER_CONFIRMATION: return "REQUIRES_USER_CONFIRMATION";
            default: return String.valueOf(status);
        }
    }

    private static String error(int code) {
        switch (code) {
            case AssetPackErrorCode.NO_ERROR: return "NO_ERROR";
            case AssetPackErrorCode.APP_UNAVAILABLE: return "APP_UNAVAILABLE";
            case AssetPackErrorCode.PACK_UNAVAILABLE: return "PACK_UNAVAILABLE";
            case AssetPackErrorCode.INVALID_REQUEST: return "INVALID_REQUEST";
            case AssetPackErrorCode.DOWNLOAD_NOT_FOUND: return "DOWNLOAD_NOT_FOUND";
            case AssetPackErrorCode.API_NOT_AVAILABLE: return "API_NOT_AVAILABLE";
            case AssetPackErrorCode.NETWORK_ERROR: return "NETWORK_ERROR";
            case AssetPackErrorCode.ACCESS_DENIED: return "ACCESS_DENIED";
            case AssetPackErrorCode.INSUFFICIENT_STORAGE: return "INSUFFICIENT_STORAGE";
            // 2.2.2 has no PLAY_STORE_NOT_FOUND (-11) and no NETWORK_UNRESTRICTED (-12), which later versions add.
            case AssetPackErrorCode.APP_NOT_OWNED: return "APP_NOT_OWNED";
            case AssetPackErrorCode.CONFIRMATION_NOT_REQUIRED: return "CONFIRMATION_NOT_REQUIRED";
            case AssetPackErrorCode.UNRECOGNIZED_INSTALLATION: return "UNRECOGNIZED_INSTALLATION";
            case AssetPackErrorCode.INTERNAL_ERROR: return "INTERNAL_ERROR";
            default: return String.valueOf(code);
        }
    }

    private static String storage(int method) {
        switch (method) {
            case AssetPackStorageMethod.STORAGE_FILES: return "STORAGE_FILES";
            case AssetPackStorageMethod.APK_ASSETS: return "APK_ASSETS";
            default: return String.valueOf(method);
        }
    }

    /** A failure's class and, for the library's own, its error code; the stack only with the tag pad-test-device. */
    private static String failure(Throwable t) {
        if (t == null) {
            return "no exception";
        }
        Log.i(TAG + "-device", Log.getStackTraceString(t));
        if (t instanceof AssetPackException) {
            return "AssetPackException " + error(((AssetPackException) t).getErrorCode());
        }
        return t.getClass().getName();
    }

    private static String canonical(File file) {
        try {
            return file.getCanonicalPath();
        } catch (java.io.IOException e) {
            return "unreadable, " + e.getClass().getName();
        }
    }

    private static String list(File folder) {
        String[] names = folder.list();
        if (names == null) {
            return "absent";
        }
        Arrays.sort(names);
        return Arrays.toString(names);
    }

    private static String text(File file) {
        try (BufferedReader reader = new BufferedReader(new FileReader(file))) {
            return reader.readLine();
        } catch (Exception e) {
            return "unreadable, " + e.getClass().getName();
        }
    }
}
