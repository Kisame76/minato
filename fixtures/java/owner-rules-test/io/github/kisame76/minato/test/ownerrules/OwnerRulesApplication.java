package io.github.kisame76.minato.test.ownerrules;

import android.app.Application;
import android.content.Context;
import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;
import android.util.Log;
import java.io.File;

/**
 * ADR-0039 decision 3's test of the owner rules for the calls that change a file or a folder's entries: chmod, chown,
 * the times, opening for writing, truncate, link, symlink, mkdir, creating, rename and unlink. On the app's own APK
 * and code folder, which belong to system, Android refuses them; in its data, cache and external files folders, which
 * are its own, they work. On the system's read-only partitions, /system and an APEX, they answer EROFS (ADR-0052's
 * later step). From Application.attachBaseContext it logs each call's answer, "ok" or the errno's name,
 * with the tag owner-rules-test, and ends with "done"; tools/android-reference.sh compares it with Android 15's log.
 * The APK's truncate comes last, after which a runtime that let it through could not read the APK any more. Only
 * public API, as an app has it on Android; no path is logged, as the code folder's name differs per install.
 */
public class OwnerRulesApplication extends Application {
    private static final String TAG = "owner-rules-test";

    @Override
    protected void attachBaseContext(Context base) {
        super.attachBaseContext(base);
        try {
            report(base);
        } catch (Throwable t) {
            Log.i(TAG, "failed: " + t);
            // Its stack, which differs by device, is only shown.
            Log.i(TAG + "-device", Log.getStackTraceString(t));
        }
        Log.i(TAG, "done");
    }

    interface Call {
        void run() throws Exception;
    }

    /** Logs `what` with the call's answer: "ok", the errno's name, or the exception's class. */
    static void log(String what, Call call) {
        String answer;
        try {
            call.run();
            answer = "ok";
        } catch (ErrnoException e) {
            answer = OsConstants.errnoName(e.errno);
        } catch (Exception e) {
            answer = e.getClass().getSimpleName();
        }
        Log.i(TAG, what + ": " + answer);
    }

    /** What an earlier run left, gone, whatever the answer. */
    static void clear(String... paths) {
        for (String path : paths) {
            try {
                Os.remove(path);
            } catch (ErrnoException e) {
                // not there
            }
        }
    }

    static void create(String path) throws ErrnoException {
        Os.close(Os.open(path, OsConstants.O_CREAT | OsConstants.O_WRONLY | OsConstants.O_TRUNC, 0600));
    }

    /** open with O_TRUNC, as android.system.Os has no truncate for apps. */
    static void truncate(String path) throws ErrnoException {
        Os.close(Os.open(path, OsConstants.O_WRONLY | OsConstants.O_TRUNC, 0));
    }

    /** The times through java.io.File, which answers only true or false. */
    static void touch(String path) throws Exception {
        if (!new File(path).setLastModified(1_000_000_000_000L)) throw new IllegalStateException("refused");
    }

    static int mode(String path) throws ErrnoException {
        return Os.stat(path).st_mode & 07777;
    }

    /** Only shown, as it differs by device: a file's owner, group and mode, or a kernel setting's value. */
    static void show(String what, Call call) {
        try {
            call.run();
        } catch (Exception e) {
            Log.i(TAG + "-device", what + ": " + e);
        }
    }

    static void showOwner(String what, String path) {
        show(what, () -> {
            android.system.StructStat status = Os.stat(path);
            Log.i(TAG + "-device", what + ": " + status.st_uid + ":" + status.st_gid + " "
                    + Integer.toOctalString(status.st_mode & 07777));
        });
    }

    static void showSetting(String path) {
        show(path, () -> {
            try (java.io.BufferedReader reader = new java.io.BufferedReader(new java.io.FileReader(path))) {
                Log.i(TAG + "-device", path + ": " + reader.readLine());
            }
        });
    }

    static void report(Context context) throws Exception {
        String apk = context.getApplicationInfo().sourceDir;
        String code = new File(apk).getParent();
        String files = context.getFilesDir().getPath();
        String data = new File(files).getParent();
        String cache = context.getCacheDir().getPath();
        String external = context.getExternalFilesDir(null).getPath();
        clear(files + "/own.txt", files + "/own-link.txt", files + "/own-symlink", files + "/own-moved.txt",
                files + "/folder", files + "/linked.apk", files + "/moved.apk", cache + "/own.txt",
                cache + "/own-link.txt", cache + "/own-moved.txt", external + "/own.txt", external + "/own-link.txt",
                external + "/own-moved.txt", external + "/folder");
        showOwner("base.apk", apk);
        showOwner("the code folder", code);
        showOwner("the data folder", data);
        showOwner("the files folder", files);
        showOwner("the cache folder", cache);
        showOwner("the external files folder", external);
        showSetting("/proc/sys/fs/protected_hardlinks");
        showSetting("/proc/sys/fs/protected_symlinks");
        // /data's own folders and those between them and the app's (ADR-0052 with decision 3).
        for (String folder : new String[] {"/data", "/data/user", "/data/user/0", "/data/data", "/data/app",
                "/data/dalvik-cache", "/data/dalvik-cache/arm64", "/data/local/tmp"}) {
            showOwner(folder, folder);
        }
        showOwner("the ~~ folder", new File(code).getParent());

        // The code, system's: its APK and its folder.
        log("code chmod base.apk", () -> Os.chmod(apk, 0644));
        log("code chown base.apk", () -> Os.chown(apk, Os.getuid(), Os.getgid()));
        log("code times of base.apk", () -> touch(apk));
        log("code open base.apk for writing", () -> Os.close(Os.open(apk, OsConstants.O_WRONLY, 0)));
        log("code link base.apk into files", () -> Os.link(apk, files + "/linked.apk"));
        log("code symlink in the code folder", () -> Os.symlink(apk, code + "/symlink"));
        log("code mkdir in the code folder", () -> Os.mkdir(code + "/folder", 0700));
        log("code create in the code folder", () -> create(code + "/new"));
        log("code chmod the code folder", () -> Os.chmod(code, mode(code)));
        log("code rename base.apk in the code folder", () -> Os.rename(apk, code + "/moved.apk"));
        log("code rename base.apk into files", () -> Os.rename(apk, files + "/moved.apk"));
        log("code unlink base.apk", () -> Os.remove(apk));
        log("code truncate base.apk", () -> truncate(apk));

        // The app's data folder and its files folder, the app's own.
        log("data chmod the data folder", () -> Os.chmod(data, mode(data)));
        String own = files + "/own.txt";
        log("files create", () -> create(own));
        log("files chmod", () -> Os.chmod(own, 0640));
        log("files chown", () -> Os.chown(own, Os.getuid(), Os.getgid()));
        log("files times", () -> touch(own));
        log("files open for writing", () -> Os.close(Os.open(own, OsConstants.O_RDWR, 0)));
        log("files truncate", () -> truncate(own));
        log("files link", () -> Os.link(own, files + "/own-link.txt"));
        log("files symlink", () -> Os.symlink(own, files + "/own-symlink"));
        log("files mkdir", () -> Os.mkdir(files + "/folder", 0700));
        log("files rename", () -> Os.rename(own, files + "/own-moved.txt"));
        log("files unlink", () -> Os.remove(files + "/own-moved.txt"));
        log("files rmdir", () -> Os.remove(files + "/folder"));
        clear(files + "/own-link.txt", files + "/own-symlink");

        // The cache folder, the app's own.
        String cached = cache + "/own.txt";
        log("cache create", () -> create(cached));
        log("cache chmod", () -> Os.chmod(cached, 0640));
        log("cache link", () -> Os.link(cached, cache + "/own-link.txt"));
        log("cache rename", () -> Os.rename(cached, cache + "/own-moved.txt"));
        log("cache unlink", () -> Os.remove(cache + "/own-moved.txt"));
        clear(cache + "/own-link.txt");

        // The external files folder, the app's own on shared storage.
        String outside = external + "/own.txt";
        log("external create", () -> create(outside));
        showOwner("an external file", outside);
        log("external chmod", () -> Os.chmod(outside, 0640));
        log("external open for writing", () -> Os.close(Os.open(outside, OsConstants.O_RDWR, 0)));
        log("external link", () -> Os.link(outside, external + "/own-link.txt"));
        log("external mkdir", () -> Os.mkdir(external + "/folder", 0700));
        log("external rename", () -> Os.rename(outside, external + "/own-moved.txt"));
        log("external unlink", () -> Os.remove(external + "/own-moved.txt"));
        log("external rmdir", () -> Os.remove(external + "/folder"));
        clear(external + "/own-link.txt");

        // The system's read-only partitions (ADR-0052's later step): every change answers EROFS, after EEXIST for a
        // name that is there and after EXDEV across mounts. No call here could harm a file if it went through: none
        // writes, truncates or removes an existing one, a removal or move in the folder names a missing file (Linux
        // checks the mount before it looks the name up), and the move into files is EXDEV before any host call.
        readOnly("system", "/system/etc", "/system/etc/public.libraries.txt", files);
        readOnly("apex", "/apex/com.android.art/javalib", "/apex/com.android.art/javalib/core-oj.jar", files);
    }

    /** The calls that change something, on `file` and in `folder` of a read-only partition. */
    static void readOnly(String name, String folder, String file, String files) {
        clear(files + "/" + name + "-linked", files + "/" + name + "-moved");
        log(name + " open a file for writing", () -> Os.close(Os.open(file, OsConstants.O_WRONLY, 0)));
        log(name + " create a file", () -> create(folder + "/minato-new.txt"));
        log(name + " create an existing file exclusively",
                () -> Os.close(Os.open(file, OsConstants.O_CREAT | OsConstants.O_EXCL | OsConstants.O_WRONLY, 0600)));
        log(name + " access a file for writing", () -> Os.access(file, OsConstants.W_OK));
        log(name + " chmod a file", () -> Os.chmod(file, mode(file)));
        log(name + " chown a file", () -> Os.chown(file, Os.getuid(), Os.getgid()));
        log(name + " fchmod an open file", () -> {
            java.io.FileDescriptor descriptor = Os.open(file, OsConstants.O_RDONLY, 0);
            try {
                Os.fchmod(descriptor, mode(file));
            } finally {
                Os.close(descriptor);
            }
        });
        log(name + " fchown an open file", () -> {
            java.io.FileDescriptor descriptor = Os.open(file, OsConstants.O_RDONLY, 0);
            try {
                Os.fchown(descriptor, Os.getuid(), Os.getgid());
            } finally {
                Os.close(descriptor);
            }
        });
        log(name + " times of a file", () -> touch(file));
        log(name + " mkdir a new folder", () -> Os.mkdir(folder + "/minato-new", 0700));
        log(name + " mkdir an existing folder", () -> Os.mkdir(folder, 0700));
        log(name + " symlink a new name", () -> Os.symlink(file, folder + "/minato-symlink"));
        log(name + " symlink an existing name", () -> Os.symlink(file, file));
        log(name + " link in its folder", () -> Os.link(file, folder + "/minato-link"));
        log(name + " link into files", () -> Os.link(file, files + "/" + name + "-linked"));
        log(name + " rename a missing file in its folder",
                () -> Os.rename(folder + "/minato-missing", folder + "/minato-moved"));
        log(name + " rename a file into files", () -> Os.rename(file, files + "/" + name + "-moved"));
        log(name + " unlink a missing file", () -> Os.remove(folder + "/minato-missing"));
        log(name + " rmdir its folder", () -> Os.remove(folder));
        log(name + " statvfs read-only", () -> {
            if ((Os.statvfs(folder).f_flag & OsConstants.ST_RDONLY) == 0) throw new IllegalStateException("writable");
        });
        clear(files + "/" + name + "-linked", files + "/" + name + "-moved");
    }
}
