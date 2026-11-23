// gui/tasks.c - the background side of wam's GUI: building snapshots for the views (including the
// provider links per row), running tasks, and the worker / lane threads. Part of main_gui.c's unity build.

//- Provider links

static char *source_names[SRC_COUNT] = { "CurseForge", "Wago", "WoWInterface", "GitHub", "Tukui" };

static int source_of_id(String8 id)
{
    if (id_is(id, "cf:")) return SRC_CURSEFORGE;
    if (id_is(id, "wago:")) return SRC_WAGO;
    if (id_is(id, "wowi:")) return SRC_WOWI;
    if (id_is(id, "gh:")) return SRC_GITHUB;
    if (id_is(id, "tukui:")) return SRC_TUKUI;
    return -1;
}

// A .toc id we'd put in a URL: letters, digits, '-', '_' only.
static B32 toc_id_ok(String8 s, B32 digits_only)
{
    if (!s.size || s.size > 64) return 0;
    for (U64 i = 0; i < s.size; i += 1) {
        U8 c = s.str[i];
        B32 digit = c >= '0' && c <= '9';
        if (digits_only ? !digit : !(digit || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_')) return 0;
    }
    return 1;
}

static String8 source_page(Arena *a, int src, String8 id)
{
    switch (src) {
        case SRC_CURSEFORGE: return str8_pushf(a, "https://www.curseforge.com/projects/%.*s", S8F(id)); // redirects to the addon page
        case SRC_WAGO:       return str8_pushf(a, "https://addons.wago.io/addons/%.*s", S8F(id));
        case SRC_WOWI:       return str8_pushf(a, "https://www.wowinterface.com/downloads/info%.*s", S8F(id));
        case SRC_GITHUB:     return str8_pushf(a, "https://github.com/%.*s", S8F(id));
        case SRC_TUKUI:      return str8_pushf(a, "https://tukui.org/%.*s", S8F(id));
    }
    return str8(0, 0);
}

static void row_add_link(Row *r, int src, String8 url)
{
    if (src < 0 || !str8_starts_with(url, str8_lit("https://"), 1) || r->link_count >= ArrayCount(r->links)) return;
    for (U32 i = 0; i < r->link_count; i += 1) if (r->link_src[i] == src) return;
    r->link_src[r->link_count] = src;
    r->links[r->link_count++] = url;
}

// The tracked source first (its stored website, else built from the id), then every other site the
// addon's .toc says it's published on.
static void row_build_links(Arena *a, Row *r, MgrAddon *e, Addon *ad)
{
    if (e) {
        int src = source_of_id(e->id);
        String8 url = e->website.size ? e->website : source_page(a, src, id_rest(e->id));
        row_add_link(r, src, url);
        r->link_primary = r->link_count == 1;
    }
    if (ad) {
        if (toc_id_ok(ad->curse_project_id, 1)) row_add_link(r, SRC_CURSEFORGE, source_page(a, SRC_CURSEFORGE, ad->curse_project_id));
        if (toc_id_ok(ad->wago_id, 0))          row_add_link(r, SRC_WAGO, source_page(a, SRC_WAGO, ad->wago_id));
        if (toc_id_ok(ad->wowi_id, 1))          row_add_link(r, SRC_WOWI, source_page(a, SRC_WOWI, ad->wowi_id));
    }
}

// The CurseForge ids of this flavor's installed addons: tracked ones, and ones a .toc names.
static S64 *installed_cf_ids(Arena *a, Mgr *m, String8 addons_dir, U64 *count)
{
    AddonArray scan = addons_scan(a, addons_dir);
    S64 *ids = push_array(a, S64, m->count + scan.count + 1);
    U64 n = 0;
    for (U64 i = 0; i < m->count; i += 1) if (id_is(m->addons[i].id, "cf:")) ids[n++] = id_number(m->addons[i].id);
    for (U64 i = 0; i < scan.count; i += 1) if (toc_id_ok(scan.v[i].curse_project_id, 1)) ids[n++] = str8_to_s64(scan.v[i].curse_project_id);
    *count = n;
    return ids;
}

// Catalog ids that may describe a row: its tracked id, then the ones its .toc names.
static void row_add_cat_id(Row *r, String8 id) { if (id.size && r->cat_id_count < ArrayCount(r->cat_ids)) r->cat_ids[r->cat_id_count++] = id; }

static Snapshot *build_snapshot(Arena *a, Mgr *m, AddonArray *scan, int flavor, F64 scan_ms)
{
    Snapshot *s = push_struct(a, Snapshot);
    s->arena = a;
    s->flavor = flavor;
    s->addons_dir = m->addons_dir;
    s->client = m->client;
    s->scan_ms = scan_ms;
    s->folders = scan->count;
    MgrUnits units = mgr_units(a, m, scan, 1); // sorted: updates first, then by name; rows keep that order
    s->rows = push_array(a, Row, units.count + 1);
    for (U64 i = 0; i < units.count; i += 1) {
        MgrUnit *u = &units.v[i];
        Row *r = &s->rows[s->row_count++];
        r->key = u->key;
        r->title = u->title;
        r->req = u->req;
        r->req_count = u->req_count;
        Addon *ad = u->addon;
        if (ad) { r->iface = best_interface(ad->interface_version, m->client); r->author = ad->author; r->notes = ad->notes; }
        row_build_links(a, r, u->entry, ad);
        if (u->entry && !id_is(u->entry->id, "gh:")) row_add_cat_id(r, u->entry->id);
        if (ad && toc_id_ok(ad->curse_project_id, 1)) row_add_cat_id(r, str8_pushf(a, "cf:%.*s", S8F(ad->curse_project_id)));
        if (ad && toc_id_ok(ad->wowi_id, 1))          row_add_cat_id(r, str8_pushf(a, "wowi:%.*s", S8F(ad->wowi_id)));
        if (ad && toc_id_ok(ad->wago_id, 0))          row_add_cat_id(r, str8_pushf(a, "wago:%.*s", S8F(ad->wago_id)));
        if (u->entry) {
            MgrAddon *e = u->entry;
            r->id = e->id;
            r->sub = str8_list_join(a, &e->folders, str8_lit(", "));
            r->version = e->version;
            r->update = mgr_has_update(e);
            r->edited = r->update && mgr_is_edited(m, e);
            r->pinned = e->pinned;
            r->latest = e->latest_version;
            s->tracked += 1;
            s->updates += r->update && !r->pinned; // what Update all would update
        } else {
            r->folder = ad->folder;
            r->sub = ad->folder;
            r->version = ad->version;
        }
    }
    return s;
}

static String8List split_lines(Arena *a, String8 text)
{
    String8List l = { 0 };
    while (text.size) {
        String8 line = str8_trim(str8_pop_line(&text));
        if (line.size) str8_list_push(a, &l, line);
    }
    return l;
}

// One part of a catalog refresh (TASK_CATALOG), on its own thread: CurseForge, WoWInterface and
// Tukui, or Wago. The first goes to the list at once when the second is still coming (post_early).
typedef struct CatPart { U32 part; B32 post_early; int flavor; U32 gen; Mgr m; Catalog cat; String8 error; } CatPart;

static void cat_part_fetch(void *user, U64 i)
{
    CatPart *p = &((CatPart *)user)[i];
    cat_fetch_parts(&p->m, &p->cat, &p->error, p->part);
    if (!p->post_early || !p->cat.count) return;
    Arena *ca = arena_alloc(GB(1));
    CatalogSnap *cs = push_struct(ca, CatalogSnap); // a copy: the UI owns (and releases) it
    cs->arena = ca; cs->flavor = p->flavor; cs->gen = p->gen; cs->saved = cat_now();
    cat_init(&cs->cat, ca);
    for (U64 k = 0; k < p->cat.count; k += 1) cat_put(&cs->cat, &p->cat.v[k]);
    wm_post(on_catalog, 0, cs);
}

static void run_task(Task *t)
{
    Arena *a = arena_alloc(GB(4));
    String8 wow_root = str8_copy(a, str8_cstring(t->wow_root));
    String8 addons_dir = wow_resolve_addons_dir(a, wow_root, wow_flavors[t->flavor].folder);
    if (!addons_dir.size) {
        worker_set_status(str8_lit("No AddOns folder for this flavor"), 1);
        arena_release(a);
        return;
    }
    Mgr *m = mgr_open(a, addons_dir, app.api_key);
    m->wago_key = str8_copy(a, str8_cstring(t->wago_key));
    os_secure_zero(t->wago_key, sizeof(t->wago_key));
    m->status = worker_status_cb;
    m->progress = worker_progress_cb;
    if (m->state_unreadable && t->kind != TASK_SEARCH && t->kind != TASK_SUGGEST && t->kind != TASK_CATALOG && t->kind != TASK_DETAILS && t->kind != TASK_CHANGES) {
        worker_set_status(str8_pushf(a, "%.*s is damaged, so wam won't change it. Fix it, or delete it and click Check for updates.", S8F(m->state_path)), 1);
        t->kind = TASK_REFRESH; // still show what's on disk
    }

    if (t->kind == TASK_SCAN_DOWNLOADS) {
        // Archives in the Downloads folder(s); inspection results are cached across scans.
        DownloadsSnap *ds = push_struct(a, DownloadsSnap);
        ds->arena = a;
        ds->flavor = t->flavor;
        ds->gen = t->gen;
        ds->client = m->client;
        ds->list = downloads_scan(a, downloads_dirs(a), 1);
        downloads_compare(a, &ds->list, addons_dir);
        downloads_mark_new(a, &ds->list);
        wm_post(on_downloads, 0, ds);
        return;
    }

    if (t->kind == TASK_SEARCH || t->kind == TASK_SUGGEST) {
        B32 quiet = t->kind == TASK_SUGGEST; // search as you type: no status messages, errors just mean no suggestions
        if (quiet) m->status = 0;
        SearchSnap *ss = push_struct(a, SearchSnap);
        ss->arena = a;
        ss->flavor = t->flavor;
        ss->gen = t->gen;
        ss->query = str8_copy(a, str8_cstring(t->text));
        ss->sort = t->sort;
        ss->index = t->index;
        ss->category = t->category;
        static const int cf_sorts[SORT_COUNT] = { CF_SORT_POPULARITY, CF_SORT_DOWNLOADS, CF_SORT_UPDATED, CF_SORT_RELEASED };
        ss->v = mgr_search_page(m, ss->query, cat_cf_query(a, t->category), cf_sorts[t->sort % SORT_COUNT], t->index, &ss->count, &ss->cf_count);
        ss->failed = !ss->v;
        if (!ss->v && !quiet) worker_set_status(m->error, 1);
        wm_post(on_search, 0, ss);
        return;
    }

    if (t->kind == TASK_DETAILS) {
        m->status = 0;
        DetailSnap *ds = push_struct(a, DetailSnap);
        ds->arena = a; ds->flavor = t->flavor; ds->gen = t->gen;
        ds->d = cat_details(m, str8_copy(a, str8_cstring(t->text)));
        wm_post(on_details, 0, ds);
        return;
    }

    if (t->kind == TASK_CHANGES) {
        m->status = 0;
        ChangesSnap *cs = push_struct(a, ChangesSnap);
        cs->arena = a; cs->flavor = t->flavor; cs->gen = t->gen;
        cs->c = mgr_changes(m, str8_copy(a, str8_cstring(t->text)));
        wm_post(on_changes, 0, cs);
        return;
    }

    if (t->kind == TASK_CATALOG) {
        // The cached catalog first (instant), then, when it's missing, old or for another game
        // version, a fresh one from the network. Each goes to the UI as it's ready. Installed
        // CurseForge addons the catalog doesn't have (it prefetches the popular ones) are fetched
        // too, so Installed can show their pictures and summaries.
        m->status = 0;
        String8 path = cat_cache_path(a, wow_flavors[t->flavor].folder);
        B32 fresh = 0;
        U64 nwant = 0;
        S64 *want = installed_cf_ids(a, m, addons_dir, &nwant);
        if (!t->force && path.size) {
            Arena *ca = arena_alloc(GB(1));
            CatalogSnap *cs = push_struct(ca, CatalogSnap);
            cs->arena = ca; cs->flavor = t->flavor; cs->gen = t->gen;
            cat_init(&cs->cat, ca);
            if (cat_parse(&cs->cat, os_read_file(ca, path), m->client, &cs->saved) && cs->cat.count) {
                S64 age = cat_now() - cs->saved;
                // Without Wago's lists, a catalog is stale once there's a Wago key (one was just added).
                fresh = age >= 0 && age < CAT_MAX_AGE && (!m->wago_key.size || cs->cat.has_wago);
                // Which installed ones it lacks, decided before the UI owns it.
                U64 nmissing = 0;
                for (U64 i = 0; i < nwant; i += 1) if (!cat_find(&cs->cat, str8_pushf(a, "cf:%lld", want[i]))) want[nmissing++] = want[i];
                nwant = nmissing;
                cs->final = fresh && !nwant;
                S64 saved = cs->saved;
                wm_post(on_catalog, 0, cs);
                if (fresh && nwant) {
                    // Just those, then into the cache too (reread: the UI has the copy above).
                    CatalogSnap *top = push_struct(a, CatalogSnap);
                    top->arena = a; top->flavor = t->flavor; top->gen = t->gen; top->final = 1; top->saved = saved;
                    cat_init(&top->cat, a);
                    if (cat_add_cf_mods(m, &top->cat, want, nwant)) {
                        Arena *ra = arena_alloc(GB(1));
                        Catalog merged = { 0 };
                        cat_init(&merged, ra);
                        if (cat_parse(&merged, os_read_file(ra, path), m->client, 0)) {
                            for (U64 i = 0; i < top->cat.count; i += 1) cat_put(&merged, &top->cat.v[i]);
                            cat_save(&merged, path, m->client, saved);
                        }
                        arena_release(ra);
                    }
                    wm_post(on_catalog, 0, top);
                    return;
                }
            } else {
                arena_release(ca);
            }
        }
        if (!fresh) {
            // Wago answers slowest (about two seconds, against half a second for the rest): it's
            // fetched beside the rest, which goes to the list as soon as it's in. Then the two
            // together are saved and go to the list again.
            CatalogSnap *cs = push_struct(a, CatalogSnap);
            cs->arena = a; cs->flavor = t->flavor; cs->gen = t->gen; cs->final = 1;
            cat_init(&cs->cat, a);
            CatPart parts[2] = { { CAT_PART_CORE, m->wago_key.size != 0, t->flavor, t->gen }, { CAT_PART_WAGO, 0, t->flavor, t->gen } };
            U64 nparts = m->wago_key.size ? 2 : 1;
            for (U64 i = 0; i < nparts; i += 1) {
                parts[i].m = *m;
                parts[i].m.arena = arena_alloc(GB(1)); // each part on its own thread, with its own arena
                cat_init(&parts[i].cat, parts[i].m.arena);
            }
            os_parallel_for_threads(nparts, cat_part_fetch, parts, 2);
            for (U64 i = 0; i < nparts; i += 1) {
                for (U64 k = 0; k < parts[i].cat.count; k += 1) cat_put(&cs->cat, &parts[i].cat.v[k]);
                if (parts[i].cat.has_wago) cs->cat.has_wago = 1;
                if (!cs->error.size) cs->error = str8_copy(a, parts[i].error);
            }
            if (nparts == 2 && !cs->cat.has_wago) cat_keep_wago(&cs->cat, os_read_file(a, path), m->client);
            for (U64 i = 0; i < nparts; i += 1) arena_release(parts[i].m.arena);
            if (parts[0].cat.count) {
                cat_add_cf_mods(m, &cs->cat, want, nwant);
                cs->saved = cat_now();
                cat_save(&cs->cat, path, m->client, cs->saved);
            }
            wm_post(on_catalog, 0, cs); // the UI releases the arena
        } else {
            arena_release(a);
        }
        return;
    }

    B32 ok = 1;
    String8 text = str8_copy(a, str8_cstring(t->text));
    switch (t->kind) {
        case TASK_INSTALL: {
            String8 id = { 0 };
            ok = mgr_install(m, text, &id);
            MgrAddon *e = ok ? mgr_find(m, id) : 0;
            if (e) worker_set_status(str8_pushf(a, "Installed %.*s %.*s", S8F(e->name), S8F(e->version)), 0);
        } break;
        case TASK_INSTALL_FILE: {
            String8List folders = { 0 };
            ok = mgr_install_local_archive(m, text, &folders);
            if (ok) {
                downloads_record_installed(text); // no longer "new" in the Downloads list
                worker_set_status(str8_pushf(a, "Installed %.*s from %.*s", S8F(str8_list_join(a, &folders, str8_lit(", "))),
                                             S8F(path_filename(text))), 0);
                if (m->api_key.size || m->wago_key.size) t->kind = TASK_SYNC; // recognize it so update checks work
            }
        } break;
        case TASK_INSTALL_DOWNLOADED: {
            // The archive the browser saved from a CurseForge download page: install it as that file.
            String8List f = split_lines(a, text);
            String8 v[7] = { 0 };
            int k = 0;
            for (String8Node *n = f.first; n && k < 7; n = n->next) v[k++] = n->string;
            for (int j = 4; j < 6; j += 1) if (str8_match(v[j], str8_lit("-"), 0)) v[j] = str8(0, 0); // "-" = none
            RemoteFile rf = { 0 };
            rf.ok = 1; rf.id = v[1]; rf.file_id = str8_to_s64(v[2]); rf.name = v[3]; rf.version = v[4]; rf.website = v[5];
            B32 temp_file = str8_match(v[6], str8_lit("delete"), 0); // from the download window's temp folder
            ok = k >= 6 && mgr_install_downloaded(m, &rf, v[0]);
            if (temp_file) os_delete_tree(v[0]);
            if (ok) {
                if (!temp_file) downloads_record_installed(v[0]);
                worker_set_status(str8_pushf(a, "Installed %.*s %.*s (downloaded from CurseForge)", S8F(rf.name), S8F(rf.version)), 0);
            }
        } break;
        case TASK_UPDATE: {
            String8List ids = split_lines(a, text);
            ok = mgr_update_list_ex(m, &ids, t->force);
        } break;
        case TASK_PIN: ok = mgr_set_pinned(m, str8_trim(text), t->force); break;
        case TASK_REMOVE: {
            String8List keys = split_lines(a, text);
            ok = mgr_remove_many(m, &keys);
        } break;
        default: break;
    }
    if (!ok) worker_set_status_url(m->error, 1, m->error_url);
    if (m->manual_count) {
        // Files only CurseForge's own page may serve: hand them to the UI, which opens the page (or offers
        // to) and installs each archive when the browser saves it.
        PendingList *pl = (PendingList *)os_alloc(sizeof(PendingList));
        if (pl) {
            pl->flavor = t->flavor;
            pl->gen = t->gen;
            for (U32 i = 0; i < m->manual_count && pl->count < ArrayCount(pl->v); i += 1) {
                RemoteFile *rf = &m->manual[i];
                PendingDownload *p = &pl->v[pl->count++];
                #define PD_COPY(dst, s8) do { U64 n_ = Min((s8).size, sizeof(dst) - 1); memcpy(dst, (s8).str, n_); dst[n_] = 0; } while (0)
                PD_COPY(p->id, rf->id); PD_COPY(p->name, rf->name); PD_COPY(p->version, rf->version);
                PD_COPY(p->website, rf->website); PD_COPY(p->file, rf->manual_file); PD_COPY(p->url, rf->manual_url);
                #undef PD_COPY
                p->file_id = rf->file_id;
                p->size = rf->manual_size;
                p->flavor = t->flavor;
                p->gen = t->gen;
            }
            if (!wm_post(on_pending, 0, pl)) os_free(pl);
        }
    }

    U64 t0 = os_now_us();
    AddonArray scan = addons_scan(a, addons_dir);
    F64 scan_ms = (F64)(os_now_us() - t0) / 1000.0;
    if (mgr_prune(m) && t->kind != TASK_REFRESH) mgr_save(m); // listings only read (they run beside other tasks)
    B32 checked = t->kind == TASK_SYNC && mgr_sync(m, &scan);
    if (t->kind == TASK_SYNC && !checked) worker_set_status(m->error, 1);

    Snapshot *snap = build_snapshot(a, m, &scan, t->flavor, scan_ms);
    snap->gen = t->gen;
    snap->checked = checked ? (U64)cat_now() : 0;
    snap->seq = t->seq ? t->seq : os_atomic_inc(&snap_seq); // listings took theirs before reading
    wm_post(on_snapshot, 0, snap);
}

static U32 lane_main(void *p)
{
    Lane *lane = (Lane *)p;
    for (;;) {
        os_event_wait(lane->event);
        os_mutex_lock(&lane->lock);
        B32 have = lane->full;
        Task t = lane->slot;
        U32 id = lane->id;
        lane->full = 0;
        os_mutex_unlock(&lane->lock);
        if (!have) continue;
        t.seq = os_atomic_inc(&snap_seq); // taken before reading the state
        run_task(&t);
        if (lane->done) wm_post(lane->done, id, 0);
    }
}

// done (optional): told on the UI thread which request the lane just answered.
static void lane_start(Lane *lane, WM_PostFunc *done)
{
    lane->event = os_event_alloc();
    lane->done = done;
    os_thread_start(lane_main, lane);
}

// %LOCALAPPDATA%\wam\tasks.log: one line per queued task, how long it waited behind the ones before
// it and how long it ran, so a slow moment can be explained afterwards. Kept under 256 KB (the
// previous one becomes tasks.old.log). Only timings, kinds and addon ids or file names: no keys.
static void task_log(Task *t, U64 start_us, U64 end_us)
{
    static char *kinds[] = { "refresh", "sync", "install", "install-file", "update", "remove", "search", "scan-downloads",
                             "suggest", "install-downloaded", "catalog", "details", "pin", "changes" };
    Temp scratch = scratch_begin(0);
    Arena *a = scratch.arena;
    String8 local = os_get_env(a, "LOCALAPPDATA");
    if (local.size) {
        String8 dir = path_join(a, local, str8_lit("wam")), path = path_join(a, dir, str8_lit("tasks.log"));
        if (os_file_size(path) > 256 * 1024) os_replace_file(path, path_join(a, dir, str8_lit("tasks.old.log")));
        OS_DateTime st = os_local_now();
        String8 what = str8_cstring(t->text);
        U64 nl = str8_find(what, 0, str8_lit("\n"), 0), lines = 1;
        for (U64 k = 0; k < what.size; k += 1) lines += what.str[k] == '\n';
        if (t->kind == TASK_INSTALL_DOWNLOADED) what = path_filename(str8_prefix(what, nl)); // a path, then the file's details
        else if (nl < what.size) what = str8_pushf(a, "%.*s (+%llu more)", (int)nl, what.str, lines - 1);
        String8 line = str8_pushf(a, "%04u-%02u-%02u %02u:%02u:%02u  #%-4u %-18s waited %6.2f s  ran %6.2f s  %.*s\r\n",
                                  st.year, st.month, st.day, st.hour, st.minute, st.second, t->id,
                                  (U32)t->kind < ArrayCount(kinds) ? kinds[t->kind] : "?",
                                  (F64)(start_us - t->queued_us) / 1e6, (F64)(end_us - start_us) / 1e6, (int)Min(what.size, 200), what.str);
        os_make_dirs(dir);
        os_append_file(path, line);
    }
    scratch_end(scratch);
}

static U32 worker_main(void *p)
{
    (void)p;
    for (;;) {
        os_semaphore_wait(task_sem);
        os_mutex_lock(&task_lock);
        Task t = task_q[task_head++ % TASK_QUEUE];
        os_mutex_unlock(&task_lock);
        U64 start = os_now_us();
        os_atomic_store(&work_permille, -1);
        os_atomic_store(&work_task, (S32)t.id);
        run_task(&t);
        os_atomic_store(&work_task, 0);
        os_atomic_store(&work_finished, (S32)t.id);
        task_log(&t, start, os_now_us());
        wm_post(on_task_done, t.id, 0);
    }
}


//- Pictures for the UI library's ui_image (icons, screenshot thumbnails)
//
// Cached on disk in %LOCALAPPDATA%\wam\images, one file per URL (named by its hash); files older
// than 30 days are deleted at startup (and fetched again when needed).

static String8 image_cache_dir(Arena *a)
{
    String8 local = os_get_env(a, "LOCALAPPDATA");
    return local.size ? path_join(a, path_join(a, local, str8_lit("wam")), str8_lit("images")) : local;
}

static String8 image_fetch(Arena *a, String8 url)
{
    String8 dir = image_cache_dir(a);
    String8 path = dir.size ? path_join(a, dir, str8_pushf(a, "%016llx", ui_image_key(url))) : dir;
    if (path.size) {
        String8 data = os_read_file(a, path);
        if (data.size) return data;
    }
    HTTP_Result r = http_get_url(a, url, str8(0, 0), 0, 0);
    if (!r.ok || !r.body.size) return str8(0, 0);
    if (path.size && os_make_dirs(dir)) os_write_file(path, r.body);
    return r.body;
}

static U32 image_cache_prune(void *p)
{
    (void)p;
    Arena *a = arena_alloc(GB(1));
    String8 dir = image_cache_dir(a);
    OS_FileInfoArray files = dir.size ? os_list_dir(a, dir) : (OS_FileInfoArray){ 0 };
    U64 now = (U64)cat_now();
    for (U64 i = 0; i < files.count; i += 1) {
        if (files.v[i].is_dir || now - files.v[i].mtime < 30ull * 86400) continue;
        os_delete_file(path_join(a, dir, files.v[i].name));
    }
    arena_release(a);
    return 0;
}
