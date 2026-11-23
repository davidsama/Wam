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

//- Threads talking to each other

// A lock (not re-entrant). Zero is unlocked: no setup needed.
typedef struct OS_Mutex { void *v; } OS_Mutex;
static void os_mutex_lock(OS_Mutex *m);
static void os_mutex_unlock(OS_Mutex *m);
// A counting semaphore: each signal lets one wait through.
typedef struct OS_Semaphore { void *h; } OS_Semaphore;
static OS_Semaphore os_semaphore_alloc(U32 max_count);
static void os_semaphore_signal(OS_Semaphore s);
static void os_semaphore_wait(OS_Semaphore s);
// An auto-reset event: signals while nobody waits collapse into one, a wait takes it.
typedef struct OS_Event { void *h; } OS_Event;
static OS_Event os_event_alloc(void);
static void os_event_signal(OS_Event e);
static void os_event_wait(OS_Event e);
static S32 os_atomic_inc(volatile S32 *p);               // returns the new value
static void os_atomic_store(volatile S32 *p, S32 v);
// Memory that outlives an arena: messages handed from one thread to another.
static void *os_alloc(U64 size);                         // zeroed; 0 on failure
static void  os_free(void *p);
static void  os_secure_zero(void *p, U64 size);          // not optimized away (it held a secret)

//- Time

static U64 os_now_ms(void);                              // monotonic, like os_now_us
typedef struct OS_DateTime { U32 year, month, day, hour, minute, second; } OS_DateTime;
static OS_DateTime os_local_now(void);
static OS_DateTime os_utc_from_unix(S64 unix_secs);     // year 0 if it can't be represented

//- More files

static B32 os_append_file(String8 path, String8 data);  // creates it if needed
static U64 os_file_size(String8 path);                   // 0 if it doesn't exist
static B32 os_delete_file(String8 path);
// Waits for changes in any of dirs (not their subfolders): file names, sizes, writes.
typedef struct OS_DirWatch { void *h[32]; U32 count; } OS_DirWatch;
static OS_DirWatch os_dir_watch_open(String8List dirs);
static B32 os_dir_watch_wait(OS_DirWatch *w, U32 timeout_ms); // 1: something changed, 0: timed out (or nothing watched)
// The bytes of the program's own RCDATA resource `name` (they stay mapped: no copy).
static String8 os_resource(String8 name);

//- The desktop

// Opens a file, URL or program on a short-lived thread (it can pump messages for a while).
static void os_shell_open_async(String8 file, String8 args);
static void os_open_url(String8 url);                    // https:// only
static String8 os_open_log;                              // testing: when set, opens are appended here instead
static String8 os_clipboard_text(Arena *a);
// Decodes a picture (PNG, JPEG, GIF, BMP, WebP if installed...) to BGRA with straight alpha:
// scaled to exactly w x h, cropped to fill, or with fit scaled down (never up) to fit inside
// w x h. Pixels are pushed on a; empty (w 0) if it can't.
typedef struct OS_Image { U32 w, h; U8 *pixels; B32 animated; } OS_Image; // animated: a GIF with more pictures (see OS_Gif)
static OS_Image os_image_decode(Arena *a, String8 data, U32 w, U32 h, B32 fit);

// An animated GIF, one frame after the other. It lives in a, as long as data does; use it on the thread
// that opened it. os_gif_next goes on to the next frame (the first one, the first time; from the last one
// back to the first) and says how long it's shown. 0: it can't be read. os_gif_pixels is the frame as a whole
// picture, scaled to w x h (pushed on out).
typedef struct OS_Gif { U32 w, h, frames; void *decoder, *stream; U32 at; B32 started; U8 *canvas, *saved; U32 disposal; S32 x0, y0, x1, y1; Arena *arena; } OS_Gif;
static OS_Gif *os_gif_open(Arena *a, String8 data); // 0 if it isn't an animated GIF (or it's too big)
static B32 os_gif_next(OS_Gif *g, U32 *delay_ms);
static OS_Image os_gif_pixels(Arena *out, OS_Gif *g, U32 w, U32 h);
static void os_gif_close(OS_Gif *g);
static B32 os_animations_on(void);   // Windows' "Animation effects" setting

#endif
