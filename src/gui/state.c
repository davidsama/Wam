// gui/state.c - wam's GUI state: what the worker hands the UI (snapshots), the task queue and
// its lanes, the App struct, and posting tasks and status messages. Part of main_gui.c's unity build.

//- Snapshots (built by the worker, read by the UI)

typedef struct Row {
    String8 key;         // id when tracked, else the folder name: what selection and tasks refer to
    String8 id;          // tracked by wam: "cf:..", "gh:..", "wowi:.."; empty = untracked
    String8 folder;      // untracked: the folder name
    String8 title, sub, version, iface, latest;
    String8 author, notes;   // from the .toc
    String8 cat_ids[4];      // catalog entries that may describe it (its picture, summary): tracked id, then .toc ids
    U32 cat_id_count;
    B32 update;
    B32 edited;          // tracked, has an update, and its files changed since wam installed it
    B32 pinned;          // kept at its version: updates skip it
    U32 link_count;      // provider pages: links[0] is where wam installs it from (when tracked)
    String8 links[4];
    int link_src[4];     // SRC_*
    B32 link_primary;    // links[0] is the tracked source (drawn filled)
    U32 *req;            // rows this one requires (from .toc dependencies)
    U32 req_count;
} Row;

typedef struct Snapshot {
    Arena *arena;
    int flavor;
    U32 gen;
    S32 seq;            // when its data was read: a newer snapshot never gets replaced by an older one
    String8 addons_dir;
    GameVersion client;
    Row *rows;
    U64 row_count, tracked, updates, folders;
    F64 scan_ms;
    U64 checked;        // it came from a check for updates that went through, at this time (else 0)
} Snapshot;

typedef struct SearchSnap {
    Arena *arena;
    int flavor;
    U32 gen;
    String8 query;
    int sort;            // BrowseSort it was asked for, and the page
    U64 index;
    SearchResult *v;
    U64 count;
    U64 cf_count;        // of those, from CurseForge: a full page (CF_PAGE_SIZE) means there may be more
    B32 failed;
} SearchSnap;

// The catalog for one flavor, from the disk cache or the network (core/catalog.c).
typedef struct CatalogSnap {
    Arena *arena;
    int flavor;
    U32 gen;
    Catalog cat;
    S64 saved;           // when it was fetched (unix seconds)
    B32 final;           // nothing newer follows for this request
    String8 error;
} CatalogSnap;

// One addon's details for the Get addons detail panel (core/catalog.c).
typedef struct DetailSnap {
    Arena *arena;
    int flavor;
    U32 gen;
    U64 arrived_ms;  // failures are asked again when selected later (a rate limit or network hiccup passes)
    AddonDetails d;
} DetailSnap;
#define DETAIL_CACHE 16

// What an installed addon's update would bring, for the changes popup (core/catalog.c).
typedef struct ChangesSnap {
    Arena *arena;
    int flavor;
    U32 gen;
    AddonChanges c;
} ChangesSnap;

//- Tasks

typedef struct DownloadsSnap {
    Arena *arena;
    int flavor;
    U32 gen;
    DownloadList list;
    GameVersion client;
} DownloadsSnap;

typedef enum TaskKind { TASK_REFRESH, TASK_SYNC, TASK_INSTALL, TASK_INSTALL_FILE, TASK_UPDATE, TASK_REMOVE, TASK_SEARCH, TASK_SCAN_DOWNLOADS, TASK_SUGGEST, TASK_INSTALL_DOWNLOADED, TASK_CATALOG, TASK_DETAILS, TASK_PIN, TASK_CHANGES } TaskKind;
// TASK_INSTALL_DOWNLOADED text: path, id, file id, name, version, website - one per line (see main_gui.c).
// text: what the user pasted (install), an archive path (zip/rar/7z), a query (search), or for update/remove
// row keys (addon ids or untracked folder names) separated by '\n'; empty update = all.
#define WAGO_KEY_MAX 600
typedef struct Task {
    TaskKind kind; int flavor; U32 gen; S32 seq; char wow_root[1024]; B32 force;
    U32 id;               // queued tasks: numbered in order; on_task_done says which one finished
    U64 queued_us;        // when it was queued (tasks.log: how long it waited)
    int sort; U64 index;  // searches: BrowseSort and page start
    char text[8192]; char wago_key[WAGO_KEY_MAX]; // the key as of queuing
} Task;

#define TASK_QUEUE 64
static OS_Mutex task_lock, status_lock;
static OS_Semaphore task_sem;
static volatile S32 snap_seq;
// A lane: a thread of its own for one kind of quick task, so it never waits behind a sync or an
// install. Only the newest request matters, so it's one slot rather than a queue.
//   listings (TASK_REFRESH): switching flavors
//   suggestions (TASK_SUGGEST): search as you type
//   the Downloads list (TASK_SCAN_DOWNLOADS): local, but RAR/7z inspection can take a while
//   the catalog (TASK_CATALOG): Get addons' list, from the disk cache and then the network
//   details (TASK_DETAILS): the selected addon's description, changelog and screenshots
//   changes (TASK_CHANGES): an installed addon's release notes since its version (the changes popup)
typedef struct Lane { OS_Mutex lock; OS_Event event; Task slot; B32 full; U32 id; WM_PostFunc *done; } Lane;
static Lane refresh_lane, suggest_lane, downloads_lane, catalog_lane, details_lane, changes_lane;
static Task task_q[TASK_QUEUE];
static U32 task_head, task_tail;
static char status_text[512];
static U64 status_ms;          // when it was set: a message that isn't an error gives way to the summary after a while
static char status_url[512];   // with an error: "Open download page" target
static B32 status_is_error;

//- App state (UI thread only, except where noted)

typedef enum View { VIEW_INSTALLED, VIEW_SEARCH, VIEW_DOWNLOADS, VIEW_COUNT } View;

// An install the user started, shown as a row in Installed until its task is done (feedback.c).
typedef struct PendingInstall {
    U32 task;            // the queued task's id
    int flavor;
    char id[160];        // the addon's id when known (Get addons), to show "Installing..." on its button
    char name[128];
    char icon[512];      // its picture's URL, if the catalog has one
    int src;             // Source, or -1
    F32 shown;           // its progress bar's fill, easing toward the real progress
} PendingInstall;

// How Get addons orders its list. Best match only applies while there's a query.
typedef enum BrowseSort { SORT_BEST, SORT_DOWNLOADS, SORT_UPDATED, SORT_NEW, SORT_COUNT } BrowseSort;
typedef enum Source { SRC_CURSEFORGE, SRC_WAGO, SRC_WOWI, SRC_GITHUB, SRC_TUKUI, SRC_COUNT } Source;


// A small set of row keys that survives snapshot swaps (rows are rebuilt on every refresh).
#define KEYSET_MAX 1024
typedef struct KeySet { char keys[KEYSET_MAX][160]; U32 count; } KeySet;

static S64 keyset_index(KeySet *s, String8 key)
{
    for (U32 i = 0; i < s->count; i += 1) if (str8_match(str8_cstring(s->keys[i]), key, 1)) return i;
    return -1;
}
static B32 keyset_has(KeySet *s, String8 key) { return keyset_index(s, key) >= 0; }
static void keyset_add(KeySet *s, String8 key)
{
    if (keyset_has(s, key) || s->count >= KEYSET_MAX || key.size >= sizeof(s->keys[0])) return;
    memcpy(s->keys[s->count], key.str, key.size);
    s->keys[s->count][key.size] = 0;
    s->count += 1;
}
static void keyset_remove(KeySet *s, String8 key)
{
    S64 i = keyset_index(s, key);
    if (i < 0) return;
    s->count -= 1;
    if ((U32)i != s->count) memcpy(s->keys[i], s->keys[s->count], sizeof(s->keys[0]));
}
static void keyset_clear(KeySet *s) { s->count = 0; }

// A CurseForge file the user is fetching through its download page (filled by the worker, kept by
// the UI until the archive shows up or 30 minutes pass).
typedef struct PendingDownload {
    char id[64], name[128], version[128], website[256], file[256], url[512];
    S64 file_id, size;
    int flavor;
    U32 gen;
    U64 since_ms;
    B32 opened;   // its page was shown (in the download window or the browser)
    B32 queued;   // to be fetched in the download window when it's free
} PendingDownload;

typedef struct PendingList { PendingDownload v[16]; U32 count; int flavor; U32 gen; } PendingList; // worker -> UI

typedef struct App {
    Arena *arena;
    String8 wow_root;       // UI thread only; each task gets a copy
    U32 root_gen;           // bumped when the user picks another install: results from the old one are dropped
    Rect root_btn;          // where the "choose install" menu drops down from
    String8 api_key;        // read-only after startup (worker reads it)
    char wago_key[WAGO_KEY_MAX]; // UI thread only; copied into each task
    B32 wago_from_env;      // the Wago key is WAGO_API_KEY's (the settings can't change it)
    B32 flavor_exists[FLAVOR_COUNT];
    int flavor;
    View view;
    Snapshot *snap;                 // = snaps[flavor]: what the Installed view shows
    U64 checked[FLAVOR_COUNT];      // when each flavor was last checked for updates (0: not since wam started)
    Snapshot *snaps[FLAVOR_COUNT];  // the latest listing of each flavor, so switching back is instant
    U32 refresh_asked, refresh_served; // listing requests: loading while served < asked
    U32 suggest_asked, suggest_served; // searches and pages on the suggestion lane, the same way
    U32 catalog_asked, catalog_served; // catalog loads (cache, then network)
    // Get addons: the catalog for this flavor (prefetched, cached on disk, and grown by every search
    // and page), shown filtered and sorted. Everything runs on it locally; the network only adds to it.
    Catalog cat;
    S64 catalog_saved;         // when the catalog was fetched (0 = none yet)
    B32 catalog_failed;        // the last fetch got nothing (offline?)
    BrowseSort sort_browse;    // with an empty query
    BrowseSort sort_query;     // while searching (Best match by default)
    U32 src_hidden;            // bit per SRC_*: sources the chips turned off
    B32 hide_installed;
    S32 category;              // index into cat_categories, -1 = all
    S32 subcategory;           // index into cat_subcategories (of that category), -1 = the whole category
    U32 cat_counts[CAT_CATEGORY_COUNT]; // per category, among what the other filters show
    U32 sub_counts[CAT_SUBCATEGORY_COUNT];
    U64 cat_any;               // of those, how many (for "All categories")
    Rect sort_btn, cat_btn, src_btn; // where the filters' menus drop down from
    U32 src_count[SRC_COUNT];  // catalog entries per source (which chips to show)
    U32 link_cat_version; Snapshot *link_snap; S32 link_snap_seq; B32 link_valid; // what cat_link last paired (see browse_link)
    // The ordered list, re-sorted only when what it depends on changes (see browse_order).
    Arena *order_arena;
    U32 *order;
    U64 order_count;
    U32 order_cat_version; Snapshot *order_snap; S32 order_snap_seq; char order_query[256]; int order_sort; U32 order_hidden; B32 order_hide_inst; S32 order_category, order_subcategory; B32 order_wago_key; B32 order_valid;
    char sel_id[160];          // the selected row (keyboard / click), "" = none; Enter installs it
    S64 sel_index;             // its position in order, -1 = none
    B32 sel_reveal;            // scroll it into view next frame
    char sel_restore[160];     // last session's selection: selected once the catalog has it (session.c)
    S64 hover_index;           // row under the mouse last frame, -1 = none
    U64 last_type_ms;          // last edit of the query
    // The detail panel: the selected addon's details, fetched a moment after the selection settles.
    DetailSnap *details[DETAIL_CACHE]; // the last few, newest at details_next - 1
    U32 details_next;
    char detail_want[160];     // the selection the panel follows
    char detail_asked[160];    // what was last sent to the details lane
    U32 details_asked, details_served;
    // The changes popup (changes.c): an installed addon's release notes since its version.
    B32 changes_open, changes_fresh;
    char changes_id[160];      // the addon it's for (its tracked id)
    ChangesSnap *changes;      // what arrived for it (0 while loading)
    UI_Scroll changes_scroll;
    UI_Scroll detail_scroll;
    // The picture viewer: a screenshot at full size over the window (draw_viewer).
    B32 viewer_open;
    char viewer_urls[6][1024];
    U32 viewer_count, viewer_index;
    B32 viewer_fresh;          // opened this frame (by a click the viewer mustn't see)
    F32 detail_share;          // the panel's share of the view's width (dragged; saved in settings.json), 0 = default
    F32 detail_content_h;      // the panel's content height last frame
    // Paging: scrolling near the end asks CurseForge for the next page of this sort + query.
    char more_query[256]; int more_sort; U64 more_index; B32 more_done; U32 more_asked;
    char sugg_last_query[256]; // last query sent while typing (no duplicate requests)
    int sugg_last_sort;
    DownloadsSnap *downloads;
    B32 dl_scan_queued;      // a downloads scan is queued or running
    B32 dl_rescan_wanted;    // asked again while one was running: run another when it finishes
    U64 dl_last_scan_ms;
    U32 pending;            // tasks queued or running
    // CurseForge files only CurseForge's own page may serve: wam opened (or offers) that page and
    // installs the archive when the browser saves it to a download folder.
    PendingDownload waiting[16];
    U32 waiting_count;
    U32 offer_count;        // of those, how many still need their page opened (the status bar offers it)
    UI_Scroll scroll[VIEW_COUNT];
    KeySet selected;        // checked rows
    KeySet removing;        // rows waiting for the remove confirmation bar
    // What the user just asked for shows at once, before the worker gets to it (feedback.c):
    KeySet gone;            // rows being removed: hidden until that task is done (back if it failed)
    U32 gone_task;
    KeySet updating;        // rows being updated: "Updating..." instead of their button
    U32 updating_task;
    PendingInstall installing[8]; // installs on their way: placeholder rows at the top of Installed
    U32 installing_count;
    U32 task_ids;           // the last queued task's id
    U32 tasks_done;         // the last finished one's
    B32 remove_with_deps;   // checkbox in that bar
    UI_TextBuf query;       // the Get addons field
    // The settings popup (settings.c)
    B32 settings_open, settings_fresh; // fresh: opened this frame (by a click it mustn't see)
    UI_TextBuf wago_entry;  // a Wago key being typed there (never drawn)
    char settings_note[160]; B32 settings_note_error; // what the last save did
    F32 settings_h;         // the panel's height, measured by the last frame
    F64 init_ms;
    B32 show_perf;          // the status line also shows frame and startup times (settings.json)
    U64 t_start;
} App;

static App app;

// Workers' results, run on the UI thread (wm_post; main_gui.c). The snapshots' arenas become the UI's.
static void on_snapshot(U64 param, void *snap);        // Snapshot: a listing
static void on_search(U64 param, void *ss);            // SearchSnap: a typed search, or a page of a sort
static void on_catalog(U64 param, void *cs);           // CatalogSnap: from the disk cache or the network
static void on_details(U64 param, void *ds);           // DetailSnap
static void on_changes(U64 param, void *cs);           // ChangesSnap
static void on_downloads(U64 param, void *ds);         // DownloadsSnap
static void on_pending(U64 param, void *pl);           // PendingList (os_alloc): CurseForge files to fetch through their page
static void on_status(U64 param, void *ptr);           // the status line or the progress changed: a frame
static void on_task_done(U64 id, void *ptr);           // a queued task finished
static void on_refresh_done(U64 id, void *ptr);        // a lane answered request id
static void on_suggest_done(U64 id, void *ptr);
static void on_catalog_done(U64 id, void *ptr);
static void on_details_done(U64 id, void *ptr);
static void on_downloads_changed(U64 param, void *ptr); // a download folder changed (the watcher thread)

static void browse_for_root(void);   // deferred UI (wm_defer): they open a dialog / menu
static void show_root_menu(void);
static void waiting_open_all(void);  // main_gui.c: opens the CurseForge pages of files waiting to be downloaded
static B32 web_ok;   // WebView2 is available: CurseForge pages open in wam's download window (wm_webview.c)

static void copy_cstr(char *dst, U64 cap, String8 s)
{
    U64 n = Min(s.size, cap - 1);
    memcpy(dst, s.str, n);
    dst[n] = 0;
}

static Task make_task(TaskKind kind, String8 text, B32 force, int flavor)
{
    Task t = { 0 };
    t.kind = kind;
    t.force = force;
    t.flavor = flavor;
    t.gen = app.root_gen;
    if (app.wow_root.size < sizeof(t.wow_root)) memcpy(t.wow_root, app.wow_root.str, app.wow_root.size);
    U64 n = Min(text.size, sizeof(t.text) - 1);
    if (n) memcpy(t.text, text.str, n);
    memcpy(t.wago_key, app.wago_key, sizeof(t.wago_key));
    return t;
}

// Queues t, or for the quick kinds puts it in its lane (replacing whatever was waiting there).
// Returns a queued task's id (0: a lane's, or the queue was full).
static U32 dispatch_task(Task t)
{
    TaskKind kind = t.kind;
    if (kind == TASK_REFRESH || kind == TASK_SUGGEST || kind == TASK_SCAN_DOWNLOADS || kind == TASK_CATALOG || kind == TASK_DETAILS || kind == TASK_CHANGES) {
        Lane *lane = kind == TASK_REFRESH ? &refresh_lane : kind == TASK_SUGGEST ? &suggest_lane : kind == TASK_CATALOG ? &catalog_lane
                   : kind == TASK_DETAILS ? &details_lane : kind == TASK_CHANGES ? &changes_lane : &downloads_lane;
        U32 id = kind == TASK_REFRESH ? ++app.refresh_asked : kind == TASK_SUGGEST ? ++app.suggest_asked : kind == TASK_CATALOG ? ++app.catalog_asked
               : kind == TASK_DETAILS ? ++app.details_asked : 0;
        os_mutex_lock(&lane->lock);
        lane->slot = t;
        lane->full = 1;
        lane->id = id;
        os_mutex_unlock(&lane->lock);
        os_event_signal(lane->event);
        return 0;
    }
    os_mutex_lock(&task_lock);
    B32 queued = task_tail - task_head < TASK_QUEUE;
    if (queued) { t.id = ++app.task_ids; t.queued_us = os_now_us(); task_q[task_tail++ % TASK_QUEUE] = t; }
    os_mutex_unlock(&task_lock);
    if (queued) { app.pending += 1; os_semaphore_signal(task_sem); }
    return queued ? t.id : 0;
}

static U32 push_task_for(TaskKind kind, String8 text, B32 force, int flavor) { return dispatch_task(make_task(kind, text, force, flavor)); }
static U32 push_task_ex(TaskKind kind, String8 text, B32 force) { return push_task_for(kind, text, force, app.flavor); }
static U32 push_task(TaskKind kind, String8 text) { return push_task_ex(kind, text, 0); }

static void worker_set_status_url(String8 msg, B32 is_error, String8 url)
{
    os_mutex_lock(&status_lock);
    U64 n = Min(msg.size, sizeof(status_text) - 1);
    memcpy(status_text, msg.str, n);
    status_text[n] = 0;
    status_is_error = is_error;
    status_ms = os_now_ms();
    n = str8_starts_with(url, str8_lit("https://"), 1) ? Min(url.size, sizeof(status_url) - 1) : 0;
    if (n) memcpy(status_url, url.str, n);
    status_url[n] = 0;
    os_mutex_unlock(&status_lock);
    wm_post(on_status, 0, 0);
}

static void worker_set_status(String8 msg, B32 is_error) { worker_set_status_url(msg, is_error, str8(0, 0)); }

static void worker_status_cb(void *user, String8 msg) { (void)user; worker_set_status(msg, 0); }

// The queued task the worker is running, and how far an install in it is (thousandths; -1: under
// way, how far unknown). Written by the worker, read by the UI for the progress bars (feedback.c).
static volatile S32 work_task, work_permille = -1;

static void worker_progress_cb(void *user, F32 fraction)
{
    (void)user;
    S32 v = fraction < 0 ? -1 : (S32)(Clamp(fraction, 0.0f, 1.0f) * 1000.0f);
    S32 was = work_permille;
    if (v == was || (v > was && was >= 0 && v - was < 5)) return; // half a percent at a time is plenty
    os_atomic_store(&work_permille, v);
    wm_post(on_status, 0, 0); // a frame for the bar
}

// "11508, 16001, 120105" -> the entry for this client: exact, else same major.minor, else same major, else first.
static String8 best_interface(String8 list, GameVersion cv)
{
    U32 want = cv.major * 10000 + cv.minor * 100 + cv.patch;
    String8 best = { 0 }, first = { 0 };
    int best_score = -1;
    String8 rest = list;
    while (rest.size) {
        U64 comma = str8_find(rest, 0, str8_lit(","), 0);
        String8 item = str8_trim(str8_prefix(rest, comma));
        rest = str8_skip(rest, comma + 1);
        if (!item.size) continue;
        if (!first.size) first = item;
        U32 v = (U32)str8_to_s64(item);
        int score = v == want ? 3 : v / 100 == want / 100 ? 2 : v / 10000 == want / 10000 ? 1 : 0;
        if (score > best_score) { best_score = score; best = item; }
    }
    return best_score > 0 ? best : first;
}

