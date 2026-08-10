#pragma once

/*
 * Yocto supplies sqlite3.h through the sqlite3 development sysroot.  The
 * desktop SDK used by some developers ships the runtime library without the
 * header, so keep the small ABI surface used by our RAII wrapper available in
 * that environment as well.  No SQLite implementation is duplicated here.
 */
#if __has_include(<sqlite3.h>)
#include <sqlite3.h>
#else
extern "C" {
struct sqlite3;
struct sqlite3_stmt;
using sqlite3_int64 = long long;
int sqlite3_open_v2(const char *, sqlite3 **, int, const char *);
int sqlite3_close_v2(sqlite3 *);
const char *sqlite3_errmsg(sqlite3 *);
int sqlite3_exec(sqlite3 *, const char *, int (*)(void *, int, char **, char **),
		 void *, char **);
void sqlite3_free(void *);
int sqlite3_prepare_v2(sqlite3 *, const char *, int, sqlite3_stmt **,
		       const char **);
int sqlite3_finalize(sqlite3_stmt *);
int sqlite3_reset(sqlite3_stmt *);
int sqlite3_clear_bindings(sqlite3_stmt *);
int sqlite3_step(sqlite3_stmt *);
int sqlite3_bind_int(sqlite3_stmt *, int, int);
int sqlite3_bind_int64(sqlite3_stmt *, int, sqlite3_int64);
int sqlite3_bind_text(sqlite3_stmt *, int, const char *, int,
		      void (*)(void *));
int sqlite3_bind_blob(sqlite3_stmt *, int, const void *, int,
		      void (*)(void *));
int sqlite3_bind_null(sqlite3_stmt *, int);
int sqlite3_column_int(sqlite3_stmt *, int);
sqlite3_int64 sqlite3_column_int64(sqlite3_stmt *, int);
const unsigned char *sqlite3_column_text(sqlite3_stmt *, int);
const void *sqlite3_column_blob(sqlite3_stmt *, int);
int sqlite3_column_bytes(sqlite3_stmt *, int);
int sqlite3_busy_timeout(sqlite3 *, int);
sqlite3_int64 sqlite3_last_insert_rowid(sqlite3 *);
int sqlite3_changes(sqlite3 *);
}

inline constexpr int SQLITE_OK = 0;
inline constexpr int SQLITE_ROW = 100;
inline constexpr int SQLITE_DONE = 101;
inline constexpr int SQLITE_OPEN_READWRITE = 0x00000002;
inline constexpr int SQLITE_OPEN_CREATE = 0x00000004;
inline constexpr int SQLITE_OPEN_FULLMUTEX = 0x00010000;
#define SQLITE_TRANSIENT reinterpret_cast<void (*)(void *)>(-1)
#endif

