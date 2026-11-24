// manager.c - install / update / remove / sync (the GUI runs them on its worker thread).
//
// Addons are identified by a source-prefixed id:
//   cf:61284        CurseForge project (needs an API key)
//   gh:owner/repo   GitHub releases (no key)
//   wowi:5108       WoWInterface (no key)
//   wago:aNDmy96o   Wago Addons (the user's own Wago API key)
//
// State lives in <flavor>\Interface\wam.json (next to AddOns) so it travels with
// the install. Each operation is synchronous; the GUI runs them on a worker thread.
// Everything a Mgr allocates lives in its arena, so callers use one arena per task.

#define MGR_MAX_ADDONS 2048

typedef struct MgrAddon {
    String8 id;
    S64 file_id;              // CF file id / GitHub asset id / WoWI upload time / Wago release time: grows with each release
    String8 name;
    String8 version;
    String8 website;
    String8List folders;
    String8List checks;       // "Folder|sig:hash" per folder, recorded at install (see mgr_record_checks)
    S64 latest_file_id;      // best file for this game version, from the last check
    String8 latest_version;
    B32 pinned;               // kept at this version: updates skip it (it's still checked, to show what's new)
} MgrAddon;

// Files' MD5s known without reading them (from the archive they were just unpacked from), sorted by
// rel: "<folder>/<path>", lowercase, '/' separators (folder_check's rel with its folder in front).
typedef struct KnownHash { String8 rel; char md5[33]; } KnownHash;
typedef struct KnownHashes { KnownHash *v; U64 count; } KnownHashes;
static int known_hash_compare(const void *pa, const void *pb);

typedef void MgrStatusFunc(void *user, String8 msg);
// How far the current install is, 0..1 (negative: under way, but how far isn't known). Called often.
typedef void MgrProgressFunc(void *user, F32 fraction);

typedef struct Mgr {
    Arena *arena;
    String8 addons_dir;
    String8 state_path;
    String8 api_key;          // CurseForge; may be empty
    String8 wago_key;         // Wago; may be empty (set after mgr_open)
    GameVersion client;
    MgrAddon *addons;
    U64 count;
    MgrStatusFunc *status;
    void *status_user;
    MgrProgressFunc *progress; // optional; gets status_user too
    String8 error;
    B32 state_unreadable;     // wam.json exists but couldn't be parsed: never overwrite it
    String8 last_installed_id; // id of the addon the last successful install recorded
    String8 error_url;         // with an error: where the user can download the file by hand
    KnownHashes unpacked;      // the files the last zip install unpacked, hashed from memory (mgr_record_checks)
    RemoteFile manual[16];     // files that failed because only CurseForge's own page may serve them (manual_url set)
    U32 manual_count;
    B32 all_shots;             // collect every screenshot (an addon's details), not just the first few (the catalog)
} Mgr;

typedef struct SearchResult {
    String8 id;     // cf:123, wago:abc, wowi:5108, tukui:elvui
    char *source;   // "CurseForge" / "Wago" / "WoWInterface" / "Tukui" (static strings)
    String8 name, summary, author, website;
    S64 downloads;
    S64 updated;    // latest release, unix seconds (0 = unknown)
    S64 created;    // first published, unix seconds (0 = unknown)
    B32 available;  // has a file for this game version
    // CurseForge only: the author lets only CurseForge's own site serve the files. Installing then
    // uses the GitHub releases its source link points to when they're current, else the download page.
    U8 site_only;   // 0 no, SITE_ONLY_GITHUB (has a GitHub source link), SITE_ONLY_PAGE (no other way)
    U32 cats;       // categories: bit i = cat_categories[i] (catalog.c)
    U64 subcats;    // subcategories: bit i = cat_subcategories[i]
    String8 icon;   // https URL of a square-ish picture, or empty
    String8 shots;  // screenshots: space-separated "thumb" or "thumb|full" https URLs
} SearchResult;

#define SITE_ONLY_GITHUB 1
#define SITE_ONLY_PAGE   2
static U32 cat_cf_bits(JSON *categories, U64 *subs);
static U32 cat_wowi_bits(S64 wowi_cat, U64 *subs);
static U32 cat_wago_bits(JSON *categories, U64 *subs);
static String8 shots_join(Arena *a, String8List *l);

static B32 mgr_install_spec(Mgr *m, String8 spec, int depth);

//- Helpers

static B32 versions_equal(String8 a, String8 b);
static B32 id_is(String8 id, char *prefix);

// GitHub (read without the API) and Tukui have no ids that grow with each release: there an update
// is a different version. Everything else compares file ids.
static B32 mgr_has_update(MgrAddon *e)
{
    if (id_is(e->id, "gh:") || id_is(e->id, "tukui:")) return e->latest_version.size && e->version.size && !versions_equal(e->latest_version, e->version);
    return e->latest_file_id > e->file_id;
}
static B32 id_is(String8 id, char *prefix) { return str8_starts_with(id, str8_cstring(prefix), 1); }
static S64 id_number(String8 id) { U64 c = str8_find(id, 0, str8_lit(":"), 0); return str8_to_s64(str8_skip(id, c + 1)); }
static String8 id_rest(String8 id) { U64 c = str8_find(id, 0, str8_lit(":"), 0); return str8_skip(id, c + 1); }

// An install's progress, for a progress bar: finding the file 0-5%, downloading 5-50%, unpacking
// 50-80%, recording what was installed 80-97%. May be called from the unpacking threads.
static void mgr_progress(Mgr *m, F32 fraction) { if (m->progress) m->progress(m->status_user, fraction); }

static void mgr_status(Mgr *m, char *fmt, ...)
{
    if (!m->status) return;
    Temp scratch = scratch_begin(m->arena);
    va_list args;
    va_start(args, fmt);
    String8 s = str8_pushfv(scratch.arena, fmt, args);
    va_end(args);
    m->status(m->status_user, s);
    scratch_end(scratch);
}

static B32 mgr_fail(Mgr *m, char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    m->error = str8_pushfv(m->arena, fmt, args);
    va_end(args);
    return 0;
}

static MgrAddon *mgr_find(Mgr *m, String8 id)
{
    for (U64 i = 0; i < m->count; i += 1) if (str8_match(m->addons[i].id, id, 1)) return &m->addons[i];
    return 0;
}

static MgrAddon *mgr_owner_of(Mgr *m, String8 folder)
{
    for (U64 i = 0; i < m->count; i += 1) {
        for (String8Node *n = m->addons[i].folders.first; n; n = n->next) {
            if (str8_match(n->string, folder, 1)) return &m->addons[i];
        }
    }
    return 0;
}

static MgrAddon *mgr_add(Mgr *m)
{
    if (m->count >= MGR_MAX_ADDONS) return 0;
    MgrAddon *e = &m->addons[m->count++];
    memset(e, 0, sizeof(*e));
    return e;
}

static void mgr_delete_entry(Mgr *m, MgrAddon *e)
{
    U64 i = (U64)(e - m->addons);
    memmove(&m->addons[i], &m->addons[i + 1], (m->count - i - 1) * sizeof(MgrAddon));
    m->count -= 1;
}

static B32 list_has_ci(String8List *l, String8 s)
{
    for (String8Node *n = l->first; n; n = n->next) if (str8_match(n->string, s, 1)) return 1;
    return 0;
}

static String8List list_without_ci(Arena *a, String8List *l, String8List *remove)
{
    String8List r = { 0 };
    for (String8Node *n = l->first; n; n = n->next) if (!list_has_ci(remove, n->string)) str8_list_push(a, &r, n->string);
    return r;
}

// True when `only` is null/empty (= everything) or contains id.
static B32 id_selected(String8List *only, String8 id)
{
    if (!only || !only->count) return 1;
    return list_has_ci(only, id);
}

static B32 folder_name_is_safe(String8 f)
{
    if (!f.size || f.str[0] == '.') return 0;
    for (U64 i = 0; i < f.size; i += 1) if (f.str[i] == '\\' || f.str[i] == '/' || f.str[i] == ':') return 0;
    return 1;
}

static B32 mgr_require_key(Mgr *m)
{
    if (m->api_key.size) return 1;
    return mgr_fail(m, "CurseForge needs your own API key: add it in Settings, or install from GitHub / WoWInterface instead.");
}

static B32 mgr_require_wago(Mgr *m)
{
    if (m->wago_key.size) return 1;
    return mgr_fail(m, "Wago needs your own Wago API key (from a Wago subscription). Add it in wam's Settings (the cog at the top).");
}

//- Parsing what the user typed into an addon id

static B32 is_digits(String8 s)
{
    if (!s.size) return 0;
    for (U64 i = 0; i < s.size; i += 1) if (s.str[i] < '0' || s.str[i] > '9') return 0;
    return 1;
}

static B32 is_slug(String8 s)
{
    if (!s.size) return 0;
    for (U64 i = 0; i < s.size; i += 1) {
        U8 c = s.str[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return 0;
    }
    return 1;
}

static String8 take_path_part(String8 s)
{
    U64 i = 0;
    while (i < s.size && s.str[i] != '/' && s.str[i] != '?' && s.str[i] != '#') i += 1;
    return str8_prefix(s, i);
}

// Accepts: 12345, cf:12345, gh:owner/repo, owner/repo, wowi:5108, and GitHub /
// WoWInterface / CurseForge URLs. Returns a normalized id, "cfslug:<slug>" for a
// CurseForge URL (resolved later), or empty if it isn't recognizable.
static String8 mgr_parse_spec(Arena *a, String8 in)
{
    in = str8_trim(in);
    while (in.size && in.str[in.size - 1] == '/') in.size -= 1;
    if (!in.size) return in;
    if (is_digits(in)) return str8_pushf(a, "cf:%.*s", S8F(in));
    if (id_is(in, "cf:") && is_digits(id_rest(in))) return str8_pushf(a, "cf:%.*s", S8F(id_rest(in)));
    if (id_is(in, "wowi:") && is_digits(id_rest(in))) return str8_pushf(a, "wowi:%.*s", S8F(id_rest(in)));
    if (id_is(in, "tukui:") && id_rest(in).size && is_slug(id_rest(in))) return str8_pushf(a, "tukui:%.*s", S8F(str8_lower(a, id_rest(in))));
    if (id_is(in, "wago:") && id_rest(in).size && str8_find(id_rest(in), 0, str8_lit("/"), 0) == id_rest(in).size) return str8_pushf(a, "wago:%.*s", S8F(id_rest(in)));
    if (id_is(in, "gh:")) in = id_rest(in);

    String8 lower = str8_lower(a, in);
    U64 p = str8_find(lower, 0, str8_lit("github.com/"), 0);
    if (p < lower.size) {
        String8 rest = str8_skip(in, p + 11);
        String8 owner = take_path_part(rest);
        String8 repo = take_path_part(str8_skip(rest, owner.size + 1));
        if (str8_ends_with(repo, str8_lit(".git"), 1)) repo = str8_chop(repo, 4);
        if (owner.size && repo.size) return str8_pushf(a, "gh:%.*s/%.*s", S8F(owner), S8F(repo));
        return str8(0, 0);
    }
    if (str8_find(lower, 0, str8_lit("wowinterface.com"), 0) < lower.size) {
        static char *markers[] = { "info", "id=", "file" };
        for (int k = 0; k < (int)ArrayCount(markers); k += 1) {
            String8 mk = str8_cstring(markers[k]);
            for (U64 i = str8_find(lower, 0, mk, 0); i < lower.size; i = str8_find(lower, i + 1, mk, 0)) {
                U64 j = i + mk.size, start = j;
                while (j < lower.size && lower.str[j] >= '0' && lower.str[j] <= '9') j += 1;
                if (j > start) return str8_pushf(a, "wowi:%.*s", S8F(str8_substr(lower, start, j)));
            }
        }
        return str8(0, 0);
    }
    p = str8_find(lower, 0, str8_lit("tukui.org/"), 0);
    if (p < lower.size) {
        // tukui.org/elvui, tukui.org/download.php?ui=elvui, api.tukui.org/v1/download/elvui/...
        String8 rest = str8_skip(lower, p + 10);
        if (str8_starts_with(rest, str8_lit("download.php?ui="), 0)) rest = str8_skip(rest, 16);
        else if (str8_starts_with(rest, str8_lit("v1/download/"), 0)) rest = str8_skip(rest, 12);
        else if (str8_starts_with(rest, str8_lit("v1/addon/"), 0)) rest = str8_skip(rest, 9);
        String8 slug = { 0 };
        U64 i = 0;
        while (i < rest.size && is_slug(str8_substr(rest, i, i + 1))) i += 1;
        slug = str8_prefix(rest, i);
        return slug.size ? str8_pushf(a, "tukui:%.*s", S8F(slug)) : str8(0, 0);
    }
    p = str8_find(lower, 0, str8_lit("addons.wago.io/addons/"), 0);
    if (p < lower.size) {
        String8 slug = take_path_part(str8_skip(in, p + 22));
        return slug.size ? str8_pushf(a, "wagoslug:%.*s", S8F(slug)) : str8(0, 0);
    }
    p = str8_find(lower, 0, str8_lit("curseforge.com/wow/addons/"), 0);
    if (p < lower.size) {
        String8 slug = take_path_part(str8_skip(lower, p + 26));
        return slug.size ? str8_pushf(a, "cfslug:%.*s", S8F(slug)) : str8(0, 0);
    }
    // owner/repo
    U64 slash = str8_find(in, 0, str8_lit("/"), 0);
    if (slash < in.size && slash > 0 && slash + 1 < in.size && str8_find(in, slash + 1, str8_lit("/"), 0) == in.size &&
        str8_find(in, 0, str8_lit(" "), 0) == in.size && str8_find(in, 0, str8_lit(":"), 0) == in.size) {
        return str8_pushf(a, "gh:%.*s", S8F(in));
    }
    return str8(0, 0);
}

//- State file

static Mgr *mgr_open(Arena *a, String8 addons_dir, String8 api_key)
{
    Mgr *m = push_struct(a, Mgr);
    m->arena = a;
    m->addons_dir = str8_copy(a, addons_dir);
    m->api_key = str8_copy(a, api_key);
    m->addons = push_array(a, MgrAddon, MGR_MAX_ADDONS);
    m->state_path = path_join(a, path_dirname(addons_dir), str8_lit("wam.json"));
    m->client = wow_client_version(addons_dir);

    String8 text = os_read_file(a, m->state_path);
    JSON *root = json_parse(a, text);
    if (str8_trim(text).size && root->type != JSON_OBJ) m->state_unreadable = 1;
    json_each(it, json_get(root, "addons")) {
        String8 id = json_str(json_get(it, "id"));
        if (!id.size && json_int(json_get(it, "modId"))) id = str8_pushf(a, "cf:%lld", json_int(json_get(it, "modId"))); // v1 files
        if (!id.size) continue;
        MgrAddon *e = mgr_add(m);
        if (!e) break;
        e->id = id;
        e->file_id = json_int(json_get(it, "fileId"));
        e->name = json_str(json_get(it, "name"));
        e->version = json_str(json_get(it, "version"));
        e->website = json_str(json_get(it, "website"));
        e->latest_file_id = json_int(json_get(it, "latestFileId"));
        e->latest_version = json_str(json_get(it, "latestVersion"));
        e->pinned = json_bool(json_get(it, "pinned"), 0);
        json_each(f, json_get(it, "folders")) if (folder_name_is_safe(json_str(f))) str8_list_push(a, &e->folders, json_str(f));
        json_each(c, json_get(it, "checks")) {
            if (folder_name_is_safe(c->key) && json_str(c).size) str8_list_push(a, &e->checks, str8_pushf(a, "%.*s|%.*s", S8F(c->key), S8F(json_str(c))));
        }
        if (!e->folders.count) m->count -= 1;
    }
    return m;
}

static B32 mgr_save(Mgr *m)
{
    if (m->state_unreadable) {
        return mgr_fail(m, "%.*s is damaged, so wam won't overwrite it. Fix it, or delete it and run Check for updates.", S8F(m->state_path));
    }
    Temp scratch = scratch_begin(m->arena);
    Arena *s = scratch.arena;
    String8List out = { 0 };
    str8_list_push(s, &out, str8_lit("{\n  \"version\": 2,\n  \"addons\": ["));
    for (U64 i = 0; i < m->count; i += 1) {
        MgrAddon *e = &m->addons[i];
        String8List folders = { 0 };
        for (String8Node *n = e->folders.first; n; n = n->next) {
            str8_list_push(s, &folders, str8_pushf(s, "\"%.*s\"", S8F(json_escape(s, n->string))));
        }
        String8List checks = { 0 };
        for (String8Node *n = e->checks.first; n; n = n->next) {
            U64 bar = str8_find(n->string, 0, str8_lit("|"), 0);
            str8_list_push(s, &checks, str8_pushf(s, "\"%.*s\": \"%.*s\"", S8F(json_escape(s, str8_prefix(n->string, bar))),
                                                  S8F(json_escape(s, str8_skip(n->string, bar + 1)))));
        }
        str8_list_push(s, &out, str8_pushf(s,
            "%s\n    {\"id\": \"%.*s\", \"fileId\": %lld, \"name\": \"%.*s\", \"version\": \"%.*s\", \"website\": \"%.*s\", "
            "\"latestFileId\": %lld, \"latestVersion\": \"%.*s\",%s \"folders\": [%.*s], \"checks\": {%.*s}}",
            i ? "," : "", S8F(json_escape(s, e->id)), e->file_id, S8F(json_escape(s, e->name)), S8F(json_escape(s, e->version)),
            S8F(json_escape(s, e->website)), e->latest_file_id, S8F(json_escape(s, e->latest_version)), e->pinned ? " \"pinned\": true," : "",
            S8F(str8_list_join(s, &folders, str8_lit(", "))), S8F(str8_list_join(s, &checks, str8_lit(", ")))));
    }
    str8_list_push(s, &out, str8_lit("\n  ]\n}\n"));
    // Write a temp file and swap it in, so a crash never leaves a half-written state file.
    String8 tmp = str8_pushf(s, "%.*s.tmp", S8F(m->state_path));
    B32 ok = os_write_file(tmp, str8_list_join(s, &out, str8_lit(""))) && os_replace_file(tmp, m->state_path);
    scratch_end(scratch);
    if (!ok) return mgr_fail(m, "could not write %.*s", S8F(m->state_path));
    return 1;
}

// Drops folders that no longer exist on disk (deleted by hand); entries with none left go away.
static B32 mgr_prune(Mgr *m)
{
    B32 changed = 0;
    for (U64 i = 0; i < m->count;) {
        MgrAddon *e = &m->addons[i];
        String8List kept = { 0 };
        for (String8Node *n = e->folders.first; n; n = n->next) {
            Temp t = temp_begin(m->arena);
            B32 exists = os_dir_exists(path_join(m->arena, m->addons_dir, n->string));
            temp_end(t);
            if (exists) str8_list_push(m->arena, &kept, n->string);
            else changed = 1;
        }
        e->folders = kept;
        if (kept.count == 0) { mgr_delete_entry(m, e); continue; }
        i += 1;
    }
    return changed;
}

//- CurseForge: choosing a file for this game version
//
// Uses the mod's latestFilesIndexes (one entry per game version + release type).
// Same major version as the client is required (1.x = Era/Forever, 2.x = Anniversary,
// 5.x = Classic, 11+/12+ = Retail). Then release > beta > alpha (stable channel),
// then same minor version, then the newest file. Retail falls back to the newest
// older retail major (addons often lag a patch), never a Classic one.

typedef struct Pick { S64 file_id; S64 score; } Pick;

static void pick_consider(Pick *p, GameVersion cv, U32 want_major, String8 game_version, S64 release_type, S64 file_id)
{
    U32 v[3];
    parse_version3(game_version, v);
    if (cv.valid && v[0] != want_major) return;
    S64 rank = (release_type == 1 ? 30 : release_type == 2 ? 20 : 10) + (cv.valid && v[1] == cv.minor ? 1 : 0);
    S64 score = rank * 10000000000ll + file_id;
    if (score > p->score) { p->score = score; p->file_id = file_id; }
}

static S64 mgr_pick_file(Mgr *m, JSON *mod)
{
    GameVersion cv = m->client;
    JSON *indexes = json_get(mod, "latestFilesIndexes");
    JSON *files = json_get(mod, "latestFiles");

    U32 majors_to_try[2] = { cv.major, 0 };
    int tries = 1;
    if (cv.valid && cv.major >= 10) {
        U32 best = 0;
        json_each(it, indexes) {
            U32 v[3]; parse_version3(json_str(json_get(it, "gameVersion")), v);
            if (v[0] >= 10 && v[0] < cv.major && v[0] > best) best = v[0]; // retail only: never a Classic file
        }
        if (best) { majors_to_try[1] = best; tries = 2; }
    }

    for (int t = 0; t < tries; t += 1) {
        Pick p = { 0 };
        json_each(it, indexes) {
            pick_consider(&p, cv, majors_to_try[t], json_str(json_get(it, "gameVersion")),
                          json_int(json_get(it, "releaseType")), json_int(json_get(it, "fileId")));
        }
        json_each(f, files) {
            json_each(gv, json_get(f, "gameVersions")) {
                pick_consider(&p, cv, majors_to_try[t], json_str(gv), json_int(json_get(f, "releaseType")), json_int(json_get(f, "id")));
            }
        }
        if (p.file_id) return p.file_id;
    }
    return 0;
}

static String8 mod_file_label(JSON *mod, S64 file_id)
{
    json_each(f, json_get(mod, "latestFiles")) {
        if (json_int(json_get(f, "id")) == file_id) return json_str(json_get(f, "displayName"));
    }
    json_each(it, json_get(mod, "latestFilesIndexes")) {
        if (json_int(json_get(it, "fileId")) == file_id) return json_str(json_get(it, "filename"));
    }
    return str8(0, 0);
}

static JSON *cf_fetch_mod(Mgr *m, S64 mod_id)
{
    HTTP_Result r = cf_get_mod(m->arena, m->api_key, mod_id);
    if (!r.ok) { mgr_fail(m, "could not fetch CurseForge addon %lld: %.*s", mod_id, S8F(r.error)); return 0; }
    JSON *mod = json_get(json_parse(m->arena, r.body), "data");
    if (mod->type != JSON_OBJ) { mgr_fail(m, "unexpected CurseForge response for addon %lld", mod_id); return 0; }
    return mod;
}

// Resolves a CurseForge mod (fetched if `mod` is null) to the file to install.
static RemoteFile cf_resolve(Mgr *m, S64 mod_id, JSON *mod)
{
    RemoteFile rf = { 0 };
    Arena *a = m->arena;
    if (!mgr_require_key(m)) { rf.error = m->error; return rf; }
    if (!mod) mod = cf_fetch_mod(m, mod_id);
    if (!mod) { rf.error = m->error; return rf; }
    mod_id = json_int(json_get(mod, "id"));
    rf.id = str8_pushf(a, "cf:%lld", mod_id);
    rf.name = json_str(json_get(mod, "name"));
    rf.website = json_str(json_get(json_get(mod, "links"), "websiteUrl"));
    S64 file_id = mgr_pick_file(m, mod);
    if (!file_id) {
        rf.error = str8_pushf(a, "%.*s has no file for this game version (%u.%u.%u)", S8F(rf.name), m->client.major, m->client.minor, m->client.patch);
        return rf;
    }
    HTTP_Result r = cf_get_file(a, m->api_key, mod_id, file_id);
    if (!r.ok) { rf.error = str8_pushf(a, "could not fetch file info for %.*s: %.*s", S8F(rf.name), S8F(r.error)); return rf; }
    JSON *file = json_get(json_parse(a, r.body), "data");
    rf.file_id = file_id;
    rf.version = json_str(json_get(file, "displayName"));
    rf.url = json_str(json_get(file, "downloadUrl"));
    json_each(dep, json_get(file, "dependencies")) {
        if (json_int(json_get(dep, "relationType")) == 3 && rf.cf_dep_count < ArrayCount(rf.cf_deps)) {
            rf.cf_deps[rf.cf_dep_count++] = json_int(json_get(dep, "modId"));
        }
    }
    if (!rf.url.size) {
        // The author turned off third-party downloads on CurseForge. Many authors also publish
        // releases on GitHub (linked as the project's source); use that if it's current.
        String8 why = str8_lit("");
        String8 gh = mgr_parse_spec(a, json_str(json_get(json_get(mod, "links"), "sourceUrl")));
        if (id_is(gh, "gh:")) {
            mgr_status(m, "%.*s isn't downloadable from CurseForge by other apps; checking its GitHub releases...", S8F(rf.name));
            RemoteFile g = gh_resolve_api(a, m->client, id_rest(gh)); // the API gives the release date to compare
            S64 cf_days = iso_days(json_str(json_get(file, "fileDate")));
            if (g.ok && (!cf_days || !g.date_days || g.date_days >= cf_days - 14)) {
                g.fallback_from = rf.name;
                return g;
            }
            why = g.ok ? str8_pushf(a, " Its GitHub releases (%.*s) are older than the CurseForge version.", S8F(g.version))
                       : str8_lit(" Its GitHub page has no usable release.");
        }
        rf.error = str8_pushf(a, "%.*s's author only allows downloads through CurseForge itself.%.*s "
                                 "Its download page starts the download by itself.",
                              S8F(rf.name), S8F(why));
        // The page for exactly this file (it downloads on its own after a few seconds), and how to
        // recognize the archive when it lands in the download folder.
        rf.file_id = file_id;
        rf.version = json_str(json_get(file, "displayName"));
        rf.manual_url = str8_pushf(a, "%.*s/download/%lld", S8F(rf.website), file_id);
        rf.manual_file = json_str(json_get(file, "fileName"));
        rf.manual_size = json_int(json_get(file, "fileLength"));
        rf.error_url = rf.manual_url;
        return rf;
    }
    rf.date_days = iso_days(json_str(json_get(file, "fileDate")));
    rf.ok = 1;
    return rf;
}

static RemoteFile mgr_resolve(Mgr *m, String8 id)
{
    if (id_is(id, "cf:")) return cf_resolve(m, id_number(id), 0);
    if (id_is(id, "cfslug:")) {
        RemoteFile rf = { 0 };
        if (!mgr_require_key(m)) { rf.error = m->error; return rf; }
        HTTP_Result r = cf_search(m->arena, m->api_key, str8(0, 0), id_rest(id));
        JSON *mod = json_get(json_parse(m->arena, r.body), "data")->first;
        if (!r.ok || !mod) { rf.error = str8_pushf(m->arena, "CurseForge addon '%.*s' not found", S8F(id_rest(id))); return rf; }
        return cf_resolve(m, 0, mod);
    }
    if (id_is(id, "gh:")) return gh_resolve(m->arena, m->client, id_rest(id));
    if (id_is(id, "wowi:")) return wowi_resolve(m->arena, id_number(id));
    if (id_is(id, "tukui:")) return tukui_resolve(m->arena, m->client, id_rest(id));
    if (id_is(id, "wago:") || id_is(id, "wagoslug:")) {
        RemoteFile rf = { 0 };
        if (!mgr_require_wago(m)) { rf.error = m->error; return rf; }
        String8 wid = id_rest(id);
        if (id_is(id, "wagoslug:")) {
            wid = wago_id_from_slug(m->arena, m->wago_key, m->client, wid, &rf.error);
            if (!wid.size) return rf;
        }
        return wago_resolve(m->arena, m->wago_key, m->client, wid);
    }
    RemoteFile rf = { 0 };
    rf.error = str8_pushf(m->arena, "unknown addon id '%.*s'", S8F(id));
    return rf;
}

//- Installing a zip into AddOns

typedef struct ExtractCtx { ZipArchive *z; String8 staging; volatile LONG failed, fail_index, fail_read; Mgr *m; volatile LONG done; KnownHash *hashes; } ExtractCtx;

static void extract_one(void *user, U64 i)
{
    ExtractCtx *c = (ExtractCtx *)user;
    ZipEntry *e = &c->z->v[i];
    if (c->failed || e->is_dir) return;
    Temp scratch = scratch_begin(0);
    String8 name = path_normalize_slashes(scratch.arena, e->name);
    if (str8_find(name, 0, str8_lit("\\"), 0) < name.size) {
        B32 read_ok;
        String8 data = zip_read(scratch.arena, c->z, e, &read_ok);
        if (!read_ok || !os_write_file(path_join(scratch.arena, c->staging, name), data)) {
            InterlockedExchange(&c->fail_read, !read_ok);
            InterlockedExchange(&c->fail_index, (LONG)i);
            InterlockedExchange(&c->failed, 1);
        } else {
            md5_hex(data, c->hashes[i].md5); // so recording the install needn't read it back from disk
        }
    }
    scratch_end(scratch);
    LONG done = InterlockedIncrement(&c->done);
    mgr_progress(c->m, 0.50f + 0.30f * (F32)done / (F32)Max(c->z->count, 1)); // unpacking: 50-80%
}

typedef enum ArchiveKind { ARCHIVE_UNKNOWN, ARCHIVE_ZIP, ARCHIVE_RAR, ARCHIVE_7Z } ArchiveKind;

static ArchiveKind archive_kind(String8 data)
{
    if (data.size >= 4 && data.str[0] == 'P' && data.str[1] == 'K' && (data.str[2] == 3 || data.str[2] == 5)) return ARCHIVE_ZIP;
    if (data.size >= 7 && memcmp(data.str, "Rar!\x1a\x07", 6) == 0) return ARCHIVE_RAR;
    if (data.size >= 6 && memcmp(data.str, "7z\xbc\xaf\x27\x1c", 6) == 0) return ARCHIVE_7Z;
    return ARCHIVE_UNKNOWN;
}

// Top-level folder of an archive entry name, if it's inside one (files at the root are readmes etc.).
static void collect_top(Arena *s, String8List *tops, String8 entry_name)
{
    String8 name = path_normalize_slashes(s, entry_name);
    U64 slash = str8_find(name, 0, str8_lit("\\"), 0);
    if (slash == name.size) return;
    String8 top = str8_prefix(name, slash);
    if (folder_name_is_safe(top) && !list_has_ci(tops, top)) str8_list_push(s, tops, top);
}

// Extracts a zip into `staging` with our own reader (parallel file writes).
static B32 zip_extract_to(Mgr *m, Arena *s, String8 data, String8 staging, String8List *tops)
{
    ZipArchive z;
    if (!zip_open(s, data, &z)) return mgr_fail(m, "the download is not a valid zip file");
    for (U64 i = 0; i < z.count; i += 1) {
        if (!zip_name_is_safe(z.v[i].name)) return mgr_fail(m, "the archive contains an unsafe path: %.*s", S8F(z.v[i].name));
        collect_top(s, tops, z.v[i].name);
    }
    if (!tops->count) return mgr_fail(m, "the archive has no addon folders");

    // Create the folder tree first, then extract files in parallel
    // (per-file write latency, mostly antivirus scanning, dominates otherwise).
    String8List dirs = { 0 };
    for (U64 i = 0; i < z.count; i += 1) {
        String8 name = path_normalize_slashes(s, z.v[i].name);
        if (str8_find(name, 0, str8_lit("\\"), 0) == name.size) continue;
        String8 dir = z.v[i].is_dir ? name : path_dirname(name);
        while (dir.size && dir.str[dir.size - 1] == '\\') dir.size -= 1;
        if (!list_has_ci(&dirs, dir)) str8_list_push(s, &dirs, dir);
    }
    for (String8Node *n = dirs.first; n; n = n->next) {
        if (!os_make_dirs(path_join(s, staging, n->string))) return mgr_fail(m, "could not create %.*s", S8F(n->string));
    }
    inflate_init();
    KnownHash *hashes = push_array(m->arena, KnownHash, z.count + 1);
    for (U64 i = 0; i < z.count; i += 1) {
        String8 rel = str8_lower(m->arena, path_normalize_slashes(m->arena, z.v[i].name));
        for (U64 k = 0; k < rel.size; k += 1) if (rel.str[k] == '\\') rel.str[k] = '/';
        hashes[i].rel = rel;
    }
    ExtractCtx ctx = { &z, staging, 0, 0, 0, m, 0, hashes };
    os_parallel_for(z.count, extract_one, &ctx);
    if (ctx.failed) {
        String8 bad = z.v[ctx.fail_index].name;
        return ctx.fail_read ? mgr_fail(m, "corrupt file in the archive: %.*s", S8F(bad)) : mgr_fail(m, "could not write %.*s", S8F(bad));
    }
    // The files written (and hashed), sorted for folder_check's lookups.
    U64 n = 0;
    for (U64 i = 0; i < z.count; i += 1) if (hashes[i].md5[0]) hashes[n++] = hashes[i];
    qsort(hashes, n, sizeof(KnownHash), known_hash_compare);
    m->unpacked = (KnownHashes){ hashes, n };
    return 1;
}

static String8 first_line(String8 s)
{
    String8 rest = str8_trim(s);
    return str8_trim(str8_pop_line(&rest));
}

// Extracts RAR / 7z into `staging` with Windows' own tar.exe (bsdtar / libarchive, in System32
// since Windows 10 1803). Names are checked before extracting, the tree is checked for links after.
static B32 tar_extract_to(Mgr *m, Arena *s, String8 data, String8 archive_path, String8 staging, String8List *tops, char *kind_name)
{
    String8 tar = path_join(s, os_system_dir(s), str8_lit("tar.exe"));
    if (!os_file_exists(tar)) return mgr_fail(m, "%s archives need tar.exe from Windows 10 (1803) or newer", kind_name);
    if (!os_write_file(archive_path, data)) return mgr_fail(m, "could not write a temporary copy of the archive");

    // 1. List entry names and validate them, like zips.
    OS_RunResult list = os_run(s, tar, str8_pushf(s, "\"%.*s\" -tf \"%.*s\"", S8F(tar), S8F(archive_path)), 30000);
    if (!list.started) return mgr_fail(m, "could not run tar.exe");
    if (list.timed_out) return mgr_fail(m, "reading the %s archive took too long", kind_name);
    if (list.exit_code != 0) return mgr_fail(m, "could not read the %s archive: %.*s", kind_name, S8F(first_line(list.output)));
    String8 rest = list.output;
    while (rest.size) {
        String8 name = str8_trim(str8_pop_line(&rest));
        if (!name.size) continue;
        if (!zip_name_is_safe(name)) return mgr_fail(m, "the archive contains an unsafe path: %.*s", S8F(name));
        collect_top(s, tops, name);
    }
    if (!tops->count) return mgr_fail(m, "the archive has no addon folders");

    // Symbolic links show up as "l..." in the verbose listing (hard links stay inside the
    // archive's own files and are harmless). Refuse them up front with a clear message.
    OS_RunResult vlist = os_run(s, tar, str8_pushf(s, "\"%.*s\" -tvf \"%.*s\"", S8F(tar), S8F(archive_path)), 30000);
    for (String8 r = vlist.output; r.size;) {
        String8 line = str8_trim(str8_pop_line(&r));
        if (line.size && line.str[0] == 'l') return mgr_fail(m, "the archive contains symbolic links, which wam doesn't install");
    }

    // 2. Extract. Any error (corrupt data, password, a link it refused) fails the whole install.
    OS_RunResult x = os_run(s, tar, str8_pushf(s, "\"%.*s\" -xf \"%.*s\" -C \"%.*s\"", S8F(tar), S8F(archive_path), S8F(staging)), 120000);
    if (!x.started) return mgr_fail(m, "could not run tar.exe");
    if (x.timed_out) return mgr_fail(m, "extracting the %s archive took too long", kind_name);
    if (x.exit_code != 0) {
        if (str8_find(x.output, 0, str8_lit("encrypted"), 1) < x.output.size) return mgr_fail(m, "password-protected archives aren't supported");
        return mgr_fail(m, "could not extract the %s archive: %.*s", kind_name, S8F(first_line(x.output)));
    }
    // 3. An addon never needs links; one could point outside AddOns.
    if (os_tree_has_links(staging)) return mgr_fail(m, "the archive contains links (symlinks or junctions), which wam doesn't install");
    for (String8Node *n = tops->first; n; n = n->next) {
        if (!os_dir_exists(path_join(s, staging, n->string))) return mgr_fail(m, "the archive didn't extract as expected (%.*s missing)", S8F(n->string));
    }
    return 1;
}

static B32 (*mgr_move)(String8 from, String8 to) = os_move; // (tests make some moves fail)

// Moves the folders set aside in `old` back into AddOns. One that can't be moved stays in `old`,
// which then holds the only copy: *keep_old is set, and the error says where it is.
static void mgr_put_back(Mgr *m, Arena *s, String8 old, String8List *moved, B32 *keep_old, String8 why)
{
    String8List stuck = { 0 };
    for (String8Node *n = moved->first; n; n = n->next) {
        if (!mgr_move(path_join(s, old, n->string), path_join(s, m->addons_dir, n->string))) str8_list_push(s, &stuck, n->string);
    }
    if (!stuck.count) { mgr_fail(m, "%.*s", S8F(why)); return; }
    *keep_old = 1;
    mgr_fail(m, "%.*s. Could not put back %.*s either: the previous version is kept in %.*s",
             S8F(why), S8F(str8_list_join(s, &stuck, str8_lit(", "))), S8F(old));
}

// Replaces the folders `tops` in AddOns with the ones in `staging`; the previous ones end up in `old`.
// On failure everything is put back; *keep_old: some of it couldn't be, so `old` must not be deleted.
static B32 mgr_swap_in(Mgr *m, Arena *s, String8 staging, String8 old, String8List *tops, B32 *keep_old)
{
    // Move existing folders aside. If any is locked, put the others back.
    String8List moved = { 0 };
    for (String8Node *n = tops->first; n; n = n->next) {
        String8 dst = path_join(s, m->addons_dir, n->string);
        if (!os_dir_exists(dst) && !os_is_link(dst)) continue;
        if (!mgr_move(dst, path_join(s, old, n->string))) {
            mgr_put_back(m, s, old, &moved, keep_old, str8_pushf(s, "could not replace %.*s - is WoW or another program using it?", S8F(n->string)));
            return 0;
        }
        str8_list_push(s, &moved, n->string);
    }

    // Move the new folders in. If one can't be, take the others out again (back to staging: a move
    // leaves nothing half-deleted in the way) and put the previous ones back.
    for (String8Node *n = tops->first; n; n = n->next) {
        if (mgr_move(path_join(s, staging, n->string), path_join(s, m->addons_dir, n->string))) continue;
        for (String8Node *r = tops->first; r != n; r = r->next) {
            String8 dst = path_join(s, m->addons_dir, r->string);
            if (!mgr_move(dst, path_join(s, staging, r->string))) os_delete_tree(dst);
        }
        mgr_put_back(m, s, old, &moved, keep_old, str8_pushf(s, "could not install %.*s", S8F(n->string)));
        return 0;
    }
    return 1;
}

// Installs a zip / rar / 7z archive into AddOns: extract to a staging folder, move the old
// folders aside, move the new ones in, and roll back if any step fails.
static B32 mgr_install_archive(Mgr *m, String8 data, String8List *out_folders)
{
    m->unpacked = (KnownHashes){ 0 }; // (a zip fills it in: its files' MD5s)
    Temp scratch = scratch_begin(m->arena);
    Arena *s = scratch.arena;
    B32 ok = 0, keep_old = 0;
    ArchiveKind kind = archive_kind(data);
    if (kind == ARCHIVE_UNKNOWN) { mgr_fail(m, "the file isn't a zip, rar or 7z archive"); scratch_end(scratch); return 0; }

    U64 tag = os_now_us();
    String8 staging = path_join(s, m->addons_dir, str8_pushf(s, ".wam-staging-%llu", tag));
    String8 old = path_join(s, m->addons_dir, str8_pushf(s, ".wam-old-%llu", tag));
    String8 archive_path = path_join(s, m->addons_dir, str8_pushf(s, ".wam-archive-%llu", tag));
    String8List tops = { 0 };
    if (!os_make_dirs(staging) || !os_make_dirs(old)) { mgr_fail(m, "could not create a staging folder in AddOns"); goto cleanup; }

    B32 extracted = kind == ARCHIVE_ZIP ? zip_extract_to(m, s, data, staging, &tops)
                  : tar_extract_to(m, s, data, archive_path, staging, &tops, kind == ARCHIVE_RAR ? "RAR" : "7z");
    if (!extracted) goto cleanup;

    if (!mgr_swap_in(m, s, staging, old, &tops, &keep_old)) goto cleanup;

    ok = 1;
    memset(out_folders, 0, sizeof(*out_folders));
    for (String8Node *n = tops.first; n; n = n->next) str8_list_push(m->arena, out_folders, str8_copy(m->arena, n->string));

cleanup:
    if (!keep_old) os_delete_tree(old);
    os_delete_tree(staging);
    os_delete_tree(archive_path);
    scratch_end(scratch);
    return ok;
}

// Records `folders` as belonging to `e`, taking them away from any other entry.
static void mgr_claim_folders(Mgr *m, MgrAddon *e, String8List *folders)
{
    for (U64 i = 0; i < m->count;) {
        MgrAddon *other = &m->addons[i];
        if (other != e) {
            other->folders = list_without_ci(m->arena, &other->folders, folders);
            if (other->folders.count == 0) {
                B32 e_after = e > other;
                mgr_delete_entry(m, other);
                if (e_after) e -= 1;
                continue;
            }
        }
        i += 1;
    }
}

//- Local edits
//
// When wam installs (or first recognizes) an addon it records two values per folder:
//   sig   MD5 over "relative path|size|mtime" of every file: cheap, needs no file reads
//   hash  MD5 over "relative path|MD5 of the contents" of every file
// Later, a folder whose sig still matches is unchanged. If the sig differs, the contents
// are hashed again; a different hash means someone edited, added or deleted files, and
// batch updates leave that addon alone instead of overwriting the changes.

typedef struct CheckEntry { String8 rel; U64 size, mtime; String8 path; } CheckEntry;

static void check_walk(Arena *a, String8 root, String8 rel, CheckEntry **v, U64 *count, U64 *cap)
{
    String8 dir = rel.size ? path_join(a, root, rel) : root;
    OS_FileInfoArray files = os_list_dir(a, dir);
    for (U64 i = 0; i < files.count; i += 1) {
        OS_FileInfo *f = &files.v[i];
        if (f->is_link) continue; // never followed (nothing wam installs has links)
        String8 r = rel.size ? str8_pushf(a, "%.*s/%.*s", S8F(rel), S8F(f->name)) : f->name;
        if (f->is_dir) { check_walk(a, root, r, v, count, cap); continue; }
        if (*count == *cap) {
            U64 ncap = *cap ? *cap * 2 : 256;
            CheckEntry *nv = push_array_nz(a, CheckEntry, ncap);
            if (*count) memcpy(nv, *v, *count * sizeof(CheckEntry));
            *v = nv; *cap = ncap;
        }
        CheckEntry *e = &(*v)[(*count)++];
        e->rel = str8_lower(a, r);
        e->size = f->size;
        e->mtime = f->mtime;
        e->path = path_join(a, dir, f->name);
    }
}

static int check_entry_compare(const void *pa, const void *pb)
{
    const CheckEntry *x = (const CheckEntry *)pa, *y = (const CheckEntry *)pb;
    U64 n = Min(x->rel.size, y->rel.size);
    int d = memcmp(x->rel.str, y->rel.str, n);
    return d ? d : (x->rel.size > y->rel.size) - (x->rel.size < y->rel.size);
}

static int known_hash_compare(const void *pa, const void *pb)
{
    const KnownHash *x = (const KnownHash *)pa, *y = (const KnownHash *)pb;
    U64 n = Min(x->rel.size, y->rel.size);
    int d = memcmp(x->rel.str, y->rel.str, n);
    return d ? d : (x->rel.size > y->rel.size) - (x->rel.size < y->rel.size);
}

static char *known_hash_find(KnownHashes *k, String8 rel)
{
    if (!k || !k->count) return 0;
    KnownHash key = { rel, { 0 } };
    KnownHash *hit = (KnownHash *)bsearch(&key, k->v, k->count, sizeof(KnownHash), known_hash_compare);
    return hit ? hit->md5 : 0;
}

// sig (and hash when want_hash) of one folder, as 32-char hex into out_sig / out_hash. Files in
// `known` (by "<folder>/<rel>") aren't read: their MD5 is taken from there.
static void folder_check_known(String8 dir, char out_sig[33], char out_hash[33], B32 want_hash, KnownHashes *known)
{
    Temp scratch = scratch_begin(0);
    Arena *s = scratch.arena;
    CheckEntry *v = 0;
    U64 count = 0, cap = 0;
    check_walk(s, dir, str8(0, 0), &v, &count, &cap);
    String8 folder = str8_lower(s, path_filename(dir));
    if (count) qsort(v, count, sizeof(CheckEntry), check_entry_compare);
    String8List sig = { 0 }, hash = { 0 };
    for (U64 i = 0; i < count; i += 1) {
        str8_list_push(s, &sig, str8_pushf(s, "%.*s|%llu|%llu\n", S8F(v[i].rel), v[i].size, v[i].mtime));
        if (want_hash) {
            char h[33];
            char *k = known_hash_find(known, str8_pushf(s, "%.*s/%.*s", S8F(folder), S8F(v[i].rel)));
            if (k) memcpy(h, k, 33);
            else {
                Temp t = temp_begin(s);
                md5_hex(os_read_file(s, v[i].path), h);
                temp_end(t);
            }
            str8_list_push(s, &hash, str8_pushf(s, "%.*s|%s\n", S8F(v[i].rel), h));
        }
    }
    md5_hex(str8_list_join(s, &sig, str8_lit("")), out_sig);
    if (want_hash) md5_hex(str8_list_join(s, &hash, str8_lit("")), out_hash);
    scratch_end(scratch);
}

static void folder_check(String8 dir, char out_sig[33], char out_hash[33], B32 want_hash) { folder_check_known(dir, out_sig, out_hash, want_hash, 0); }

static String8 mgr_check_of(MgrAddon *e, String8 folder)
{
    for (String8Node *n = e->checks.first; n; n = n->next) {
        U64 bar = str8_find(n->string, 0, str8_lit("|"), 0);
        if (str8_match(str8_prefix(n->string, bar), folder, 1)) return str8_skip(n->string, bar + 1);
    }
    return str8(0, 0);
}

// Records the current state of e's folders as "as installed".
static void mgr_record_checks(Mgr *m, MgrAddon *e)
{
    String8List checks = { 0 };
    U64 i = 0;
    for (String8Node *n = e->folders.first; n; n = n->next, i += 1) {
        mgr_progress(m, 0.80f + 0.17f * (F32)i / (F32)Max(e->folders.count, 1)); // recording: 80-97%
        char sig[33], hash[33];
        folder_check_known(path_join(m->arena, m->addons_dir, n->string), sig, hash, 1, &m->unpacked);
        str8_list_push(m->arena, &checks, str8_pushf(m->arena, "%.*s|%s:%s", S8F(n->string), sig, hash));
    }
    e->checks = checks;
}

// True if any of e's folders differs from what was recorded (unknown = not edited).
// A folder whose files were only touched (same contents) gets its sig refreshed in memory.
static B32 mgr_is_edited(Mgr *m, MgrAddon *e)
{
    B32 edited = 0;
    for (String8Node *n = e->folders.first; n; n = n->next) {
        String8 rec = mgr_check_of(e, n->string);
        U64 colon = str8_find(rec, 0, str8_lit(":"), 0);
        if (rec.size != 65 || colon != 32) continue;
        String8 dir = path_join(m->arena, m->addons_dir, n->string);
        if (!os_dir_exists(dir)) continue;
        char sig[33], hash[33];
        folder_check(dir, sig, hash, 0);
        if (memcmp(sig, rec.str, 32) == 0) continue;
        folder_check(dir, sig, hash, 1);
        if (memcmp(hash, rec.str + 33, 32) != 0) { edited = 1; continue; }
        memcpy(rec.str, sig, 32); // same contents, new timestamps: skip the hashing next time
    }
    return edited;
}

//- Install / update / remove

typedef struct DownloadProgress { Mgr *m; String8 name; int last_pct; } DownloadProgress;

static void download_progress(void *user, U64 got, U64 total)
{
    DownloadProgress *p = (DownloadProgress *)user;
    mgr_progress(p->m, total ? 0.05f + 0.45f * (F32)((F64)got / (F64)total) : -1.0f); // downloading: 5-50%
    int pct = total ? (int)(got * 100 / total) : -1;
    if (pct == p->last_pct || (pct >= 0 && pct < p->last_pct + 5 && pct != 100)) return;
    p->last_pct = pct;
    if (pct >= 0) mgr_status(p->m, "Downloading %.*s  %d%%", S8F(p->name), pct);
    else mgr_status(p->m, "Downloading %.*s  %llu KB", S8F(p->name), got / 1024);
}

// Installs `data` (an archive) as the file `rf` describes and records it, then its CurseForge dependencies.
static B32 mgr_install_remote_bytes(Mgr *m, RemoteFile *rf, String8 data, int depth)
{
    mgr_status(m, "Installing %.*s...", S8F(rf->name));
    mgr_progress(m, 0.50f);
    String8List folders = { 0 };
    if (!mgr_install_archive(m, data, &folders)) return 0; // some WoWInterface uploads are .rar

    MgrAddon *e = mgr_find(m, rf->id);
    if (e) {
        // Folders the old version had but the new one doesn't ship anymore.
        for (String8Node *n = e->folders.first; n; n = n->next) {
            if (!list_has_ci(&folders, n->string)) os_delete_tree(path_join(m->arena, m->addons_dir, n->string));
        }
    } else {
        e = mgr_add(m);
        if (!e) return mgr_fail(m, "too many tracked addons");
    }
    e->id = rf->id;
    e->file_id = rf->file_id;
    e->name = rf->name;
    e->version = rf->version;
    e->website = rf->website;
    e->folders = folders;
    mgr_record_checks(m, e); // "as installed", to notice local edits before the next update
    if (rf->fallback_from.size) {
        e->name = rf->fallback_from; // the CurseForge name is what people know it by
    } else if (id_is(rf->id, "gh:")) {
        // Repo names are often terse ("BigWigs" is fine, "addon" isn't): prefer the addon's own title.
        String8 title = addon_folder_title(m->arena, m->addons_dir, folders.first->string);
        if (title.size) e->name = title;
    }
    if (e->latest_file_id <= rf->file_id) { e->latest_file_id = rf->file_id; e->latest_version = rf->version; }
    String8 name = e->name;
    if (depth == 0) m->last_installed_id = e->id;
    mgr_claim_folders(m, e, &folders);
    if (!mgr_save(m)) return 0;

    MgrProgressFunc *progress = m->progress;
    m->progress = 0; // the bar is the addon's: its dependencies' downloads don't move it back
    for (U32 i = 0; i < rf->cf_dep_count && depth < 3; i += 1) {
        String8 dep = str8_pushf(m->arena, "cf:%lld", rf->cf_deps[i]);
        if (mgr_find(m, dep)) continue;
        String8 saved_error = m->error;
        if (!mgr_install_spec(m, dep, depth + 1)) {
            mgr_status(m, "Could not install a dependency of %.*s: %.*s", S8F(name), S8F(m->error));
        }
        m->error = saved_error;
    }
    m->progress = progress;
    return 1;
}

static B32 mgr_install_remote(Mgr *m, RemoteFile *rf, int depth)
{
    if (!rf->ok) {
        if (rf->manual_url.size && m->manual_count < ArrayCount(m->manual)) m->manual[m->manual_count++] = *rf;
        m->error_url = rf->error_url;
        return mgr_fail(m, "%.*s", S8F(rf->error));
    }
    Temp scratch = scratch_begin(m->arena);
    DownloadProgress prog = { m, rf->name, -100 };
    if (rf->fallback_from.size) {
        mgr_status(m, "%.*s: CurseForge doesn't allow other apps to download it, installing %.*s from GitHub instead...",
                   S8F(rf->fallback_from), S8F(rf->version));
    }
    mgr_status(m, "Downloading %.*s...", S8F(rf->name));
    mgr_progress(m, 0.05f);
    HTTP_Result r = cf_download(scratch.arena, m->api_key, rf->url, download_progress, &prog);
    if (!r.ok) { scratch_end(scratch); return mgr_fail(m, "download of %.*s failed: %.*s", S8F(rf->name), S8F(r.error)); }
    B32 ok = mgr_install_remote_bytes(m, rf, r.body, depth);
    scratch_end(scratch);
    return ok;
}

// A file the user downloaded through CurseForge's own page (rf from a failed install: id, file_id,
// version, name), installed as that CurseForge file so update checks work as usual.
static B32 mgr_install_downloaded(Mgr *m, RemoteFile *rf, String8 path)
{
    String8 data = os_read_file(m->arena, path);
    if (!data.size) return mgr_fail(m, "could not read %.*s", S8F(path));
    m->last_installed_id = str8(0, 0);
    return mgr_install_remote_bytes(m, rf, data, 0);
}

static B32 mgr_install_spec(Mgr *m, String8 spec, int depth)
{
    String8 id = mgr_parse_spec(m->arena, spec);
    if (!id.size) return mgr_fail(m, "'%.*s' isn't a CurseForge id, GitHub repo, or WoWInterface, Wago or Tukui link", S8F(spec));
    mgr_status(m, "Looking up %.*s...", S8F(id));
    RemoteFile rf = mgr_resolve(m, id);
    return mgr_install_remote(m, &rf, depth);
}

// Installs from anything the user can paste: CurseForge id/URL, GitHub URL or owner/repo, WoWInterface URL.
// On success, *out_id (if given) is the installed addon's id.
static B32 mgr_install(Mgr *m, String8 spec, String8 *out_id)
{
    m->last_installed_id = str8(0, 0);
    mgr_progress(m, 0.02f); // finding the file
    B32 ok = mgr_install_spec(m, spec, 0);
    if (ok && out_id) *out_id = m->last_installed_id;
    return ok;
}

static B32 is_archive_name(String8 path)
{
    return str8_ends_with(path, str8_lit(".zip"), 1) || str8_ends_with(path, str8_lit(".rar"), 1) || str8_ends_with(path, str8_lit(".7z"), 1);
}

// Installs a zip / rar / 7z the user downloaded themselves. Whatever wam tracked for those folders
// is now stale, so they become untracked; the next sync recognizes them again by fingerprint.
static B32 mgr_install_local_archive(Mgr *m, String8 path, String8List *out_folders)
{
    String8 data = os_read_file(m->arena, path);
    if (!data.size) return mgr_fail(m, "could not read %.*s", S8F(path));
    if (archive_kind(data) == ARCHIVE_UNKNOWN) return mgr_fail(m, "%.*s isn't a zip, rar or 7z archive", S8F(path_filename(path)));
    mgr_status(m, "Installing %.*s...", S8F(path_filename(path)));
    if (!mgr_install_archive(m, data, out_folders)) return 0;
    mgr_claim_folders(m, 0, out_folders);
    return mgr_save(m);
}

// Refreshes latest_file_id / latest_version for tracked addons (all when `only` is null or empty).
// Returns 0 if any source failed; m->error holds the first failure.
static B32 mgr_refresh_latest(Mgr *m, String8List *only)
{
    B32 ok = 1;
    String8 first_error = { 0 };
    S64 *cf_ids = push_array(m->arena, S64, m->count + 1), *wowi_ids = push_array(m->arena, S64, m->count + 1);
    U64 ncf = 0, nwowi = 0;
    for (U64 i = 0; i < m->count; i += 1) {
        MgrAddon *e = &m->addons[i];
        if (!id_selected(only, e->id)) continue;
        if (id_is(e->id, "cf:")) cf_ids[ncf++] = id_number(e->id);
        else if (id_is(e->id, "wowi:")) wowi_ids[nwowi++] = id_number(e->id);
    }

    if (ncf && m->api_key.size) {
        HTTP_Result r = cf_get_mods(m->arena, m->api_key, cf_ids, ncf);
        if (!r.ok) { ok = 0; first_error = str8_pushf(m->arena, "CurseForge update check failed: %.*s", S8F(r.error)); }
        json_each(mod, json_get(json_parse(m->arena, r.body), "data")) {
            MgrAddon *e = mgr_find(m, str8_pushf(m->arena, "cf:%lld", json_int(json_get(mod, "id"))));
            if (!e) continue;
            e->name = json_str(json_get(mod, "name"));
            e->website = json_str(json_get(json_get(mod, "links"), "websiteUrl"));
            e->latest_file_id = mgr_pick_file(m, mod);
            e->latest_version = mod_file_label(mod, e->latest_file_id);
        }
    }
    if (nwowi) {
        HTTP_Result r = wowi_details(m->arena, wowi_ids, nwowi);
        if (!r.ok && ok) { ok = 0; first_error = str8_pushf(m->arena, "WoWInterface update check failed: %.*s", S8F(r.error)); }
        json_each(item, json_parse(m->arena, r.body)) {
            RemoteFile rf = wowi_file_from_json(m->arena, item);
            MgrAddon *e = mgr_find(m, rf.id);
            if (!e) continue;
            e->latest_file_id = rf.file_id;
            e->latest_version = rf.version;
            if (rf.name.size) e->name = rf.name;
        }
    }
    // Tukui: one list for all of them.
    B32 any_tukui = 0;
    for (U64 i = 0; i < m->count; i += 1) any_tukui |= id_is(m->addons[i].id, "tukui:") && id_selected(only, m->addons[i].id);
    if (any_tukui) {
        String8 err = { 0 };
        JSON *list = tukui_list(m->arena, &err);
        if (err.size && ok) { ok = 0; first_error = err; }
        json_each(item, list) {
            RemoteFile rf = tukui_file_from_json(m->arena, m->client, item);
            MgrAddon *e = mgr_find(m, rf.id);
            if (!e || !id_selected(only, e->id)) continue;
            if (rf.name.size) e->name = rf.name;
            if (rf.ok) { e->latest_version = rf.version; e->latest_file_id = 0; }
        }
    }

    // Wago: one batch request.
    String8 *wago_ids = push_array(m->arena, String8, m->count + 1);
    U64 nwago = 0;
    for (U64 i = 0; i < m->count; i += 1) {
        if (id_is(m->addons[i].id, "wago:") && id_selected(only, m->addons[i].id)) wago_ids[nwago++] = id_rest(m->addons[i].id);
    }
    if (nwago && !m->wago_key.size) {
        if (ok) first_error = str8_lit("Wago addons can't be checked without your Wago API key (add it in Settings)");
        ok = 0;
    } else if (nwago) {
        HTTP_Result r = wago_recents(m->arena, m->wago_key, m->client, wago_ids, nwago);
        if (!r.ok && ok) { ok = 0; first_error = str8_pushf(m->arena, "Wago update check failed: %.*s", S8F(r.error)); }
        json_each(item, json_get(json_parse(m->arena, r.body), "addons")) {
            if (item->type != JSON_OBJ) continue;
            RemoteFile rf = wago_file_from_scan(m->arena, m->wago_key, item);
            MgrAddon *e = mgr_find(m, rf.id);
            if (!e) continue;
            if (rf.name.size) e->name = rf.name;
            if (rf.ok) { e->latest_file_id = rf.file_id; e->latest_version = rf.version; }
        }
    }
    for (U64 i = 0; i < m->count; i += 1) {
        MgrAddon *e = &m->addons[i];
        if (!id_is(e->id, "gh:") || !id_selected(only, e->id)) continue;
        RemoteFile rf = gh_resolve(m->arena, m->client, id_rest(e->id));
        if (!rf.ok) { if (ok) first_error = rf.error; ok = 0; continue; }
        e->latest_file_id = rf.file_id;
        e->latest_version = rf.version;
    }
    if (!ok) m->error = first_error;
    return ok;
}

//- WTF backup
//
// Addon settings live in <flavor>\WTF. Before installing updates wam zips it to
// <flavor>\WTF-Backup\WTF-YYYY-MM-DD.zip, once per day, and keeps the newest WTF_BACKUPS_KEPT.
// The zip is written with Windows' tar.exe (bsdtar), under a temporary name first, so a
// half-written file never counts as today's backup.

#define WTF_BACKUPS_KEPT 7

static String8 mgr_flavor_dir(Arena *a, Mgr *m) { return path_dirname(path_dirname(m->addons_dir)); } // ...\_retail_

#define WTF_BACKUP_DATED 14 // the length of "WTF-YYYY-MM-DD"

// A name wam gives its backups, WTF-YYYY-MM-DD.zip or WTF-YYYY-MM-DD-<n>.zip: only these are ever
// deleted, not zips the user keeps next to them.
static B32 wtf_backup_name(String8 f)
{
    char *form = "WTF-dddd-dd-dd";
    if (f.size < WTF_BACKUP_DATED + 4 || !str8_ends_with(f, str8_lit(".zip"), 1)) return 0;
    for (U64 i = 0; i < WTF_BACKUP_DATED; i += 1) {
        if (form[i] == 'd' ? (f.str[i] < '0' || f.str[i] > '9') : f.str[i] != (U8)form[i]) return 0;
    }
    U64 end = f.size - 4;
    if (end == WTF_BACKUP_DATED) return 1;
    if (f.str[WTF_BACKUP_DATED] != '-' || end == WTF_BACKUP_DATED + 1) return 0;
    for (U64 i = WTF_BACKUP_DATED + 1; i < end; i += 1) if (f.str[i] < '0' || f.str[i] > '9') return 0;
    return 1;
}

// Oldest first: by date, then the day's first (no number), -2, ... -10.
static int wtf_backup_compare(const void *pa, const void *pb)
{
    const String8 *x = (const String8 *)pa, *y = (const String8 *)pb;
    int d = memcmp(x->str, y->str, WTF_BACKUP_DATED);
    if (!d) d = (x->size > y->size) - (x->size < y->size);
    return d ? d : memcmp(x->str, y->str, x->size);
}

static void wtf_backups_prune(Arena *a, String8 dir)
{
    OS_FileInfoArray files = os_list_dir(a, dir);
    String8 *zips = push_array(a, String8, files.count + 1);
    U64 n = 0;
    for (U64 i = 0; i < files.count; i += 1) {
        if (!files.v[i].is_dir && wtf_backup_name(files.v[i].name)) zips[n++] = files.v[i].name;
    }
    qsort(zips, n, sizeof(String8), wtf_backup_compare);
    for (U64 i = 0; i + WTF_BACKUPS_KEPT < n; i += 1) os_delete_tree(path_join(a, dir, zips[i]));
}

// Makes today's backup unless it exists (always a new one with `always`). *out_path: the zip, or
// empty when there was nothing to back up or today's already existed.
static B32 mgr_backup_wtf(Mgr *m, B32 always, String8 *out_path)
{
    Arena *a = m->arena;
    if (out_path) *out_path = str8(0, 0);
    String8 flavor = mgr_flavor_dir(a, m);
    String8 wtf = path_join(a, flavor, str8_lit("WTF"));
    if (!os_dir_exists(wtf)) return 1;
    String8 dir = path_join(a, flavor, str8_lit("WTF-Backup"));
    if (!os_make_dirs(dir)) return mgr_fail(m, "could not create %.*s", S8F(dir));

    SYSTEMTIME t;
    GetLocalTime(&t);
    String8 stem = str8_pushf(a, "WTF-%04u-%02u-%02u", t.wYear, t.wMonth, t.wDay);
    String8 zip = path_join(a, dir, str8_pushf(a, "%.*s.zip", S8F(stem)));
    if (os_file_exists(zip)) {
        if (!always) return 1;
        for (int k = 2; os_file_exists(zip); k += 1) zip = path_join(a, dir, str8_pushf(a, "%.*s-%d.zip", S8F(stem), k));
    }

    String8 tar = path_join(a, os_system_dir(a), str8_lit("tar.exe"));
    if (!os_file_exists(tar)) return mgr_fail(m, "backing up WTF needs tar.exe from Windows 10 (1803) or newer");
    String8 tmp = path_join(a, dir, str8_pushf(a, "%.*s.partial.zip", S8F(stem)));
    os_delete_tree(tmp);
    mgr_status(m, "Backing up addon settings (WTF)...");
    // -a picks the format from the .zip extension; -C makes the paths inside start at WTF\.
    OS_RunResult r = os_run(a, tar, str8_pushf(a, "\"%.*s\" -a -c -f \"%.*s\" -C \"%.*s\" WTF", S8F(tar), S8F(tmp), S8F(flavor)), 600000);
    if (!r.started || r.timed_out || r.exit_code != 0) {
        os_delete_tree(tmp);
        String8 why = !r.started ? str8_lit("tar.exe didn't start") : r.timed_out ? str8_lit("it took too long") : first_line(r.output);
        return mgr_fail(m, "could not back up WTF (%.*s), so no updates were installed", S8F(why));
    }
    if (!os_move(tmp, zip)) { os_delete_tree(tmp); return mgr_fail(m, "could not save the WTF backup to %.*s", S8F(zip)); }

    wtf_backups_prune(a, dir);

    if (out_path) *out_path = zip;
    return 1;
}

// Updates the tracked addons in `only`, or all of them when it's null or empty. Addons whose
// files were edited since wam installed them are skipped unless `force`.
static B32 mgr_update_list_ex(Mgr *m, String8List *only, B32 force)
{
    if (only) for (String8Node *it = only->first; it; it = it->next) {
        if (!mgr_find(m, it->string)) return mgr_fail(m, "%.*s isn't installed by wam", S8F(it->string));
    }
    U64 n = 0;
    for (U64 i = 0; i < m->count; i += 1) n += id_selected(only, m->addons[i].id);
    if (!n) { mgr_status(m, "Nothing to update"); return 1; }

    mgr_status(m, "Checking %llu addon%s for updates...", n, n == 1 ? "" : "s");
    B32 check_ok = mgr_refresh_latest(m, only);
    String8 check_error = m->error;

    // Collect ids first: installing reshuffles the entry array.
    String8 *ids = push_array(m->arena, String8, m->count + 1);
    String8List skipped = { 0 }, pinned = { 0 };
    U64 nids = 0;
    for (U64 i = 0; i < m->count; i += 1) {
        MgrAddon *e = &m->addons[i];
        if (!id_selected(only, e->id) || !mgr_has_update(e)) continue;
        if (e->pinned) { str8_list_push(m->arena, &pinned, e->name.size ? e->name : e->id); continue; } // even when named: unpin to update
        if (!force && mgr_is_edited(m, e)) { str8_list_push(m->arena, &skipped, e->name.size ? e->name : e->id); continue; }
        ids[nids++] = e->id;
    }

    String8 backup = { 0 };
    if (nids && !mgr_backup_wtf(m, 0, &backup)) { mgr_save(m); return 0; }

    U64 updated = 0, failed = 0;
    String8 first_error = { 0 }, first_url = { 0 };
    for (U64 i = 0; i < nids; i += 1) {
        RemoteFile rf = mgr_resolve(m, ids[i]);
        m->error_url = str8(0, 0);
        if (mgr_install_remote(m, &rf, 0)) updated += 1;
        else { failed += 1; if (!first_error.size) { first_error = m->error; first_url = m->error_url; } }
    }
    mgr_save(m);
    m->error_url = first_url;
    String8 skip_note = skipped.count
        ? str8_pushf(m->arena, " Kept %.*s as is: you edited %s files (their Overwrite edits button replaces them).",
                     S8F(str8_list_join(m->arena, &skipped, str8_lit(", "))), skipped.count == 1 ? "its" : "their")
        : str8(0, 0);
    if (pinned.count && (!only || !only->count)) {
        skip_note = str8_pushf(m->arena, "%.*s %llu pinned addon%s kept %s version.", S8F(skip_note), pinned.count, pinned.count == 1 ? "" : "s", pinned.count == 1 ? "its" : "their");
    } else if (pinned.count) {
        skip_note = str8_pushf(m->arena, "%.*s Kept %.*s as is: pinned (unpin to update).", S8F(skip_note), S8F(str8_list_join(m->arena, &pinned, str8_lit(", "))));
    }
    if (failed == 1) return mgr_fail(m, "%.*s%.*s", S8F(first_error), S8F(skip_note));
    if (failed) return mgr_fail(m, "%llu updates failed. First: %.*s%.*s", failed, S8F(first_error), S8F(skip_note));
    if (!check_ok && !updated) { m->error = check_error; return 0; }
    String8 backup_note = backup.size ? str8_pushf(m->arena, " (settings backed up to WTF-Backup\\%.*s)", S8F(path_filename(backup))) : str8(0, 0);
    if (updated) mgr_status(m, "Updated %llu addon%s%.*s.%.*s", updated, updated == 1 ? "" : "s", S8F(backup_note), S8F(skip_note));
    else if (skipped.count || pinned.count) mgr_status(m, "Nothing updated.%.*s", S8F(skip_note));
    else mgr_status(m, "Everything is up to date");
    return 1;
}

static B32 mgr_update_list(Mgr *m, String8List *only) { return mgr_update_list_ex(m, only, 0); }

// Pins (keeps at its version) or unpins a tracked addon.
static B32 mgr_set_pinned(Mgr *m, String8 id, B32 pinned)
{
    MgrAddon *e = mgr_find(m, id);
    if (!e) return mgr_fail(m, "%.*s isn't installed by wam", S8F(id));
    if (e->pinned == (pinned != 0)) return 1;
    e->pinned = pinned != 0;
    if (!mgr_save(m)) return 0;
    String8 name = e->name.size ? e->name : e->id;
    if (pinned) mgr_status(m, "Pinned %.*s at %.*s: updates leave it alone until you unpin it", S8F(name), S8F(e->version));
    else if (mgr_has_update(e)) mgr_status(m, "Unpinned %.*s; %.*s is available", S8F(name), S8F(e->latest_version));
    else mgr_status(m, "Unpinned %.*s", S8F(name));
    return 1;
}

// Updates one tracked addon, or all of them when `only` is empty.
static B32 mgr_update(Mgr *m, String8 only)
{
    String8List one = { 0 };
    if (only.size) str8_list_push(m->arena, &one, only);
    return mgr_update_list(m, &one);
}

static B32 mgr_remove(Mgr *m, String8 id)
{
    MgrAddon *e = mgr_find(m, id);
    if (!e) return mgr_fail(m, "%.*s isn't installed by wam", S8F(id));
    mgr_status(m, "Removing %.*s...", S8F(e->name));
    String8List left = { 0 };
    for (String8Node *n = e->folders.first; n; n = n->next) {
        String8 path = path_join(m->arena, m->addons_dir, n->string);
        if (os_dir_exists(path) && !os_recycle(path)) str8_list_push(m->arena, &left, n->string);
    }
    if (left.count) {
        e->folders = left;
        mgr_save(m);
        return mgr_fail(m, "could not remove %.*s - is WoW running?", S8F(left.first->string));
    }
    String8 name = e->name;
    mgr_delete_entry(m, e);
    if (!mgr_save(m)) return 0;
    mgr_status(m, "Removed %.*s (moved to the Recycle Bin)", S8F(name));
    return 1;
}

// Removes an addon folder by name; if wam tracks it, removes the whole addon.
static B32 mgr_remove_folder(Mgr *m, String8 folder)
{
    if (!folder_name_is_safe(folder)) return mgr_fail(m, "'%.*s' isn't an installed addon or folder", S8F(folder));
    MgrAddon *owner = mgr_owner_of(m, folder);
    if (owner) return mgr_remove(m, owner->id);
    String8 path = path_join(m->arena, m->addons_dir, folder);
    if (!os_dir_exists(path)) return mgr_fail(m, "%.*s doesn't exist", S8F(folder));
    if (!os_recycle(path)) return mgr_fail(m, "could not remove %.*s - is WoW running?", S8F(folder));
    mgr_status(m, "Removed %.*s (moved to the Recycle Bin)", S8F(folder));
    return 1;
}

//- Dependencies
//
// A "unit" is what the user sees as one addon: a tracked entry (all its folders), or one
// untracked folder. Edges come from the .toc's required dependencies (folder names),
// mapped to the unit that owns that folder; deps inside the same unit are ignored.

typedef struct MgrUnit {
    String8 key;              // addon id when tracked, else the folder name
    String8 title;
    MgrAddon *entry;          // tracked entry, or 0
    Addon *addon;             // scan data of the first folder, or 0
    String8List folders;
    U32 *req;                 // units this one requires
    U32 req_count;
} MgrUnit;

typedef struct MgrUnits { MgrUnit *v; U64 count; } MgrUnits;

static Addon *mgr_scan_find(AddonArray *scan, String8 folder)
{
    for (U64 i = 0; i < scan->count; i += 1) if (str8_match(scan->v[i].folder, folder, 1)) return &scan->v[i];
    return 0;
}

static S64 mgr_unit_of_folder(MgrUnits *u, String8 folder)
{
    for (U64 i = 0; i < u->count; i += 1) if (list_has_ci(&u->v[i].folders, folder)) return (S64)i;
    return -1;
}

static S64 mgr_unit_find(MgrUnits *u, String8 key)
{
    for (U64 i = 0; i < u->count; i += 1) if (str8_match(u->v[i].key, key, 1)) return (S64)i;
    return -1;
}

thread_static B32 units_updates_first; // qsort has no context argument; two worker threads sort at once

static int unit_compare(const void *pa, const void *pb)
{
    const MgrUnit *x = (const MgrUnit *)pa, *y = (const MgrUnit *)pb;
    if (units_updates_first) {
        B32 ux = x->entry && mgr_has_update(x->entry) && !x->entry->pinned, uy = y->entry && mgr_has_update(y->entry) && !y->entry->pinned;
        if (ux != uy) return uy - ux;
    }
    String8 s = x->title, t = y->title;
    U64 n = Min(s.size, t.size);
    for (U64 i = 0; i < n; i += 1) {
        int d = (int)char_lower(s.str[i]) - (int)char_lower(t.str[i]);
        if (d) return d;
    }
    return (s.size > t.size) - (s.size < t.size);
}

static MgrUnits mgr_units(Arena *a, Mgr *m, AddonArray *scan, B32 updates_first)
{
    MgrUnits u = { 0 };
    u.v = push_array(a, MgrUnit, m->count + scan->count + 1);
    for (U64 i = 0; i < m->count; i += 1) {
        MgrAddon *e = &m->addons[i];
        MgrUnit *x = &u.v[u.count++];
        x->key = e->id;
        x->entry = e;
        x->folders = e->folders;
        x->addon = mgr_scan_find(scan, e->folders.first->string);
        x->title = e->name.size ? e->name : e->folders.first->string;
    }
    for (U64 i = 0; i < scan->count; i += 1) {
        Addon *ad = &scan->v[i];
        if (mgr_owner_of(m, ad->folder)) continue;
        MgrUnit *x = &u.v[u.count++];
        x->key = ad->folder;
        x->addon = ad;
        str8_list_push(a, &x->folders, ad->folder);
        x->title = ad->title.size ? ad->title : ad->folder;
    }
    units_updates_first = updates_first;
    qsort(u.v, u.count, sizeof(MgrUnit), unit_compare);

    for (U64 i = 0; i < u.count; i += 1) {
        MgrUnit *x = &u.v[i];
        U64 cap = 0;
        for (String8Node *f = x->folders.first; f; f = f->next) {
            Addon *ad = mgr_scan_find(scan, f->string);
            if (ad) cap += ad->required_deps.count;
        }
        x->req = push_array(a, U32, cap + 1);
        for (String8Node *f = x->folders.first; f; f = f->next) {
            Addon *ad = mgr_scan_find(scan, f->string);
            if (!ad) continue;
            for (String8Node *d = ad->required_deps.first; d; d = d->next) {
                S64 j = mgr_unit_of_folder(&u, d->string);
                if (j < 0 || (U64)j == i) continue; // not installed (e.g. Blizzard_*), or part of the same addon
                B32 dup = 0;
                for (U32 k = 0; k < x->req_count; k += 1) dup |= x->req[k] == (U32)j;
                if (!dup) x->req[x->req_count++] = (U32)j;
            }
        }
    }
    return u;
}

// mark[i] == 1: chosen for removal. Marks with 2 every dependency that only chosen (or
// already marked) units need, transitively. Library addons that something else still
// requires stay.
static void mgr_mark_unused_deps(MgrUnits *u, U8 *mark)
{
    for (B32 changed = 1; changed;) {
        changed = 0;
        for (U64 j = 0; j < u->count; j += 1) {
            if (mark[j]) continue;
            B32 needed_by_marked = 0, needed_by_other = 0;
            for (U64 i = 0; i < u->count && !needed_by_other; i += 1) {
                for (U32 k = 0; k < u->v[i].req_count; k += 1) {
                    if (u->v[i].req[k] != j) continue;
                    if (mark[i]) needed_by_marked = 1; else needed_by_other = 1;
                }
            }
            if (needed_by_marked && !needed_by_other) { mark[j] = 2; changed = 1; }
        }
    }
}

// Units that stay installed but require something in `mark` (they'd stop loading).
static B32 mgr_unit_needs_marked(MgrUnits *u, U8 *mark, U64 i)
{
    if (mark[i]) return 0;
    for (U32 k = 0; k < u->v[i].req_count; k += 1) if (mark[u->v[i].req[k]]) return 1;
    return 0;
}

// Removes several addons: keys are addon ids or untracked folder names.
static B32 mgr_remove_many(Mgr *m, String8List *keys)
{
    U64 removed = 0, failed = 0;
    String8 first_error = { 0 };
    for (String8Node *n = keys->first; n; n = n->next) {
        B32 ok = mgr_find(m, n->string) ? mgr_remove(m, n->string) : mgr_remove_folder(m, n->string);
        if (ok) removed += 1;
        else { failed += 1; if (!first_error.size) first_error = m->error; }
    }
    if (failed) return mgr_fail(m, "removed %llu, %llu failed: %.*s", removed, failed, S8F(first_error));
    if (removed > 1) mgr_status(m, "Removed %llu addons (moved to the Recycle Bin)", removed);
    return 1;
}

//- Fingerprint cache
//
// Recognizing untracked folders needs their fingerprints (CurseForge's and Wago's), which means
// reading every file their .toc loads: seconds for a big guide addon when Windows doesn't have the
// files in memory, on every Check for updates. So they're kept in %LOCALAPPDATA%\wam\fingerprints.tsv
// with each folder's signature (its files' names, sizes and dates, from the directory listing:
// nothing is read), and a folder whose signature hasn't changed reuses them. One line per folder:
//   <full path, lowercase> \t <signature> \t <CurseForge fingerprint> \t <files> \t <Wago hash, or ->

#define FP_CACHE_MAX 20000 // lines kept: this AddOns folder's, then other folders' as they were

typedef struct FpSig { char s[33]; } FpSig;
typedef struct FpJob { AddonArray *scan; FpSig *sig; U8 *hit; } FpJob;

static void fp_sig_one(void *user, U64 i)
{
    FpJob *j = (FpJob *)user;
    char unused[33];
    folder_check(j->scan->v[i].path, j->sig[i].s, unused, 0);
}

static void fp_compute_one(void *user, U64 i)
{
    FpJob *j = (FpJob *)user;
    if (!j->hit[i]) addon_fingerprint_one(j->scan, i);
}

static String8 fp_cache_path(Arena *a)
{
    String8 local = os_get_env(a, "LOCALAPPDATA");
    return local.size ? path_join(a, path_join(a, local, str8_lit("wam")), str8_lit("fingerprints.tsv")) : local;
}

// addons_fingerprint, with the cache. *out_cached: how many folders came from it (may be 0).
static void mgr_fingerprint(Mgr *m, AddonArray *scan, U64 *out_cached)
{
    if (out_cached) *out_cached = 0;
    if (scan->fingerprinted) return;
    Arena *a = m->arena;
    String8 path = fp_cache_path(a);
    if (!path.size) { addons_fingerprint(scan); return; }
    U64 n = scan->count;
    FpJob j = { scan, push_array(a, FpSig, n + 1), push_array(a, U8, n + 1) };
    os_parallel_for(n, fp_sig_one, &j);

    // The cache's lines, and which of them are this scan's folders.
    String8List lines = { 0 };
    for (String8 rest = os_read_file(a, path); rest.size;) {
        String8 line = str8_pop_line(&rest);
        if (line.size) str8_list_push(a, &lines, line);
    }
    String8 *keys = push_array(a, String8, n + 1);
    for (U64 i = 0; i < n; i += 1) keys[i] = str8_lower(a, scan->v[i].path);
    U64 cached = 0;
    for (String8Node *ln = lines.first; ln; ln = ln->next) {
        String8 f[5] = { 0 };
        String8 rest = ln->string;
        for (int k = 0; k < 5; k += 1) {
            U64 tab = str8_find(rest, 0, str8_lit("\t"), 0);
            f[k] = str8_prefix(rest, tab);
            rest = str8_skip(rest, tab + 1);
        }
        if (f[1].size != 32 || !(f[4].size == 32 || str8_match(f[4], str8_lit("-"), 0))) continue;
        for (U64 i = 0; i < n; i += 1) {
            if (j.hit[i] || !str8_match(keys[i], f[0], 0) || memcmp(j.sig[i].s, f[1].str, 32) != 0) continue;
            Addon *ad = &scan->v[i];
            ad->fingerprint = (U32)str8_to_s64(f[2]);
            ad->fingerprint_files = (U32)str8_to_s64(f[3]);
            if (f[4].size == 32) { memcpy(ad->wago_hash, f[4].str, 32); ad->wago_hash[32] = 0; }
            else ad->wago_hash[0] = 0;
            j.hit[i] = 1;
            cached += 1;
            break;
        }
    }

    // The rest the slow way, in parallel.
    os_parallel_for(n, fp_compute_one, &j);
    scan->files_hashed = 0;
    for (U64 i = 0; i < n; i += 1) if (!j.hit[i]) scan->files_hashed += scan->v[i].fingerprint_files;
    scan->fingerprinted = 1;
    if (out_cached) *out_cached = cached;
    if (cached == n) return; // nothing new to remember

    // This folder's lines (as they are now), then other folders' as they were, up to the limit.
    String8 prefix = str8_lower(a, path_join(a, m->addons_dir, str8_lit("")));
    String8List out = { 0 };
    for (U64 i = 0; i < n; i += 1) {
        Addon *ad = &scan->v[i];
        str8_list_push(a, &out, str8_pushf(a, "%.*s\t%s\t%u\t%u\t%s\n", S8F(keys[i]), j.sig[i].s, ad->fingerprint, ad->fingerprint_files,
                                           ad->wago_hash[0] ? ad->wago_hash : "-"));
    }
    for (String8Node *ln = lines.first; ln && out.count < FP_CACHE_MAX; ln = ln->next) {
        if (!str8_starts_with(ln->string, prefix, 0)) str8_list_push(a, &out, str8_pushf(a, "%.*s\n", S8F(ln->string)));
    }
    String8 tmp = str8_pushf(a, "%.*s.tmp", S8F(path));
    if (os_make_dirs(path_dirname(path)) && os_write_file(tmp, str8_list_join(a, &out, str8_lit("")))) os_replace_file(tmp, path);
}

//- Sync: recognize untracked addons, then check everything for updates.

static B32 versions_equal(String8 a, String8 b)
{
    a = str8_trim(a); b = str8_trim(b);
    if (a.size && (a.str[0] == 'v' || a.str[0] == 'V')) a = str8_skip(a, 1);
    if (b.size && (b.str[0] == 'v' || b.str[0] == 'V')) b = str8_skip(b, 1);
    return a.size && str8_match(a, b, 1);
}

static U64 mgr_adopt_curseforge(Mgr *m, AddonArray *scan)
{
    S64 *fps = push_array(m->arena, S64, scan->count + 1);
    U64 nfp = 0, adopted = 0;
    for (U64 i = 0; i < scan->count; i += 1) if (!mgr_owner_of(m, scan->v[i].folder)) fps[nfp++] = scan->v[i].fingerprint;
    if (!nfp) return 0;
    mgr_status(m, "Identifying %llu addon folder%s on CurseForge...", nfp, nfp == 1 ? "" : "s");
    HTTP_Result r = cf_match_fingerprints(m->arena, m->api_key, fps, nfp);
    if (!r.ok) { mgr_fail(m, "CurseForge fingerprint lookup failed: %.*s", S8F(r.error)); return 0; }
    JSON *data = json_get(json_parse(m->arena, r.body), "data");
    // Exact matches first; partial ones are installs missing some of the file's folders
    // (e.g. someone deleted optional plugins). Either way only folders that matched are claimed.
    JSON *lists[2] = { json_get(data, "exactMatches"), json_get(data, "partialMatches") };
    for (int li = 0; li < 2; li += 1)
    json_each(match, lists[li]) {
        String8 id = str8_pushf(m->arena, "cf:%lld", json_int(json_get(match, "id")));
        JSON *file = json_get(match, "file");
        String8List folders = { 0 };
        json_each(mod, json_get(file, "modules")) {
            String8 f = json_str(json_get(mod, "name"));
            for (U64 i = 0; i < scan->count; i += 1) {
                if (str8_match(scan->v[i].folder, f, 1) && !mgr_owner_of(m, f) && !list_has_ci(&folders, f)) {
                    str8_list_push(m->arena, &folders, scan->v[i].folder);
                }
            }
        }
        if (!folders.count) continue;
        MgrAddon *e = mgr_find(m, id);
        if (!e) {
            e = mgr_add(m);
            if (!e) break;
            e->id = id;
            e->file_id = json_int(json_get(file, "id"));
            e->version = json_str(json_get(file, "displayName"));
            e->name = folders.first->string;
            adopted += 1;
        }
        for (String8Node *n = folders.first; n; n = n->next) str8_list_push(m->arena, &e->folders, n->string);
    }
    return adopted;
}

// Untracked folders whose .toc has ## X-WoWI-ID. If the installed version matches the
// one on WoWInterface we know which file it is; otherwise it's marked as needing an update.
static U64 mgr_adopt_wowi(Mgr *m, AddonArray *scan)
{
    S64 *ids = push_array(m->arena, S64, scan->count + 1);
    U64 n = 0, adopted = 0;
    for (U64 i = 0; i < scan->count; i += 1) {
        Addon *ad = &scan->v[i];
        S64 id = str8_to_s64(ad->wowi_id);
        if (id <= 0 || mgr_owner_of(m, ad->folder)) continue;
        B32 dup = 0;
        for (U64 k = 0; k < n; k += 1) dup |= ids[k] == id;
        if (!dup) ids[n++] = id;
    }
    if (!n) return 0;
    mgr_status(m, "Identifying %llu addon%s on WoWInterface...", n, n == 1 ? "" : "s");
    HTTP_Result r = wowi_details(m->arena, ids, n);
    if (!r.ok) { mgr_fail(m, "WoWInterface lookup failed: %.*s", S8F(r.error)); return 0; }
    json_each(item, json_parse(m->arena, r.body)) {
        RemoteFile rf = wowi_file_from_json(m->arena, item);
        if (!rf.ok || mgr_find(m, rf.id)) continue;
        S64 uid = id_number(rf.id);
        MgrAddon *e = 0;
        for (U64 i = 0; i < scan->count; i += 1) {
            Addon *ad = &scan->v[i];
            if (str8_to_s64(ad->wowi_id) != uid || mgr_owner_of(m, ad->folder)) continue;
            if (!e) {
                e = mgr_add(m);
                if (!e) return adopted;
                e->id = rf.id;
                e->name = rf.name;
                e->website = rf.website;
                e->version = ad->version;
                e->file_id = versions_equal(ad->version, rf.version) ? rf.file_id : 0;
                adopted += 1;
            }
            str8_list_push(m->arena, &e->folders, ad->folder);
        }
    }
    return adopted;
}

// Untracked folders matched on Wago by fingerprint (or by ## X-Wago-ID). Folders that match the
// installed release's hash record that release; folders found only by id need an update.
static U64 mgr_adopt_wago(Mgr *m, AddonArray *scan)
{
    Temp scratch = scratch_begin(m->arena);
    Arena *s = scratch.arena;
    String8List parts = { 0 };
    U64 n = 0, adopted = 0;
    for (U64 i = 0; i < scan->count; i += 1) {
        Addon *ad = &scan->v[i];
        if (mgr_owner_of(m, ad->folder) || !ad->fingerprint_files) continue;
        String8 wago = ad->wago_id.size ? str8_pushf(s, ",\"wago\":\"%.*s\"", S8F(json_escape(s, ad->wago_id))) : str8_lit("");
        str8_list_push(s, &parts, str8_pushf(s, "{\"name\":\"%.*s\",\"hash\":\"%s\"%.*s}", S8F(json_escape(s, ad->folder)), ad->wago_hash, S8F(wago)));
        n += 1;
    }
    if (!n) { scratch_end(scratch); return 0; }
    mgr_status(m, "Identifying %llu addon folder%s on Wago...", n, n == 1 ? "" : "s");
    String8 body = str8_pushf(s, "{\"game_version\":\"%s\",\"addons\":[%.*s]}", wago_game_version(m->client),
                              S8F(str8_list_join(s, &parts, str8_lit(","))));
    HTTP_Result r = wago_call(m->arena, m->wago_key, "POST", str8_lit("/addons/_match"), body);
    scratch_end(scratch);
    if (!r.ok) { mgr_fail(m, "Wago fingerprint lookup failed: %.*s", S8F(r.error)); return 0; }

    json_each(item, json_get(json_parse(m->arena, r.body), "addons")) {
        if (item->type != JSON_OBJ) continue; // the API sometimes returns null entries
        String8 wid = json_str(json_get(item, "id"));
        if (!wid.size) continue;
        String8 id = str8_pushf(m->arena, "wago:%.*s", S8F(wid));
        String8List folders = { 0 };
        B32 all_hashes_match = 1;
        json_each(mod, json_get(item, "modules")) {
            Addon *ad = mgr_scan_find(scan, mod->key);
            if (!ad || mgr_owner_of(m, ad->folder) || list_has_ci(&folders, ad->folder)) continue;
            B32 by_hash = str8_match(json_str(json_get(mod, "hash")), str8_cstring(ad->wago_hash), 1);
            B32 by_id = ad->wago_id.size && str8_match(ad->wago_id, wid, 0);
            if (!by_hash && !by_id) continue;
            all_hashes_match &= by_hash;
            str8_list_push(m->arena, &folders, ad->folder);
        }
        if (!folders.count || mgr_find(m, id)) continue;
        MgrAddon *e = mgr_add(m);
        if (!e) break;
        JSON *matched = json_get(item, "matched_release");
        e->id = id;
        e->name = json_str(json_get(item, "name"));
        if (!e->name.size) e->name = folders.first->string;
        e->website = json_str(json_get(item, "website_url"));
        e->file_id = all_hashes_match && matched->type == JSON_OBJ ? iso_seconds(json_str(json_get(matched, "created_at"))) : 0;
        e->version = matched->type == JSON_OBJ ? json_str(json_get(matched, "label")) : str8(0, 0);
        if (!e->version.size) { Addon *ad = mgr_scan_find(scan, folders.first->string); if (ad) e->version = ad->version; }
        e->folders = folders;
        adopted += 1;
    }
    return adopted;
}

// Untracked ElvUI / Tukui installs: Tukui's list names each addon's folders. The installed version
// comes from the main folder's .toc; if it isn't Tukui's current one, the addon shows an update.
static U64 mgr_adopt_tukui(Mgr *m, AddonArray *scan)
{
    // Only ask Tukui when a folder it could own is lying around untracked.
    static char *known[] = { "ElvUI", "Tukui" };
    B32 worth = 0;
    for (U64 i = 0; i < scan->count && !worth; i += 1) {
        if (mgr_owner_of(m, scan->v[i].folder)) continue;
        for (int k = 0; k < (int)ArrayCount(known); k += 1) worth |= str8_starts_with(scan->v[i].folder, str8_cstring(known[k]), 1);
    }
    if (!worth) return 0;
    String8 err = { 0 };
    JSON *list = tukui_list(m->arena, &err);
    if (err.size) { mgr_fail(m, "%.*s", S8F(err)); return 0; }
    U64 adopted = 0;
    json_each(item, list) {
        RemoteFile rf = tukui_file_from_json(m->arena, m->client, item);
        if (!rf.id.size || mgr_find(m, rf.id)) continue;
        String8List folders = { 0 };
        json_each(d, json_get(item, "directories")) {
            Addon *ad = mgr_scan_find(scan, json_str(d));
            if (ad && !mgr_owner_of(m, ad->folder)) str8_list_push(m->arena, &folders, ad->folder);
        }
        JSON *dirs = json_get(item, "directories");
        Addon *main_ad = dirs->first ? mgr_scan_find(scan, json_str(dirs->first)) : 0;
        if (!folders.count || !main_ad || mgr_owner_of(m, main_ad->folder)) continue; // its main folder must be there
        MgrAddon *e = mgr_add(m);
        if (!e) break;
        e->id = rf.id;
        e->name = rf.name;
        e->website = rf.website;
        e->version = main_ad->version;
        e->folders = folders;
        adopted += 1;
    }
    return adopted;
}

static B32 mgr_sync(Mgr *m, AddonArray *scan)
{
    mgr_prune(m);
    mgr_fingerprint(m, scan, 0); // recognition needs them (cached per folder); plain listing doesn't
    B32 ok = 1;
    String8 first_error = { 0 };
    U64 adopted = 0;
    // Wago first when the user set it up: it carries addons whose authors block other apps on CurseForge.
    if (m->wago_key.size) {
        adopted += mgr_adopt_wago(m, scan);
        if (m->error.size) { ok = 0; first_error = m->error; m->error = str8(0, 0); }
    }
    if (m->api_key.size) {
        adopted += mgr_adopt_curseforge(m, scan);
        if (m->error.size) { if (ok) first_error = m->error; ok = 0; m->error = str8(0, 0); }
    }
    adopted += mgr_adopt_wowi(m, scan);
    if (m->error.size && ok) { ok = 0; first_error = m->error; m->error = str8(0, 0); }
    adopted += mgr_adopt_tukui(m, scan);
    if (m->error.size && ok) { ok = 0; first_error = m->error; }

    // Addons tracked before wam recorded checksums (or just recognized): take their state now as the baseline.
    for (U64 i = 0; i < m->count; i += 1) if (!m->addons[i].checks.count) mgr_record_checks(m, &m->addons[i]);

    if (m->count) {
        mgr_status(m, "Checking %llu addon%s for updates...", m->count, m->count == 1 ? "" : "s");
        if (!mgr_refresh_latest(m, 0) && ok) { ok = 0; first_error = m->error; }
    }
    if (!mgr_save(m)) return 0;

    U64 untracked = 0;
    for (U64 i = 0; i < scan->count; i += 1) untracked += !mgr_owner_of(m, scan->v[i].folder);
    if (!ok) { m->error = first_error; return 0; }
    // Only what's new: the counts are on screen already. Nothing to say clears "Checking...".
    String8List say = { 0 };
    if (adopted) str8_list_push(m->arena, &say, str8_pushf(m->arena, "Recognized %llu more addon%s", adopted, adopted == 1 ? "" : "s"));
    if (untracked && !m->api_key.size && !m->wago_key.size) str8_list_push(m->arena, &say, str8_lit("Recognizing more addons needs a CurseForge or Wago key"));
    mgr_status(m, "%.*s", S8F(str8_list_join(m->arena, &say, str8_lit(". "))));
    return 1;
}

//- Search and browse (Tukui, Wago when the user has a key, CurseForge)

#define SHOTS_LISTED 6   // in the catalog
#define SHOTS_MAX    30  // in an addon's details
static U32 shots_cap(Mgr *m) { return m->all_shots ? SHOTS_MAX : SHOTS_LISTED; }

// Adds one screenshot ("thumb|full", or just "thumb") to a shots list, its links made safe for it.
static void shots_push(Arena *a, String8List *l, String8 thumb, String8 full)
{
    thumb = url_escape_unsafe(a, thumb);
    full = url_escape_unsafe(a, full);
    str8_list_push(a, l, full.size && !str8_match(full, thumb, 0) ? str8_pushf(a, "%.*s|%.*s", S8F(thumb), S8F(full)) : thumb);
}

static void cf_search_result(Mgr *m, JSON *mod, SearchResult *sr)
{
    sr->id = str8_pushf(m->arena, "cf:%lld", json_int(json_get(mod, "id")));
    sr->source = "CurseForge";
    sr->name = json_str(json_get(mod, "name"));
    sr->summary = json_str(json_get(mod, "summary"));
    sr->downloads = json_int(json_get(mod, "downloadCount"));
    sr->website = json_str(json_get(json_get(mod, "links"), "websiteUrl"));
    sr->updated = iso_seconds(json_str(json_get(mod, "dateReleased")));
    sr->created = iso_seconds(json_str(json_get(mod, "dateCreated")));
    JSON *authors = json_get(mod, "authors");
    if (authors->first) sr->author = json_str(json_get(authors->first, "name"));
    sr->available = mgr_pick_file(m, mod) != 0;
    sr->cats = cat_cf_bits(json_get(mod, "categories"), &sr->subcats);
    if (!json_bool(json_get(mod, "allowModDistribution"), 1))
        sr->site_only = id_is(mgr_parse_spec(m->arena, json_str(json_get(json_get(mod, "links"), "sourceUrl"))), "gh:") ? SITE_ONLY_GITHUB : SITE_ONLY_PAGE;
    sr->icon = url_escape_unsafe(m->arena, json_str(json_get(json_get(mod, "logo"), "thumbnailUrl")));
    String8List shots = { 0 };
    json_each(sh, json_get(mod, "screenshots")) {
        String8 th = json_str(json_get(sh, "thumbnailUrl")), full = json_str(json_get(sh, "url"));
        if (th.size && shots.count < shots_cap(m)) shots_push(m->arena, &shots, th, full);
    }
    sr->shots = shots_join(m->arena, &shots);
}

static void tukui_search_result(Mgr *m, JSON *item, SearchResult *sr)
{
    RemoteFile rf = tukui_file_from_json(m->arena, m->client, item);
    sr->id = rf.id;
    sr->source = "Tukui";
    sr->name = rf.name;
    sr->summary = json_str(json_get(item, "small_desc"));
    sr->author = json_str(json_get(item, "author"));
    sr->website = rf.website;
    sr->updated = rf.date_days * 86400;
    sr->available = rf.ok;
    sr->icon = url_escape_unsafe(m->arena, json_str(json_get(item, "logo_square_url")));
    // screenshot_url, then gallery_url: a list (or the list written as one string, "['https://...', ...]").
    String8List shots = { 0 };
    String8 first = json_str(json_get(item, "screenshot_url"));
    if (first.size) shots_push(m->arena, &shots, first, str8(0, 0));
    JSON *gal = json_get(item, "gallery_url");
    json_each(g, gal) if (shots.count < shots_cap(m) && str8_starts_with(json_str(g), str8_lit("https://"), 1)) shots_push(m->arena, &shots, json_str(g), str8(0, 0));
    String8 gallery = json_str(gal); // older responses: the list written as one string
    for (U64 i = 0; i < gallery.size && shots.count < shots_cap(m);) {
        U64 at = str8_find(gallery, i, str8_lit("https://"), 0);
        if (at >= gallery.size) break;
        U64 end = at;
        while (end < gallery.size && gallery.str[end] != '\'' && gallery.str[end] != '"' && gallery.str[end] != ',' && gallery.str[end] != ']' && gallery.str[end] > ' ') end += 1;
        String8 u = str8_substr(gallery, at, end);
        if (!str8_match(u, first, 0)) shots_push(m->arena, &shots, u, str8(0, 0));
        i = end;
    }
    sr->shots = shots_join(m->arena, &shots);
}

static void wowi_search_result(Mgr *m, JSON *item, SearchResult *sr)
{
    sr->id = str8_pushf(m->arena, "wowi:%lld", json_int(json_get(item, "UID")));
    sr->source = "WoWInterface";
    sr->name = json_str(json_get(item, "UIName"));
    sr->author = json_str(json_get(item, "UIAuthorName"));
    sr->website = json_str(json_get(item, "UIFileInfoURL"));
    sr->downloads = json_int(json_get(item, "UIDownloadTotal"));
    sr->updated = json_int(json_get(item, "UIDate")) / 1000;
    sr->available = wowi_compat_ok(item, m->client);
    sr->cats = cat_wowi_bits(json_int(json_get(item, "UICATID")), &sr->subcats);
    JSON *thumbs = json_get(item, "UIIMG_Thumbs");
    if (thumbs->first) sr->icon = url_escape_unsafe(m->arena, json_str(thumbs->first));
    // WoWInterface's thumbnails are tiny: its full pictures make the screenshot thumbnails.
    String8List shots = { 0 };
    json_each(img, json_get(item, "UIIMGs")) if (shots.count < shots_cap(m) && json_str(img).size) shots_push(m->arena, &shots, json_str(img), str8(0, 0));
    sr->shots = shots_join(m->arena, &shots);
}

// One addon from a Wago search (or browse) result.
static void wago_search_result(Mgr *m, JSON *item, SearchResult *sr)
{
    sr->id = str8_pushf(m->arena, "wago:%.*s", S8F(json_str(json_get(item, "id"))));
    sr->source = "Wago";
    sr->name = json_str(json_get(item, "display_name"));
    sr->summary = json_str(json_get(item, "summary"));
    sr->downloads = json_int(json_get(item, "download_count"));
    sr->website = json_str(json_get(item, "website_url"));
    sr->icon = url_escape_unsafe(m->arena, json_str(json_get(item, "thumbnail_image")));
    sr->cats = cat_wago_bits(json_get(item, "categories"), &sr->subcats);
    // Its authors, then the owner if not among them ("Stanzilla, MysticalOS, QartemisT"): the catalog
    // pairs it with its CurseForge entry by a shared author (cat_link), and CurseForge lists one.
    String8List authors = { 0 };
    String8 owner = json_str(json_get(item, "owner"));
    B32 owner_listed = 0;
    json_each(au, json_get(item, "authors")) {
        String8 name = str8_trim(json_str(au));
        if (!name.size || authors.count >= 4) continue;
        owner_listed |= str8_match(name, owner, 1);
        str8_list_push(m->arena, &authors, name);
    }
    if (owner.size && !owner_listed && authors.count < 4) str8_list_push(m->arena, &authors, owner);
    sr->author = str8_list_join(m->arena, &authors, str8_lit(", "));
    JSON *stable = wago_stable_release(json_get(item, "releases"));
    sr->available = stable != 0;
    if (stable) sr->updated = iso_seconds(json_str(json_get(stable, "created_at")));
}

// One page of results. An empty query browses (Tukui's list, then CurseForge sorted by cf_sort);
// Wago joins searches (it has no browse API), on the first page only. *cf_count: how many came
// from CurseForge, so callers know whether there's a next page (CF_PAGE_SIZE = maybe more).
static SearchResult *mgr_search_page(Mgr *m, String8 query, String8 cf_categories, int cf_sort, U64 index, U64 *out_count, U64 *cf_count)
{
    *out_count = 0;
    if (cf_count) *cf_count = 0;
    // Tukui hosts only a handful of addons (ElvUI, Tukui): match its whole list locally. Needs no key.
    JSON *tukui = index == 0 ? tukui_list(m->arena, 0) : &json_nil;
    U64 ntukui = 0;
    json_each(item, tukui) ntukui += !query.size || fuzzy_match(query, json_str(json_get(item, "name"))).score >= 0;
    B32 use_wago = m->wago_key.size && query.size && index == 0;
    if (!m->api_key.size && !use_wago && !ntukui) { mgr_require_key(m); return 0; }
    if (query.size) mgr_status(m, "Searching for \"%.*s\"...", S8F(query));
    JSON *wago = &json_nil, *cf = &json_nil;
    String8 wago_error = { 0 }, cf_error = { 0 };
    if (use_wago) {
        HTTP_Result r = wago_search(m->arena, m->wago_key, m->client, query);
        if (r.ok) wago = json_get(json_parse(m->arena, r.body), "data");
        else wago_error = str8_pushf(m->arena, "Wago search failed: %.*s", S8F(r.error));
    }
    if (m->api_key.size) {
        HTTP_Result r = cf_search_page(m->arena, m->api_key, query, str8(0, 0), cf_categories, cf_sort, index);
        if (r.ok) cf = json_get(json_parse(m->arena, r.body), "data");
        else cf_error = str8_pushf(m->arena, "CurseForge search failed: %.*s", S8F(r.error));
    }
    // Fail only when every source that was asked failed; otherwise show what came back.
    if (!ntukui && wago_error.size && (cf_error.size || !m->api_key.size)) { mgr_fail(m, "%.*s", S8F(wago_error)); return 0; }
    if (!ntukui && cf_error.size && !use_wago) { mgr_fail(m, "%.*s", S8F(cf_error)); return 0; }

    SearchResult *results = push_array(m->arena, SearchResult, ntukui + wago->count + cf->count + 1);
    U64 n = 0;
    json_each(item, tukui) {
        if (query.size && fuzzy_match(query, json_str(json_get(item, "name"))).score < 0) continue;
        tukui_search_result(m, item, &results[n++]);
    }
    json_each(item, wago) {
        if (item->type != JSON_OBJ) continue;
        wago_search_result(m, item, &results[n++]);
    }
    json_each(mod, cf) {
        if (mod->type != JSON_OBJ) continue;
        cf_search_result(m, mod, &results[n++]);
        if (cf_count) *cf_count += 1;
    }
    *out_count = n;
    String8 partial = wago_error.size ? wago_error : cf_error;
    if (query.size && partial.size) mgr_status(m, "%llu result%s for \"%.*s\" (%.*s)", n, n == 1 ? "" : "s", S8F(query), S8F(partial));
    else if (query.size) mgr_status(m, "%llu result%s for \"%.*s\"", n, n == 1 ? "" : "s", S8F(query));
    return results;
}

static SearchResult *mgr_search(Mgr *m, String8 query, U64 *out_count)
{
    return mgr_search_page(m, query, str8(0, 0), CF_SORT_POPULARITY, 0, out_count, 0);
}
