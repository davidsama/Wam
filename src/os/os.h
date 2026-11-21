// os.h - thin platform layer. Implemented in os_win32.c.
#ifndef OS_H
#define OS_H

static void *os_reserve(U64 size);
static B32   os_commit(void *ptr, U64 size);
static void  os_release(void *ptr, U64 size);

static String16 os_str16_from_8(Arena *a, String8 s);   // null-terminated
static String8  os_str8_from_16(Arena *a, U16 *s, U64 len);

typedef struct OS_FileInfo { String8 name; B32 is_dir; B32 is_link; U64 size; U64 mtime; } OS_FileInfo; // mtime: unix seconds
typedef struct OS_FileInfoArray { OS_FileInfo *v; U64 count; } OS_FileInfoArray;

static String8          os_read_file(Arena *a, String8 path);  // empty on failure
static String8          os_read_file_range(Arena *a, String8 path, U64 offset, U64 size); // may return less at EOF
static String8          os_downloads_dir(Arena *a);           // the user's Downloads known folder
static B32              os_write_file(String8 path, String8 data);
static B32              os_file_exists(String8 path);
static B32              os_dir_exists(String8 path);
static OS_FileInfoArray os_list_dir(Arena *a, String8 path);
static String8          os_get_env(Arena *a, char *name);
static B32              os_make_dirs(String8 path);       // like mkdir -p
static B32              os_move(String8 from, String8 to); // same volume, files or dirs
static B32              os_replace_file(String8 from, String8 to); // atomic overwrite
static B32              os_delete_tree(String8 path);     // never follows junctions/symlinks
static B32              os_recycle(String8 path);         // send to Recycle Bin
static B32              os_is_link(String8 path);         // junction or symlink
static B32              os_file_version(String8 exe, U32 out[4]);
static B32              os_tree_has_links(String8 path);  // any junction/symlink inside?
static String8          os_system_dir(Arena *a);          // C:\Windows\System32
static String8List      os_installed_program_dirs(Arena *a, char *name_prefix); // InstallLocation from Windows' uninstall list
static String8          os_protect(Arena *a, String8 data);   // DPAPI: only this Windows user can unprotect it
static String8          os_unprotect(Arena *a, String8 blob); // empty on failure

// Runs `exe` with a full command line (exe path quoted first), no window, stdin = NUL.
// Captures stdout+stderr. Kills it after timeout_ms. Returns 0 if it couldn't start or timed out.
typedef struct OS_RunResult { B32 started, timed_out; U32 exit_code; String8 output; } OS_RunResult;
static OS_RunResult     os_run(Arena *a, String8 exe, String8 cmdline, U32 timeout_ms);
static U64              os_now_us(void);

typedef U32 OS_ThreadFunc(void *param);
static void os_thread_start(OS_ThreadFunc *fn, void *param);

// Runs fn(user, i) for i in [0, count) on up to 8 threads and waits for all of them.
typedef void OS_ParallelFunc(void *user, U64 index);
static void os_parallel_for(U64 count, OS_ParallelFunc *fn, void *user);

#endif
