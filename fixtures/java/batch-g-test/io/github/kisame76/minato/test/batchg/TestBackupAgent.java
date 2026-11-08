package io.github.kisame76.minato.test.batchg;

import android.app.backup.BackupAgentHelper;
import android.app.backup.FileBackupHelper;
import android.app.backup.SharedPreferencesBackupHelper;

/** The backup agent of the test: a helper agent with a preferences helper and a file helper, as apps declare. */
public class TestBackupAgent extends BackupAgentHelper {
    @Override
    public void onCreate() {
        addHelper("prefs", new SharedPreferencesBackupHelper(this, "batch-g-prefs"));
        addHelper("files", new FileBackupHelper(this, "batch-g.txt"));
    }
}
