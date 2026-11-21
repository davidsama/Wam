// downloads.c - find addon archives the user downloaded by hand.
//
// Looks in the Windows Downloads known folder plus any custom download folder set in
// Chromium-based browsers (Preferences: download.default_directory) or Firefox
// (prefs.js: browser.download.dir when folderList = 2). Only local files are read.
//
// An archive is an addon if it has <Folder>/<Folder>.toc entries. Zips are inspected by
// reading only their central directory and the .toc entries, so big unrelated zips cost
// a few KB. RAR / 7z are listed with Windows' tar.exe.

#define DL_MAX_ARCHIVES 80        // newest archives inspected per scan
#define DL_MAX_TOCS 24            // .toc files read per archive (addon packs can be big)

typedef enum DownloadState { DL_NEW, DL_DIFFERENT, DL_INSTALLED } DownloadState;

typedef struct DownloadItem {
    String8 path, file_name;
    U64 size, mtime;
    ArchiveKind kind;
    String8 title;                // the main addon's title, or "N addons" for a pack
    String8 version;
    String8 interface_version;
    String8List folders;          // top-level folders that have a .toc
    String8List titles;           // their titles (same order)
    B32 is_pack;                  // several unrelated addons in one archive
    // Compared with the current flavor's AddOns:
    DownloadState state;
    String8 installed_version;
    U32 installed_count;
    B32 is_new;                   // never installed anywhere (any flavor or install) - see downloads_mark_new
} DownloadItem;

typedef struct DownloadList { DownloadItem *v; U64 count; String8List dirs; } DownloadList;

//- Download folders

static void dl_add_dir(Arena *a, String8List *dirs, String8 d)
{
    d = str8_trim(d);
    while (d.size > 3 && (d.str[d.size - 1] == '\\' || d.str[d.size - 1] == '/')) d.size -= 1;
    if (!d.size || !os_dir_exists(d)) return;
    d = path_normalize_slashes(a, d);
    if (!list_has_name(dirs, d)) str8_list_push(a, dirs, d);
}

static void dl_chromium(Arena *a, String8List *dirs, String8 user_data)
{
    Temp scratch = scratch_begin(a);
    OS_FileInfoArray profiles = os_list_dir(scratch.arena, user_data);
    for (U64 i = 0; i < profiles.count; i += 1) {
        String8 n = profiles.v[i].name;
        if (!profiles.v[i].is_dir || !(str8_match(n, str8_lit("Default"), 1) || str8_starts_with(n, str8_lit("Profile "), 1))) continue;
        String8 prefs = os_read_file(scratch.arena, path_join(scratch.arena, path_join(scratch.arena, user_data, n), str8_lit("Preferences")));
        String8 dir = json_str(json_get(json_get(json_parse(scratch.arena, prefs), "download"), "default_directory"));
        if (dir.size) dl_add_dir(a, dirs, str8_copy(a, dir));
    }
    scratch_end(scratch);
}

// Opera keeps Preferences directly in its profile folder.
static void dl_chromium_single(Arena *a, String8List *dirs, String8 profile)
{
    Temp scratch = scratch_begin(a);
    String8 prefs = os_read_file(scratch.arena, path_join(scratch.arena, profile, str8_lit("Preferences")));
    String8 dir = json_str(json_get(json_get(json_parse(scratch.arena, prefs), "download"), "default_directory"));
    if (dir.size) dl_add_dir(a, dirs, str8_copy(a, dir));
    scratch_end(scratch);
}

// user_pref("browser.download.dir", "D:\\Downloads"); counts only with folderList == 2.
static void dl_firefox(Arena *a, String8List *dirs, String8 profiles_dir)
{
    Temp scratch = scratch_begin(a);
    OS_FileInfoArray profiles = os_list_dir(scratch.arena, profiles_dir);
    for (U64 i = 0; i < profiles.count; i += 1) {
        if (!profiles.v[i].is_dir) continue;
        String8 text = os_read_file(scratch.arena, path_join(scratch.arena, path_join(scratch.arena, profiles_dir, profiles.v[i].name), str8_lit("prefs.js")));
        B32 custom = str8_find(text, 0, str8_lit("\"browser.download.folderList\", 2)"), 0) < text.size;
        String8 key = str8_lit("\"browser.download.dir\", \"");
        U64 p = str8_find(text, 0, key, 0);
        if (!custom || p == text.size) continue;
        U8 *out = push_array_nz(a, U8, text.size - p);
        U64 n = 0;
        for (U64 j = p + key.size; j < text.size && text.str[j] != '"'; j += 1) {
            if (text.str[j] == '\\' && j + 1 < text.size) j += 1; // JS string escapes: \\ and \"
            out[n++] = text.str[j];
        }
        dl_add_dir(a, dirs, str8(out, n));
    }
    scratch_end(scratch);
}

static String8List downloads_dirs(Arena *a)
{
    String8List dirs = { 0 };
    String8 test_dir = os_get_env(a, "WAM_TEST_DOWNLOADS_DIR"); // testing: look only here
    if (test_dir.size) { dl_add_dir(a, &dirs, test_dir); return dirs; }
    dl_add_dir(a, &dirs, os_downloads_dir(a));
    String8 local = os_get_env(a, "LOCALAPPDATA"), roaming = os_get_env(a, "APPDATA");
    static char *chromium[] = {
        "Google\\Chrome\\User Data", "Microsoft\\Edge\\User Data", "BraveSoftware\\Brave-Browser\\User Data",
        "Vivaldi\\User Data", "Chromium\\User Data",
    };
    if (local.size) for (int i = 0; i < (int)ArrayCount(chromium); i += 1) dl_chromium(a, &dirs, path_join(a, local, str8_cstring(chromium[i])));
    if (roaming.size) {
        dl_chromium_single(a, &dirs, path_join(a, roaming, str8_lit("Opera Software\\Opera Stable")));
        dl_chromium_single(a, &dirs, path_join(a, roaming, str8_lit("Opera Software\\Opera GX Stable")));
        dl_firefox(a, &dirs, path_join(a, roaming, str8_lit("Mozilla\\Firefox\\Profiles")));
    }
    return dirs;
}

//- Inspecting one archive

typedef struct DlToc { String8 folder; String8 entry; S64 zip_index; Addon toc; B32 read; } DlToc;

// "Folder/Folder.toc" or "Folder/Folder_Mainline.toc" (either slash).
static B32 dl_toc_entry(String8 name, String8 *out_folder)
{
    U64 slash = 0;
    while (slash < name.size && name.str[slash] != '/' && name.str[slash] != '\\') slash += 1;
    if (slash == 0 || slash >= name.size) return 0;
    String8 folder = str8_prefix(name, slash), file = str8_skip(name, slash + 1);
    for (U64 i = 0; i < file.size; i += 1) if (file.str[i] == '/' || file.str[i] == '\\') return 0;
    if (!toc_name_matches_folder(file, folder)) return 0;
    *out_folder = folder;
    return 1;
}

// Adds an entry, preferring <Folder>.toc over flavor-specific ones for the same folder.
static void dl_note_toc(DlToc *tocs, U32 *count, String8 folder, String8 entry, S64 zip_index)
{
    for (U32 i = 0; i < *count; i += 1) {
        if (!str8_match(tocs[i].folder, folder, 1)) continue;
        String8 plain = str8_skip(entry, folder.size + 1);
        if (str8_match(str8_chop(plain, 4), folder, 1)) { tocs[i].entry = entry; tocs[i].zip_index = zip_index; }
        return;
    }
    if (*count >= 512) return;
    tocs[*count].folder = folder;
    tocs[*count].entry = entry;
    tocs[*count].zip_index = zip_index;
    *count += 1;
}

static B32 dl_inspect(Arena *a, DownloadItem *it)
{
    Temp scratch = scratch_begin(a);
    Arena *s = scratch.arena;
    DlToc *tocs = push_array(s, DlToc, 512);
    U32 ntocs = 0;

    String8 head = os_read_file_range(s, it->path, 0, 16);
    it->kind = archive_kind(head);
    ZipArchive z = { 0 };
    String8 tar = { 0 };
    if (it->kind == ARCHIVE_ZIP) {
        if (!zip_open_file(s, it->path, it->size, &z)) { scratch_end(scratch); return 0; }
        for (U64 i = 0; i < z.count; i += 1) {
            String8 folder;
            if (zip_name_is_safe(z.v[i].name) && dl_toc_entry(z.v[i].name, &folder)) dl_note_toc(tocs, &ntocs, folder, z.v[i].name, (S64)i);
        }
    } else if (it->kind == ARCHIVE_RAR || it->kind == ARCHIVE_7Z) {
        tar = path_join(s, os_system_dir(s), str8_lit("tar.exe"));
        OS_RunResult list = os_run(s, tar, str8_pushf(s, "\"%.*s\" -tf \"%.*s\"", S8F(tar), S8F(it->path)), 20000);
        if (!list.started || list.timed_out || list.exit_code) { scratch_end(scratch); return 0; }
        for (String8 rest = list.output; rest.size;) {
            String8 name = str8_trim(str8_pop_line(&rest)), folder;
            if (name.size && zip_name_is_safe(name) && dl_toc_entry(name, &folder)) dl_note_toc(tocs, &ntocs, folder, name, -1);
        }
    } else {
        scratch_end(scratch);
        return 0;
    }
    if (!ntocs) { scratch_end(scratch); return 0; }

    // Read the .toc files (a limited number for big packs). For RAR / 7z, extract all of them
    // with one tar call into a temp folder: in a solid archive each separate call would
    // decompress from the start again.
    U32 nread = Min(ntocs, DL_MAX_TOCS);
    String8 tmp = { 0 };
    if (it->kind != ARCHIVE_ZIP) {
        tmp = path_join(s, os_get_env(s, "TEMP"), str8_pushf(s, "wam-dl-%llu", os_now_us()));
        String8List args = { 0 };
        str8_list_push(s, &args, str8_pushf(s, "\"%.*s\" -xf \"%.*s\" -C \"%.*s\"", S8F(tar), S8F(it->path), S8F(tmp)));
        for (U32 i = 0; i < nread; i += 1) str8_list_push(s, &args, str8_pushf(s, "\"%.*s\"", S8F(tocs[i].entry)));
        if (os_make_dirs(tmp)) os_run(s, tar, str8_list_join(s, &args, str8_lit(" ")), 20000);
    }
    for (U32 i = 0; i < nread; i += 1) {
        String8 text = { 0 };
        if (tocs[i].zip_index >= 0) {
            B32 ok;
            text = zip_read_file_entry(s, it->path, &z.v[tocs[i].zip_index], &ok);
            if (!ok) text = str8(0, 0);
        } else if (tmp.size) {
            text = os_read_file(s, path_join(s, tmp, path_normalize_slashes(s, tocs[i].entry)));
        }
        addon_parse_toc(s, &tocs[i].toc, text);
        tocs[i].read = 1;
    }
    if (tmp.size) os_delete_tree(tmp);

    // Which folder is "the" addon? One whose name prefixes all others (BigWigs, Details),
    // else the one most others depend on (DBM-Core). Neither: a pack of separate addons.
    S64 main = -1;
    for (U32 i = 0; i < ntocs && main < 0; i += 1) {
        B32 prefix_of_all = 1;
        for (U32 j = 0; j < ntocs && prefix_of_all; j += 1) prefix_of_all = str8_starts_with(tocs[j].folder, tocs[i].folder, 1);
        if (prefix_of_all) main = i;
    }
    if (main < 0) {
        U32 best = 0;
        for (U32 i = 0; i < ntocs; i += 1) {
            U32 dependents = 0;
            for (U32 j = 0; j < ntocs; j += 1) if (j != i && list_has_name(&tocs[j].toc.required_deps, tocs[i].folder)) dependents += 1;
            if (dependents > best || (dependents == best && dependents && tocs[i].folder.size < tocs[main].folder.size)) { best = dependents; main = i; }
        }
        // A pack if there's no clear main addon, or most folders don't need it.
        if (main >= 0 && ntocs > 1 && best * 2 < ntocs - 1) main = -1;
    }
    if (ntocs == 1) main = 0;

    for (U32 i = 0; i < ntocs; i += 1) {
        str8_list_push(a, &it->folders, str8_copy(a, tocs[i].folder));
        String8 t = tocs[i].toc.title.size ? tocs[i].toc.title : tocs[i].folder;
        str8_list_push(a, &it->titles, str8_copy(a, t));
    }
    if (main >= 0) {
        Addon *t = &tocs[main].toc;
        it->title = str8_copy(a, t->title.size ? t->title : tocs[main].folder);
        it->version = str8_copy(a, t->version);
        it->interface_version = str8_copy(a, t->interface_version);
    } else {
        it->is_pack = 1;
        it->title = str8_pushf(a, "%u addons", ntocs);
    }
    scratch_end(scratch);
    return 1;
}

//- Cache (the GUI rescans whenever the window is activated)

typedef struct DlCacheEntry { String8 path; U64 size, mtime; B32 is_addon; DownloadItem item; } DlCacheEntry;
static Arena *dl_cache_arena;
static DlCacheEntry *dl_cache;
static U64 dl_cache_count, dl_cache_cap;

static DlCacheEntry *dl_cache_find(String8 path, U64 size, U64 mtime)
{
    for (U64 i = 0; i < dl_cache_count; i += 1) {
        DlCacheEntry *c = &dl_cache[i];
        if (c->size == size && c->mtime == mtime && str8_match(c->path, path, 1)) return c;
    }
    return 0;
}

static DlCacheEntry *dl_cache_add(String8 path, U64 size, U64 mtime)
{
    if (!dl_cache_arena) dl_cache_arena = arena_alloc(GB(1));
    if (dl_cache_count == dl_cache_cap) {
        U64 cap = dl_cache_cap ? dl_cache_cap * 2 : 256;
        DlCacheEntry *v = push_array(dl_cache_arena, DlCacheEntry, cap);
        if (dl_cache_count) memcpy(v, dl_cache, dl_cache_count * sizeof(DlCacheEntry));
        dl_cache = v;
        dl_cache_cap = cap;
    }
    DlCacheEntry *c = &dl_cache[dl_cache_count++];
    memset(c, 0, sizeof(*c));
    c->path = str8_copy(dl_cache_arena, path);
    c->size = size;
    c->mtime = mtime;
    return c;
}

//- Scanning

typedef struct DlFile { String8 path, name; U64 size, mtime; } DlFile;
static B32 dl_profile;

static int dl_file_newest_first(const void *a, const void *b)
{
    const DlFile *x = (const DlFile *)a, *y = (const DlFile *)b;
    return (x->mtime < y->mtime) - (x->mtime > y->mtime);
}

static B32 downloads_is_archive_name(String8 n)
{
    return str8_ends_with(n, str8_lit(".zip"), 1) || str8_ends_with(n, str8_lit(".rar"), 1) || str8_ends_with(n, str8_lit(".7z"), 1);
}

// Finds addon archives in `dirs`, newest first. use_cache: keep results between calls
// (only from one thread, the GUI's worker).
static DownloadList downloads_scan(Arena *a, String8List dirs, B32 use_cache)
{
    DownloadList out = { 0 };
    out.dirs = dirs;
    Temp scratch = scratch_begin(a);
    U64 cap = 4096, n = 0;
    DlFile *files = push_array(scratch.arena, DlFile, cap);
    for (String8Node *d = dirs.first; d; d = d->next) {
        OS_FileInfoArray list = os_list_dir(scratch.arena, d->string);
        for (U64 i = 0; i < list.count && n < cap; i += 1) {
            OS_FileInfo *f = &list.v[i];
            if (f->is_dir || f->size < 22 || f->size > GB(2) || !downloads_is_archive_name(f->name)) continue;
            files[n].path = path_join(scratch.arena, d->string, f->name);
            files[n].name = f->name;
            files[n].size = f->size;
            files[n].mtime = f->mtime;
            n += 1;
        }
    }
    qsort(files, n, sizeof(DlFile), dl_file_newest_first);
    n = Min(n, DL_MAX_ARCHIVES);

    out.v = push_array(a, DownloadItem, n + 1);
    for (U64 i = 0; i < n; i += 1) {
        DlFile *f = &files[i];
        DlCacheEntry *c = use_cache ? dl_cache_find(f->path, f->size, f->mtime) : 0;
        if (!c) {
            DownloadItem it = { 0 };
            it.path = f->path; it.file_name = f->name; it.size = f->size; it.mtime = f->mtime;
            Arena *dst = use_cache ? (dl_cache_arena ? dl_cache_arena : (dl_cache_arena = arena_alloc(GB(1)))) : a;
            U64 pt = os_now_us();
            B32 is_addon = dl_inspect(dst, &it);
            if (dl_profile) printf("  %6.0f ms  %.*s\n", (os_now_us() - pt) / 1000.0, S8F(f->name));
            if (!use_cache) {
                if (is_addon) { it.path = str8_copy(a, it.path); it.file_name = str8_copy(a, it.file_name); out.v[out.count++] = it; }
                continue;
            }
            c = dl_cache_add(f->path, f->size, f->mtime);
            c->is_addon = is_addon;
            c->item = it;
            c->item.path = c->path;
            c->item.file_name = str8_copy(dl_cache_arena, f->name);
        }
        if (c->is_addon) out.v[out.count++] = c->item;
    }
    scratch_end(scratch);
    return out;
}

// Fills state / installed_version by looking at the folders in this flavor's AddOns.
static void downloads_compare(Arena *a, DownloadList *list, String8 addons_dir)
{
    for (U64 i = 0; i < list->count; i += 1) {
        DownloadItem *it = &list->v[i];
        it->installed_count = 0;
        for (String8Node *f = it->folders.first; f; f = f->next) {
            it->installed_count += os_dir_exists(path_join(a, addons_dir, f->string));
        }
        it->installed_version = str8(0, 0);
        if (!it->installed_count) { it->state = DL_NEW; continue; }
        if (it->is_pack) { it->state = it->installed_count == it->folders.count ? DL_INSTALLED : DL_DIFFERENT; continue; }
        // Compare against the main folder (the one named like the title) - first folder with a toc as fallback.
        Addon installed;
        String8 main = it->folders.first->string;
        for (String8Node *f = it->folders.first, *t = it->titles.first; f && t; f = f->next, t = t->next) {
            if (str8_match(t->string, it->title, 0)) { main = f->string; break; }
        }
        if (addon_folder_toc(a, addons_dir, main, &installed)) it->installed_version = installed.version;
        it->state = it->installed_version.size && str8_match(it->installed_version, it->version, 1) && it->installed_count == it->folders.count
                  ? DL_INSTALLED : DL_DIFFERENT;
    }
}

//- "New" archives
//
// An archive counts as new until it has been installed somewhere: through wam (Install, drag and
// drop, install-zip), or found installed while the Downloads list was compared with some flavor.
// Installed archives are remembered by file name + size in %APPDATA%\wam\downloads.json, so the
// mark survives restarts and switching flavors or installs. The Downloads lane and the task worker
// both write it, under dl_seen_lock.

#define DL_SEEN_MAX 1000
static SRWLOCK dl_seen_lock = SRWLOCK_INIT; // the Downloads lane and the task worker both update the file

static String8 dl_seen_path(Arena *a)
{
    String8 settings = wam_settings_path(a);
    return settings.size ? path_join(a, path_dirname(settings), str8_lit("downloads.json")) : settings;
}

static String8 dl_key(Arena *a, String8 file_name, U64 size)
{
    return str8_pushf(a, "%.*s|%llu", S8F(str8_lower(a, file_name)), size);
}

static String8List dl_seen_load(Arena *a)
{
    String8List l = { 0 };
    String8 path = dl_seen_path(a);
    if (!path.size) return l;
    json_each(k, json_get(json_parse(a, os_read_file(a, path)), "installed")) {
        String8 s = json_str(k);
        if (s.size) str8_list_push(a, &l, s);
    }
    return l;
}

static B32 dl_seen_save(String8List *l)
{
    Temp t = scratch_begin(0);
    String8 path = dl_seen_path(t.arena);
    String8List out = { 0 };
    // Newest last; drop the oldest beyond the cap.
    U64 skip = l->count > DL_SEEN_MAX ? l->count - DL_SEEN_MAX : 0, i = 0;
    for (String8Node *n = l->first; n; n = n->next, i += 1) {
        if (i < skip) continue;
        str8_list_push(t.arena, &out, str8_pushf(t.arena, "%s\n    \"%.*s\"", out.count ? "," : "", S8F(json_escape(t.arena, n->string))));
    }
    String8 body = str8_pushf(t.arena, "{\n  \"installed\": [%.*s\n  ]\n}\n", S8F(str8_list_join(t.arena, &out, str8_lit(""))));
    String8 tmp = str8_pushf(t.arena, "%.*s.tmp", S8F(path));
    B32 ok = path.size && os_make_dirs(path_dirname(path)) && os_write_file(tmp, body) && os_replace_file(tmp, path);
    scratch_end(t);
    return ok;
}

static B32 dl_seen_has(String8List *l, String8 key)
{
    for (String8Node *n = l->first; n; n = n->next) if (str8_match(n->string, key, 0)) return 1;
    return 0;
}

// Sets is_new for every archive; ones the compare found installed are remembered too.
static void downloads_mark_new(Arena *a, DownloadList *list)
{
    AcquireSRWLockExclusive(&dl_seen_lock);
    String8List seen = dl_seen_load(a);
    B32 changed = 0;
    for (U64 i = 0; i < list->count; i += 1) {
        DownloadItem *it = &list->v[i];
        String8 key = dl_key(a, it->file_name, it->size);
        B32 known = dl_seen_has(&seen, key);
        if (!known && it->state == DL_INSTALLED) { str8_list_push(a, &seen, key); changed = 1; known = 1; }
        it->is_new = !known;
    }
    if (changed) dl_seen_save(&seen);
    ReleaseSRWLockExclusive(&dl_seen_lock);
}

// An archive was just installed: it's no longer new, in any flavor.
static void downloads_record_installed(String8 archive_path)
{
    Temp t = scratch_begin(0);
    String8 name = path_filename(archive_path);
    OS_FileInfoArray files = os_list_dir(t.arena, path_dirname(archive_path));
    for (U64 i = 0; i < files.count; i += 1) {
        if (files.v[i].is_dir || !str8_match(files.v[i].name, name, 1)) continue;
        AcquireSRWLockExclusive(&dl_seen_lock);
        String8List seen = dl_seen_load(t.arena);
        String8 key = dl_key(t.arena, name, files.v[i].size);
        if (!dl_seen_has(&seen, key)) { str8_list_push(t.arena, &seen, key); dl_seen_save(&seen); }
        ReleaseSRWLockExclusive(&dl_seen_lock);
        break;
    }
    scratch_end(t);
}
