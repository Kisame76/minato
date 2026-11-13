// The guest's libsqlite.so: what SQLite and Android's functions in it answer, one line each
// after "sqlite-native-test: ", which must be fixtures/expected/sqlite-native-test.txt (tools/sqlite-native-check.sh).
// It opens the library with dlopen, as an app with a targetSdk below 24 may, so that the same program runs on Android
// 15's own /system/lib64/libsqlite.so for the expected lines. The build's flags show in the library's version, its
// limits, its modules and functions and its defaults for a new database; ICU in upper, lower, LIKE, REGEXP and the
// collations; Android's functions (external/sqlite/android) in the phone numbers, the collators and _TOKENIZE.
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sqlite3.h>

#define PREFIX "sqlite-native-test: "
#define DATABASE "/data/local/tmp/sqlite-native-test.db"

// The library's functions that this program calls, found with dlsym.
static struct {
    const char *(*libversion)(void);
    int (*libversion_number)(void);
    const char *(*sourceid)(void);
    int (*threadsafe)(void);
    int (*open_v2)(const char *, sqlite3 **, int, const char *);
    int (*close)(sqlite3 *);
    int (*prepare_v2)(sqlite3 *, const char *, int, sqlite3_stmt **, const char **);
    int (*step)(sqlite3_stmt *);
    int (*column_count)(sqlite3_stmt *);
    const unsigned char *(*column_text)(sqlite3_stmt *, int);
    int (*finalize)(sqlite3_stmt *);
    const char *(*errmsg)(sqlite3 *);
    int (*extended_errcode)(sqlite3 *);
    int (*limit)(sqlite3 *, int, int);
    int (*register_android_functions)(sqlite3 *, int);
    int (*register_localized_collators)(sqlite3 *, const char *, int);
} lib;

static int load(void) {
    void *library = dlopen("libsqlite.so", RTLD_NOW);
    if (library == NULL) {
        printf(PREFIX "dlopen: %s\n", dlerror());
        return 0;
    }
    const struct {
        void **function;
        const char *name;
    } symbols[] = {
        {(void **)&lib.libversion, "sqlite3_libversion"},
        {(void **)&lib.libversion_number, "sqlite3_libversion_number"},
        {(void **)&lib.sourceid, "sqlite3_sourceid"},
        {(void **)&lib.threadsafe, "sqlite3_threadsafe"},
        {(void **)&lib.open_v2, "sqlite3_open_v2"},
        {(void **)&lib.close, "sqlite3_close"},
        {(void **)&lib.prepare_v2, "sqlite3_prepare_v2"},
        {(void **)&lib.step, "sqlite3_step"},
        {(void **)&lib.column_count, "sqlite3_column_count"},
        {(void **)&lib.column_text, "sqlite3_column_text"},
        {(void **)&lib.finalize, "sqlite3_finalize"},
        {(void **)&lib.errmsg, "sqlite3_errmsg"},
        {(void **)&lib.extended_errcode, "sqlite3_extended_errcode"},
        {(void **)&lib.limit, "sqlite3_limit"},
        {(void **)&lib.register_android_functions, "register_android_functions"},
        {(void **)&lib.register_localized_collators, "register_localized_collators"},
    };
    for (size_t i = 0; i < sizeof symbols / sizeof symbols[0]; i++) {
        *symbols[i].function = dlsym(library, symbols[i].name);
        if (*symbols[i].function == NULL) {
            printf(PREFIX "dlsym %s: %s\n", symbols[i].name, dlerror());
            return 0;
        }
    }
    return 1;
}

// Runs the statements of `sql` one after the other. With `print`, each row is one line, "<label>: " and its columns
// joined by "|", NULL as "null"; an error ends the run with the line "<label>: error <extended code> <message>".
static void run(sqlite3 *db, const char *label, const char *sql, int print) {
    const char *next = sql;
    while (next != NULL && *next != '\0') {
        sqlite3_stmt *statement = NULL;
        if (lib.prepare_v2(db, next, -1, &statement, &next) != SQLITE_OK) {
            printf(PREFIX "%s: error %d %s\n", label, lib.extended_errcode(db), lib.errmsg(db));
            return;
        }
        if (statement == NULL) continue;  // only white space or a comment was left
        int result;
        while ((result = lib.step(statement)) == SQLITE_ROW) {
            if (!print) continue;
            printf(PREFIX "%s: ", label);
            for (int column = 0; column < lib.column_count(statement); column++) {
                const unsigned char *text = lib.column_text(statement, column);
                printf("%s%s", column > 0 ? "|" : "", text == NULL ? "null" : (const char *)text);
            }
            printf("\n");
        }
        lib.finalize(statement);
        if (result != SQLITE_DONE) {
            printf(PREFIX "%s: error %d %s\n", label, lib.extended_errcode(db), lib.errmsg(db));
            return;
        }
    }
}

static void query(sqlite3 *db, const char *label, const char *sql) { run(db, label, sql, 1); }

// The words that the collations order, each with its own case or accent.
#define WORDS "VALUES ('Zebra'), ('Äpfel'), ('apfel'), ('Apfel'), ('zebra'), ('Ofen'), ('Öl'), ('_x'), ('10'), ('9')"

static void in_memory(void) {
    sqlite3 *db = NULL;
    printf(PREFIX "memory: open %d\n", lib.open_v2(":memory:", &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL));
    static const struct {
        const char *name;
        int id;
    } limits[] = {
        {"length", SQLITE_LIMIT_LENGTH}, {"sql length", SQLITE_LIMIT_SQL_LENGTH}, {"columns", SQLITE_LIMIT_COLUMN},
        {"expression depth", SQLITE_LIMIT_EXPR_DEPTH}, {"compound select", SQLITE_LIMIT_COMPOUND_SELECT},
        {"vdbe ops", SQLITE_LIMIT_VDBE_OP}, {"function args", SQLITE_LIMIT_FUNCTION_ARG},
        {"attached", SQLITE_LIMIT_ATTACHED}, {"like pattern", SQLITE_LIMIT_LIKE_PATTERN_LENGTH},
        {"variables", SQLITE_LIMIT_VARIABLE_NUMBER}, {"trigger depth", SQLITE_LIMIT_TRIGGER_DEPTH},
        {"worker threads", SQLITE_LIMIT_WORKER_THREADS},
    };
    for (size_t i = 0; i < sizeof limits / sizeof limits[0]; i++) {
        printf(PREFIX "limit %s: %d\n", limits[i].name, lib.limit(db, limits[i].id, -1));
    }
    query(db, "modules", "SELECT group_concat(name, ' ') FROM (SELECT name FROM pragma_module_list "
                         "WHERE name NOT LIKE 'pragma\\_%' ESCAPE '\\' ORDER BY name)");
    query(db, "functions", "SELECT count(DISTINCT name) FROM pragma_function_list");
    query(db, "json", "SELECT json_extract('{\"a\":[1,2]}', '$.a[1]')");
    query(db, "math", "SELECT sqrt(16)");
    query(db, "fts5", "CREATE VIRTUAL TABLE f5 USING fts5(x)");
    query(db, "rtree", "CREATE VIRTUAL TABLE r USING rtree(id, x0, x1)");
    query(db, "fts4", "CREATE VIRTUAL TABLE f4 USING fts4(x); INSERT INTO f4 VALUES ('hello world'), ('goodbye'); "
                      "SELECT rowid FROM f4 WHERE f4 MATCH 'hel*'");
    query(db, "bytecode", "SELECT count(*) > 0 FROM bytecode('SELECT 1')");
    // ICU's functions and LIKE (SQLITE_ENABLE_ICU), which SQLite's own would answer STRAßE, ÄÖÜ and 0.
    query(db, "icu", "SELECT upper('straße'), lower('ÄÖÜ'), 'Ä' LIKE 'ä', 'abc' REGEXP 'b'");
    query(db, "icu collation", "SELECT icu_load_collation('de_DE', 'german')");
    query(db, "german order", "SELECT group_concat(x, ' ') FROM (SELECT column1 AS x FROM (" WORDS ") "
                              "ORDER BY x COLLATE german)");
    query(db, "binary order", "SELECT group_concat(x, ' ') FROM (SELECT column1 AS x FROM (" WORDS ") ORDER BY x)");
    printf(PREFIX "android functions: %d\n", lib.register_android_functions(db, 0));
    query(db, "phone numbers",
          "SELECT PHONE_NUMBERS_EQUAL('+1 650-555-1234', '650 555 1234'), "
          "PHONE_NUMBERS_EQUAL('+1 650-555-1234', '650 555 1234', 1), "
          "PHONE_NUMBERS_EQUAL('011 81 3 1234 5678', '+81 3 1234 5678', 0), "
          "PHONE_NUMBERS_EQUAL('1234567', '7654321'), _PHONE_NUMBER_STRIPPED_REVERSED('+1 (650) 555-1234')");
    query(db, "unicode order", "SELECT group_concat(x, ' ') FROM (SELECT column1 AS x FROM (" WORDS ") "
                               "ORDER BY x COLLATE UNICODE)");
    printf(PREFIX "localized collators: %d\n", lib.register_localized_collators(db, "en_US", 0));
    query(db, "localized order", "SELECT group_concat(x, ' ') FROM (SELECT column1 AS x FROM (" WORDS ") "
                                 "ORDER BY x COLLATE LOCALIZED)");
    query(db, "phonebook order", "SELECT group_concat(x, ' ') FROM (SELECT column1 AS x FROM (" WORDS ") "
                                 "ORDER BY x COLLATE PHONEBOOK)");
    query(db, "tokenize", "CREATE TABLE tokens (token TEXT, source INTEGER, token_index INTEGER); "
                          "SELECT _TOKENIZE('tokens', 7, 'Hello Wörld zebra', ' ', 1)");
    query(db, "token", "SELECT token, source, token_index FROM tokens ORDER BY token_index");
    printf(PREFIX "memory: close %d\n", lib.close(db));
}

static void in_file(void) {
    static const char *const files[] = {DATABASE, DATABASE "-journal", DATABASE "-wal", DATABASE "-shm"};
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) unlink(files[i]);
    sqlite3 *db = NULL;
    printf(PREFIX "file: open %d\n", lib.open_v2(DATABASE, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL));
    // A new database's defaults, which the build's flags set (sqlite-defaults in dist/Android.bp).
    static const char *const pragmas[] = {
        "page_size", "auto_vacuum", "journal_mode", "journal_size_limit", "secure_delete", "legacy_alter_table",
        "synchronous", "cache_size", "temp_store", "locking_mode", "encoding", "mmap_size", "foreign_keys",
        "automatic_index", "wal_autocheckpoint", "max_page_count", "user_version", "trusted_schema",
    };
    for (size_t i = 0; i < sizeof pragmas / sizeof pragmas[0]; i++) {
        char label[64], sql[64];
        snprintf(label, sizeof label, "pragma %s", pragmas[i]);
        snprintf(sql, sizeof sql, "PRAGMA %s", pragmas[i]);
        query(db, label, sql);
    }
    query(db, "table", "CREATE TABLE t (a INTEGER PRIMARY KEY, b TEXT UNIQUE); "
                       "INSERT INTO t (b) VALUES ('one'), ('two'); SELECT a, b FROM t ORDER BY a");
    query(db, "unique", "INSERT INTO t (b) VALUES ('one')");
    query(db, "not null", "CREATE TABLE n (x NOT NULL); INSERT INTO n VALUES (NULL)");
    query(db, "view rowid", "CREATE VIEW v AS SELECT b FROM t; SELECT count(rowid) FROM v");
    query(db, "alter", "ALTER TABLE t RENAME TO u; SELECT sql FROM sqlite_schema WHERE name = 'v'");
    query(db, "dbstat", "SELECT count(*) > 0 FROM dbstat");
    query(db, "wal", "PRAGMA journal_mode = WAL");
    query(db, "wal insert", "INSERT INTO u (b) VALUES ('three'); SELECT count(*) FROM u");
    query(db, "checkpoint", "PRAGMA wal_checkpoint(TRUNCATE)");
    query(db, "delete mode", "PRAGMA journal_mode = DELETE");
    query(db, "rollback", "BEGIN; INSERT INTO u (b) VALUES ('four'); ROLLBACK; SELECT count(*) FROM u");
    query(db, "integrity", "PRAGMA integrity_check");
    printf(PREFIX "file: close %d\n", lib.close(db));
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        printf(PREFIX "left %s: %s\n", files[i] + strlen("/data/local/tmp/"),
               access(files[i], F_OK) == 0 ? "yes" : "no");
        unlink(files[i]);
    }
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (!load()) return 1;
    printf(PREFIX "library: %s %d\n", lib.libversion(), lib.libversion_number());
    printf(PREFIX "source: %s\n", lib.sourceid());
    printf(PREFIX "threadsafe: %d\n", lib.threadsafe());
    in_memory();
    in_file();
    printf(PREFIX "done\n");
    return 0;
}
