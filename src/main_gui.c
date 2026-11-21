// main_gui.c - wam.exe, unity build entry point for the GUI.
//
// Layers, bottom up:
//   base/, os/        types, arenas, strings, JSON; the Win32 platform layer
//   core/             addon management
//   render/, font/    D3D11 rect renderer, GDI glyph atlas
//   ui/               a reusable immediate-mode UI library: widgets (ui.c) and the window (ui_win32.c)
//   gui/              wam's GUI on top of it: state, background tasks, views, window composition
//   main_gui.c        keyboard handling, worker results, and startup
//
// Event-driven: the window renders only when input, a resize, or a finished background
// task asks for it (or while something animates). Idle = 0% CPU.
//
// Threading: one worker thread runs disk/network operations (sync, install, update, remove,
// search) from a queue; lanes run the quick ones (listings, suggestions, the Downloads scan) on
// their own threads. Each task gets its own arena and posts back an immutable snapshot; the UI
// thread only ever reads snapshots.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define COBJMACROS
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <winhttp.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <math.h>

#include "base/base.h"
#include "version.h"        // WAM_VERSION
#include "os/os.h"
#include "core/addons.h"

#include "base/base.c"
#include "base/json.c"
#include "os/os_win32.c"
#include "core/fingerprint.c"
#include "core/addons.c"
#include "core/zip.c"
#include "core/http.c"
#include "core/curseforge.c"
#include "core/sources.c"
#include "core/wago.c"
#include "core/fuzzy.c"
#include "core/manager.c"
#include "core/markup.c"
#include "core/catalog.c"
#include "core/downloads.c"
#include "render/render_d3d11.c"
#ifdef WAM_FONT_GDI
#include "font/font_gdi.c"      // the old GDI text backend: compile with /DWAM_FONT_GDI (font_dwrite.obj not needed)
#else
#include "font/font_dwrite.c"   // DirectWrite text, with font_dwrite.cpp linked in
#endif
#include "ui/ui.h"
#include "ui/ui.c"
#include "ui/ui_image.c"     // pictures from URLs, loaded in the background
#include "ui/ui_win32.c"
#include "ui/ui_webview.c"   // optional: the CurseForge download window (WebView2)

#define WM_APP_SNAPSHOT  (WM_APP + 1)
#define WM_APP_SEARCH    (WM_APP + 2)
#define WM_APP_STATUS    (WM_APP + 3)
#define WM_APP_TASK_DONE (WM_APP + 4)
#define WM_APP_DOWNLOADS (WM_APP + 5)
#define WM_APP_REFRESH_DONE (WM_APP + 6) // wParam: the listing request it answered
#define WM_APP_SUGGEST     (WM_APP + 7) // lParam: SearchSnap of a typed search or a page
#define WM_APP_SUGGEST_DONE (WM_APP + 8) // wParam: the suggestion request it answered
#define WM_APP_DOWNLOADS_DONE (WM_APP + 9) // unused: WM_APP_DOWNLOADS carries the result
#define WM_APP_DOWNLOADS_CHANGED (WM_APP + 10) // a download folder changed (from the watcher thread)
#define WM_APP_PENDING   (WM_APP + 11) // lParam: PendingList (heap) of CurseForge files to fetch through their page
#define WM_APP_CATALOG   (WM_APP + 12) // lParam: CatalogSnap (from the disk cache or the network)
#define WM_APP_CATALOG_DONE (WM_APP + 13) // wParam: the catalog request it answered
#define WM_APP_DETAILS   (WM_APP + 14) // lParam: DetailSnap
#define WM_APP_DETAILS_DONE (WM_APP + 15) // wParam: the details request it answered
#define WM_APP_CHANGES   (WM_APP + 16) // lParam: ChangesSnap
#define WM_APP_CHANGES_DONE (WM_APP + 17) // (unused: WM_APP_CHANGES carries the result)
#define FLAVOR_COUNT ((int)ArrayCount(wow_flavors))


#include "gui/state.c"
#include "gui/tasks.c"
#include "gui/feedback.c"   // what the user asked for shows at once
#include "gui/views.c"
#include "gui/settings.c"   // the settings popup
#include "gui/changes.c"    // the changes popup (what an update would bring)
#include "gui/frame.c"
#include "gui/session.c"

//- CurseForge files fetched through their own download page

// The browser saves "Name.zip", or "Name (1).zip" when that already exists.
static B32 download_name_matches(String8 want, String8 got)
{
    if (str8_match(want, got, 1)) return 1;
    String8 ext = path_ext(want), stem = str8_chop(want, ext.size);
    if (!str8_starts_with(got, stem, 1) || !str8_ends_with(got, ext, 1)) return 0;
    String8 mid = str8_substr(got, stem.size, got.size - ext.size); // " (1)"
    if (mid.size < 4 || mid.str[0] != ' ' || mid.str[1] != '(' || mid.str[mid.size - 1] != ')') return 0;
    for (U64 i = 2; i + 1 < mid.size; i += 1) if (mid.str[i] < '0' || mid.str[i] > '9') return 0;
    return 1;
}

static void waiting_remove(U32 i)
{
    app.waiting[i] = app.waiting[--app.waiting_count];
}

// The install task for an archive a waiting file was expecting (from the download window's temp
// folder, which the task then deletes, or from a download folder).
static void waiting_install(PendingDownload *p, String8 path, B32 delete_after)
{
    String8 task = str8_pushf(ui.frame_arena, "%.*s\n%s\n%lld\n%s\n%s\n%s\n%s", S8F(path), p->id, p->file_id, p->name,
                              p->version[0] ? p->version : "-", p->website[0] ? p->website : "-", delete_after ? "delete" : "keep");
    U32 id = push_task_for(TASK_INSTALL_DOWNLOADED, task, 0, p->flavor);
    if (p->flavor == app.flavor) feedback_install(id, str8_cstring(p->id), str8_cstring(p->name), str8(0, 0));
}

static void waiting_fetch_next(void);

// The download window finished (or was closed).
static char web_fetching[64];   // id of the waiting file the download window is fetching
static void waiting_web_done(void *user, String8 path, B32 ok, String8 error)
{
    (void)user; (void)error;
    for (U32 i = 0; i < app.waiting_count; i += 1) {
        PendingDownload *p = &app.waiting[i];
        if (strcmp(p->id, web_fetching) != 0) continue;
        if (ok) {
            worker_set_status(str8_pushf(ui.frame_arena, "Installing %s from CurseForge...", p->name), 0);
            waiting_install(p, path, 1);
            app.waiting[i] = app.waiting[--app.waiting_count];
        } else {
            // Closed (or failed) before the file arrived: it stays waiting, so a download made by
            // hand into a download folder is still picked up.
            p->queued = 0;
            worker_set_status(str8_pushf(ui.frame_arena, "%s wasn't downloaded. Update it again, or download it from CurseForge and drop it onto wam.", p->name), 1);
        }
        break;
    }
    web_fetching[0] = 0;
    waiting_fetch_next();
}

// Fetches one waiting file: in wam's download window (WebView2) when there is one, which shows
// CurseForge's page, catches the download and closes; otherwise in the default browser, after which
// the download-folder watcher picks the archive up.
static void waiting_fetch(PendingDownload *p)
{
    p->opened = 1;
    p->since_ms = GetTickCount64();
    if (web_ok && !ui_web_busy()) {
        String8 dir = path_join(ui.frame_arena, os_get_env(ui.frame_arena, "TEMP"), str8_lit("wam-cf"));
        os_make_dirs(dir);
        String8 title = str8_pushf(ui.frame_arena, "Downloading %s from CurseForge", p->name);
        String8 note = str8_pushf(ui.frame_arena, "%s's author lets only CurseForge's own website serve its files, so wam fetches it through "
                                  "this page. The download starts by itself in a few seconds; this window then closes and wam installs it. "
                                  "Close the window to cancel.", p->name);
        memcpy(web_fetching, p->id, sizeof(web_fetching));
        if (ui_web_download(str8_cstring(p->url), title, note, dir, waiting_web_done, 0)) return;
        web_fetching[0] = 0;
    }
    ui_open_url(str8_cstring(p->url));
}

// One window at a time: the next queued file when the window is free.
static void waiting_fetch_next(void)
{
    if (web_ok && ui_web_busy()) return;
    for (U32 i = 0; i < app.waiting_count; i += 1) {
        if (app.waiting[i].queued && !app.waiting[i].opened) { waiting_fetch(&app.waiting[i]); return; }
    }
}

// Fetches every waiting file that hasn't been yet (the status bar's "Get N from CurseForge").
static void waiting_open_all(void)
{
    U32 n = 0;
    for (U32 i = 0; i < app.waiting_count; i += 1) {
        PendingDownload *p = &app.waiting[i];
        if (p->opened) continue;
        p->queued = 1;
        n += 1;
    }
    app.offer_count = 0;
    if (!n) return;
    waiting_fetch_next();
    worker_set_status(str8_pushf(ui.frame_arena, web_ok ? "Downloading %u addon%s from CurseForge, one after another" : "Opened %u CurseForge download page%s: each download starts by itself, and wam installs the files as they arrive",
                                 n, n == 1 ? "" : "s"), 0);
}

// The worker found files only CurseForge's page may serve. One file: fetch it right away (the user
// just asked for it). Several (Update all): offer them in the status bar instead of opening a
// window or tab per addon unasked.
static void waiting_add(PendingList *pl)
{
    if (pl->gen != app.root_gen) return;
    for (U32 i = 0; i < pl->count; i += 1) {
        PendingDownload *p = &pl->v[i];
        p->since_ms = GetTickCount64();
        U32 k = 0;
        while (k < app.waiting_count && strcmp(app.waiting[k].id, p->id) != 0) k += 1; // the same addon again: replace
        if (k == app.waiting_count) {
            if (app.waiting_count == ArrayCount(app.waiting)) continue;
            app.waiting_count += 1;
        }
        app.waiting[k] = *p;
    }
    if (pl->count == 1) {
        for (U32 k = 0; k < app.waiting_count; k += 1) {
            if (strcmp(app.waiting[k].id, pl->v[0].id) != 0) continue;
            app.waiting[k].queued = 1;
            B32 now = !web_ok || !ui_web_busy();
            if (now) waiting_fetch(&app.waiting[k]);
            worker_set_status(str8_pushf(ui.frame_arena, web_ok ? "%s can only come from CurseForge's own page: downloading it there%s."
                                                                : "Opened CurseForge's download page for %s (only it may serve the file). wam installs it when the download finishes.",
                                         pl->v[0].name, now ? "" : " after the current one"), 0);
        }
    } else {
        app.offer_count = 0;
        for (U32 k = 0; k < app.waiting_count; k += 1) app.offer_count += !app.waiting[k].opened;
        worker_set_status(str8_pushf(ui.frame_arena, "%u addons can only be downloaded on CurseForge's site.", app.offer_count), 1);
    }
}

// A new Downloads listing: install any archive a waiting file was expecting.
static void waiting_match(DownloadsSnap *ds)
{
    U64 now_ms = GetTickCount64();
    for (U32 i = 0; i < app.waiting_count;) {
        PendingDownload *p = &app.waiting[i];
        if (p->gen != app.root_gen || now_ms - p->since_ms > 30 * 60 * 1000) { waiting_remove(i); continue; } // gave up
        DownloadItem *hit = 0;
        for (U64 k = 0; k < ds->list.count && !hit; k += 1) {
            DownloadItem *it = &ds->list.v[k];
            if (!download_name_matches(str8_cstring(p->file), it->file_name)) continue;
            if (p->size > 0 && (S64)it->size != p->size) continue;     // still downloading, or another file
            hit = it;
        }
        if (!hit) { i += 1; continue; }
        waiting_install(p, hit->path, 0);
        worker_set_status(str8_pushf(ui.frame_arena, "Installing %s from %.*s...", p->name, S8F(hit->file_name)), 0);
        waiting_remove(i);
    }
}

//- Window callbacks (the UI library's window calls these)

static void app_key(WPARAM wp)
{
    B32 ctrl = GetKeyState(VK_CONTROL) < 0;
    char before[256]; U32 before_len = app.query.len; memcpy(before, app.query.s, sizeof(before));
    if (viewer_key(wp)) return; // the picture viewer takes every key while it's open
    if (settings_key(wp, ctrl)) return; // so does the settings popup
    if (changes_key(wp)) return;        // and the changes popup
    if (ctrl && wp == VK_OEM_COMMA) { settings_open(); return; }
    if (app.view == VIEW_SEARCH && browse_key(wp, ctrl)) { ui_request_frame(); return; }
    if (wp == VK_F5) push_task(TASK_REFRESH, str8(0, 0));
    if (ctrl && wp == 'F') app.view = VIEW_SEARCH;
    if (ctrl && wp == 'V' && app.view == VIEW_SEARCH) ui_text_paste(&app.query, ui_clipboard_text(ui.frame_arena));
    if (ctrl && wp == 'A' && app.view == VIEW_INSTALLED && app.snap) {
        for (U64 i = 0; i < app.snap->row_count; i += 1) keyset_add(&app.selected, app.snap->rows[i].key);
    }
    if (wp == VK_ESCAPE) {
        if (app.removing.count) keyset_clear(&app.removing);    // first: cancel a pending remove
        else if (app.view == VIEW_SEARCH && app.sel_id[0]) { app.sel_id[0] = 0; app.sel_index = -1; } // first Esc: the selection
        else if (app.view == VIEW_SEARCH) app.query.len = 0;
        else keyset_clear(&app.selected);
    }
    if (app.view == VIEW_SEARCH && (app.query.len != before_len || memcmp(before, app.query.s, sizeof(before)))) query_changed();
    ui_request_frame();
}

static void app_char(U32 c)
{
    if (settings_char(c)) return; // the settings popup takes the typing while it's open
    if (app.changes_open) return;
    if (app.view != VIEW_SEARCH || app.viewer_open) return; // (the viewer takes the keys)
    char before[256]; U32 before_len = app.query.len; memcpy(before, app.query.s, sizeof(before));
    if (c == '\r' && browse_enter()) {
        // Enter on the selected row installed it.
    } else if (c == '\r') {
        String8 q = str8_trim(ui_text(&app.query));
        String8 id = mgr_parse_spec(ui.frame_arena, q);
        B32 is_link = id.size && (!id_is(id, "cf:") || id_is(q, "cf:")); // plain numbers stay a search
        before_len = app.query.len; memcpy(before, app.query.s, sizeof(before));
        if (is_link) {
            SearchResult *known = cat_find(&app.cat, id); // its name and picture, if the catalog has it
            feedback_install(push_task(TASK_INSTALL, q), id, known ? known->name : id_rest(id), known ? known->icon : str8(0, 0));
            app.query.len = 0;
        } else if (q.size) {
            // Search now (with a status message) instead of waiting for the typing pause.
            KillTimer(app.hwnd, TIMER_SUGGEST);
            suggest_due_ms = 0;
            paging_sync();
            Task t = make_task(TASK_SEARCH, q, 0, app.flavor);
            t.sort = browse_sort();
            dispatch_task(t);
            copy_cstr(app.sugg_last_query, sizeof(app.sugg_last_query), q);
            app.sugg_last_sort = t.sort;
        }
    } else if (c != 0x16) { // Ctrl+V arrives as 0x16 too; it's handled as a key
        ui_text_char(&app.query, c);
    }
    if ((app.query.len != before_len || memcmp(before, app.query.s, sizeof(before)))) query_changed();
    ui_request_frame();
}

static void app_file_dropped(String8 path)
{
    if (is_archive_name(path)) feedback_install(push_task(TASK_INSTALL_FILE, path), str8(0, 0), path_filename(path), str8(0, 0));
    else worker_set_status(str8_pushf(ui.frame_arena, "%.*s isn't a .zip, .rar or .7z file", S8F(path_filename(path))), 1);
    app.view = VIEW_INSTALLED;
}

// Back from the browser? Look for new downloads (cheap: results are cached per file).
static void app_activated(void) { request_downloads_scan(); }

// Keys, characters, the suggestion timer, and the workers' results.
static LRESULT app_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, B32 *handled)
{
    (void)hwnd;
    *handled = 1;
    switch (msg) {
        case WM_KEYDOWN: app_key(wp); return 0;
        case WM_CHAR: app_char((U32)wp); return 0;
        case WM_TIMER: if (wp == TIMER_SUGGEST) suggest_timer(); else if (wp == TIMER_DETAILS) details_timer(); return 0;
        case WM_APP_SUGGEST: {
            SearchSnap *ss = (SearchSnap *)lp;
            search_result_arrived(ss);
            arena_release(ss->arena);
            ui_request_frame();
            return 0;
        }
        case WM_APP_CATALOG: {
            CatalogSnap *cs = (CatalogSnap *)lp;
            catalog_arrived(cs);
            arena_release(cs->arena);
            ui_request_frame();
            return 0;
        }
        case WM_APP_CATALOG_DONE: if ((U32)wp > app.catalog_served) app.catalog_served = (U32)wp; ui_request_frame(); return 0;
        case WM_APP_DETAILS: details_arrived((DetailSnap *)lp); ui_request_frame(); return 0;
        case WM_APP_CHANGES: changes_arrived((ChangesSnap *)lp); ui_request_frame(); return 0;
        case WM_APP_CHANGES_DONE: return 0;
        case WM_APP_DETAILS_DONE: if ((U32)wp > app.details_served) app.details_served = (U32)wp; ui_request_frame(); return 0;
        case WM_APP_SUGGEST_DONE: if ((U32)wp > app.suggest_served) app.suggest_served = (U32)wp; ui_request_frame(); return 0;
        case WM_APP_SNAPSHOT: {
            Snapshot *snap = (Snapshot *)lp;
            if (snap->gen != app.root_gen) { arena_release(snap->arena); return 0; } // from the previous install
            // Keep it for its flavor (switching back is instant) unless what we have was read later.
            Snapshot **slot = &app.snaps[snap->flavor];
            if (*slot && (*slot)->seq > snap->seq) { arena_release(snap->arena); return 0; }
            if (*slot) arena_release((*slot)->arena);
            *slot = snap;
            if (snap->flavor != app.flavor) return 0;
            app.snap = snap;
            // Forget selected rows that no longer exist (removed, or now tracked under a new id).
            KeySet *sets[2] = { &app.selected, &app.removing };
            for (int k = 0; k < 2; k += 1) {
                for (U32 i = 0; i < sets[k]->count;) {
                    String8 key = str8_cstring(sets[k]->keys[i]);
                    B32 found = 0;
                    for (U64 r = 0; r < snap->row_count && !found; r += 1) found = str8_match(snap->rows[r].key, key, 1);
                    if (found) i += 1; else keyset_remove(sets[k], key);
                }
            }
            request_downloads_scan(); // installed state may have changed
            ui_request_frame();
            return 0;
        }
        case WM_APP_SEARCH: {
            SearchSnap *ss = (SearchSnap *)lp; // Enter: the same as a typed search, with a status message
            search_result_arrived(ss);
            arena_release(ss->arena);
            ui_request_frame();
            return 0;
        }
        case WM_APP_DOWNLOADS: {
            DownloadsSnap *ds = (DownloadsSnap *)lp;
            app.dl_scan_queued = 0;
            if (app.dl_rescan_wanted) { app.dl_rescan_wanted = 0; request_downloads_scan(); }
            if (ds->flavor != app.flavor || ds->gen != app.root_gen) { arena_release(ds->arena); request_downloads_scan(); return 0; }
            if (app.downloads) arena_release(app.downloads->arena);
            app.downloads = ds;
            waiting_match(ds);
            ui_request_frame();
            return 0;
        }
        case WM_APP_DOWNLOADS_CHANGED: request_downloads_scan(); return 0; // the folder watcher saw something
        case WM_APP_PENDING: {
            PendingList *pl = (PendingList *)lp;
            waiting_add(pl);
            HeapFree(GetProcessHeap(), 0, pl);
            ui_request_frame();
            return 0;
        }
        case WM_APP_STATUS: ui_request_frame(); return 0;
        case WM_APP_TASK_DONE: if (app.pending) app.pending -= 1; feedback_task_done((U32)wp); ui_request_frame(); return 0;
        case WM_APP_REFRESH_DONE: if ((U32)wp > app.refresh_served) app.refresh_served = (U32)wp; ui_request_frame(); return 0;
    }
    *handled = 0;
    return 0;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show)
{
    (void)prev; (void)cmdline;
    app.t_start = os_now_us();
    app.arena = arena_alloc(GB(1));
    ui.frame_arena = arena_alloc(GB(1));
    ui_open_log = os_get_env(app.arena, "WAM_TEST_OPEN_LOG"); // testing: log link opens instead
    ui_frame_log = os_get_env(app.arena, "WAM_TEST_FRAME_LOG"); // testing: log frame times
    ui_shot_dir = os_get_env(app.arena, "WAM_TEST_SHOT_DIR");   // testing: F12 saves the frame there
    ui_image_log = os_get_env(app.arena, "WAM_TEST_IMAGE_LOG"); // testing: log pictures that fail to load
    ui_web_data_dir = path_join(app.arena, path_join(app.arena, os_get_env(app.arena, "LOCALAPPDATA"), str8_lit("wam")), str8_lit("webview2"));

    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc > 1) app.wow_root = wow_normalize_root(app.arena, os_str8_from_16(app.arena, (U16 *)argv[1], wcslen(argv[1])));
    else app.wow_root = wow_find_root(app.arena);
    LocalFree(argv);
    app.api_key = cf_api_key(app.arena);
    {
        Temp t = temp_begin(app.arena);
        WagoKeySource src;
        String8 wk = wago_api_key_ex(t.arena, &src);
        app.wago_from_env = src == WAGO_KEY_ENV;
        if (wk.size < sizeof(app.wago_key)) { memcpy(app.wago_key, wk.str, wk.size); app.wago_key[wk.size] = 0; }
        temp_end(t);
    }

    // Default flavor: Forever, else Retail, else the first one installed. The tabs switch between them.
    detect_flavors();
    session_restore_flavor(); // the one used last time, if this install still has it

    static UI_App desc = {
        L"wam", L"WoW Addon Manager", 1100, 700, 1, 820, 460, 1,
        app_frame, app_message, app_file_dropped, app_activated,
    };
    {
        Temp t = temp_begin(app.arena);
        WamWindow win = wam_settings_load(t.arena).window; // where it was last time
        desc.x = win.x; desc.y = win.y; desc.w = win.w; desc.h = win.h; desc.maximized = win.maximized;
        temp_end(t);
    }
    if (!ui_window_open(inst, &desc)) return 1;
    ui.theme = ui_theme_tinker();
    app.hwnd = ui_hwnd;
    web_ok = ui_web_available() && !os_get_env(app.arena, "WAM_TEST_NO_WEBVIEW").size; // testing: force the browser path

    InitializeCriticalSection(&task_lock);
    InitializeCriticalSection(&status_lock);
    task_sem = CreateSemaphoreW(0, 0, TASK_QUEUE, 0);
    os_thread_start(worker_main, 0);
    lane_start(&refresh_lane, WM_APP_REFRESH_DONE);
    lane_start(&suggest_lane, WM_APP_SUGGEST_DONE);
    lane_start(&downloads_lane, WM_APP_DOWNLOADS_DONE);
    lane_start(&catalog_lane, WM_APP_CATALOG_DONE);
    lane_start(&details_lane, WM_APP_DETAILS_DONE);
    lane_start(&changes_lane, WM_APP_CHANGES_DONE);
    app.category = app.subcategory = -1;
    {
        Temp t = temp_begin(app.arena);
        app.detail_share = (F32)wam_settings_load(t.arena).detail_share;
        temp_end(t);
    }
    ui_image_fetch = image_fetch;
    os_thread_start(image_cache_prune, 0);
    app.sort_browse = SORT_DOWNLOADS;
    app.sort_query = SORT_BEST;
    os_thread_start(downloads_watch_main, 0);
    catalog_reset();
    session_restore(); // the view, search, sorts, filters and selection of last time
    if (app.wow_root.size) {
        push_task(TASK_REFRESH, str8(0, 0)); // show local state right away
        push_task(TASK_SYNC, str8(0, 0));    // then recognize + check updates (GitHub/WoWInterface need no key)
    }
    int code = ui_window_run(show);
    UI_WindowPlace *p = &ui_window_place;
    if (p->w > 0 && p->h > 0) wam_save_window((WamWindow){ p->x, p->y, p->w, p->h, p->maximized });
    session_save();
    return code;
}
