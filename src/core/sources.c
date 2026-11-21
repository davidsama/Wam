// sources.c - keyless addon sources: GitHub releases and WoWInterface.
//
// Every source resolves an addon id to a RemoteFile: what to download and how to
// recognize a newer version (file_id only ever grows for a given addon).
//   gh:owner/repo  - latest GitHub release; file_id = asset id
//   wowi:12345     - WoWInterface project; file_id = upload date (ms)

typedef struct RemoteFile {
    B32 ok;
    String8 error;
    String8 id;
    S64 file_id;
    String8 name, version, website, url;
    S64 cf_deps[16];   // required CurseForge dependencies
    U32 cf_dep_count;
    S64 date_days;     // release date, days since 1970 (0 = unknown)
    String8 error_url; // on failure: a page where the user can get the file by hand
    String8 fallback_from; // set when this replaces a CurseForge file the author doesn't let other apps download
    // A CurseForge file other apps may not download: its own download page (which starts the download
    // by itself), and the archive's name and size so wam can recognize it when the browser saves it.
    String8 manual_url, manual_file;
    S64 manual_size;
} RemoteFile;

// "1.60.1" -> { 1, 60, 1 }; stops at the first character that isn't a digit or a dot.
static void parse_version3(String8 s, U32 out[3])
{
    out[0] = out[1] = out[2] = 0;
    int part = 0;
    for (U64 i = 0; i < s.size && part < 3; i += 1) {
        if (s.str[i] >= '0' && s.str[i] <= '9') out[part] = out[part] * 10 + (s.str[i] - '0');
        else if (s.str[i] == '.') part += 1;
        else break;
    }
}

static U32 game_interface(GameVersion v) { return v.major * 10000 + v.minor * 100 + v.patch; }

// "2026-11-07T06:53:14Z" -> days since 1970-01-01 (0 if unparseable).
static S64 iso_days(String8 s)
{
    if (s.size < 10 || s.str[4] != '-' || s.str[7] != '-') return 0;
    S64 y = str8_to_s64(str8_prefix(s, 4)), m = str8_to_s64(str8_substr(s, 5, 7)), d = str8_to_s64(str8_substr(s, 8, 10));
    if (y < 1970 || m < 1 || m > 12 || d < 1 || d > 31) return 0;
    y -= m <= 2;
    S64 era = y / 400, yoe = y - era * 400;
    S64 doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    S64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

// Flavor names used by the BigWigs packager (release.json, zip suffixes).
static char *game_flavor_name(GameVersion v)
{
    if (v.major >= 10) return "mainline";
    if (v.major == 1) return v.minor >= 60 ? "forever" : "classic";
    if (v.major == 2) return "bcc";
    if (v.major == 3) return "wrath";
    if (v.major == 4) return "cata";
    if (v.major == 5) return "mists";
    return "mainline";
}

//- GitHub

#define GH_HEADERS "Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n"

static JSON *gh_find_asset(JSON *assets, String8 name)
{
    json_each(a, assets) if (str8_match(json_str(json_get(a, "name")), name, 1)) return a;
    return 0;
}

// Classifies a zip name like "Addon-v1.2-classic.zip" by its flavor suffix; "" = no suffix (mainline).
static char *gh_zip_flavor(Arena *a, String8 name)
{
    static char *map[][2] = {
        { "-forever", "forever" }, { "-classic", "classic" }, { "-vanilla", "classic" }, { "-era", "classic" },
        { "-bcc", "bcc" }, { "-tbc", "bcc" }, { "-wrath", "wrath" }, { "-wotlkc", "wrath" }, { "-cata", "cata" },
        { "-mists", "mists" }, { "-mop", "mists" },
    };
    String8 lower = str8_lower(a, name);
    for (U64 i = 0; i < lower.size; i += 1) if (lower.str[i] == '_') lower.str[i] = '-';
    for (int i = 0; i < (int)ArrayCount(map); i += 1) if (str8_find(lower, 0, str8_cstring(map[i][0]), 0) < lower.size) return map[i][1];
    return "";
}

// The best file in a BigWigs-packager release.json for this game version: exact interface, then same
// minor, then same major; full builds before -nolib. Empty if none fits.
static String8 gh_pick_from_release_json(JSON *root, GameVersion cv)
{
    U32 want = game_interface(cv);
    int best = 0;
    String8 best_file = { 0 };
    json_each(entry, json_get(root, "releases")) {
        int match = 0;
        json_each(md, json_get(entry, "metadata")) {
            U32 iface = (U32)json_int(json_get(md, "interface"));
            int s = iface == want ? 3 : iface / 100 == want / 100 ? 2 : iface / 10000 == want / 10000 ? 1 : 0;
            match = Max(match, s);
        }
        int score = match * 2 + !json_bool(json_get(entry, "nolib"), 0);
        if (match && score > best) { best = score; best_file = json_str(json_get(entry, "filename")); }
    }
    return best_file;
}

// Through the API (60 requests an hour without a token). Gives asset ids and the release date.
static RemoteFile gh_resolve_api(Arena *a, GameVersion cv, String8 repo)
{
    RemoteFile rf = { 0 };
    rf.id = str8_pushf(a, "gh:%.*s", S8F(repo));
    rf.website = str8_pushf(a, "https://github.com/%.*s", S8F(repo));
    rf.name = path_filename(repo);

    HTTP_Result r = http_get_url(a, str8_pushf(a, "https://api.github.com/repos/%.*s/releases/latest", S8F(repo)), str8_lit(GH_HEADERS), 0, 0);
    if (r.status == 404) { rf.error = str8_pushf(a, "%.*s has no releases on GitHub (or the repository doesn't exist)", S8F(repo)); return rf; }
    if (r.status == 403 || r.status == 429) { rf.error = str8_lit("GitHub rate limit reached (60 requests per hour). Try again later."); return rf; }
    if (!r.ok) { rf.error = str8_pushf(a, "GitHub request for %.*s failed: %.*s", S8F(repo), S8F(r.error)); return rf; }
    JSON *rel = json_parse(a, r.body);
    JSON *assets = json_get(rel, "assets");
    rf.version = json_str(json_get(rel, "tag_name"));
    rf.date_days = iso_days(json_str(json_get(rel, "published_at")));

    JSON *chosen = 0;

    // 1. BigWigs-packager metadata: exact interface, then same minor, then same major.
    JSON *meta = gh_find_asset(assets, str8_lit("release.json"));
    if (meta && cv.valid) {
        HTTP_Result mr = http_get_url(a, json_str(json_get(meta, "browser_download_url")), str8(0, 0), 0, 0);
        String8 best_file = gh_pick_from_release_json(json_parse(a, mr.body), cv);
        if (best_file.size) chosen = gh_find_asset(assets, best_file);
    }

    // 2. Otherwise go by zip names.
    if (!chosen) {
        char *flavor = game_flavor_name(cv);
        JSON *only = 0, *exact = 0, *plain = 0, *era = 0;
        int zips = 0;
        json_each(asset, assets) {
            String8 name = json_str(json_get(asset, "name"));
            if (!str8_ends_with(name, str8_lit(".zip"), 1)) continue;
            zips += 1;
            only = asset;
            if (str8_find(name, 0, str8_lit("nolib"), 1) < name.size) continue;
            char *f = gh_zip_flavor(a, name);
            if (!strcmp(f, flavor) && !exact) exact = asset;
            if (!f[0] && !plain) plain = asset;
            if (!strcmp(f, "classic") && !era) era = asset;
        }
        if (exact) chosen = exact;
        else if (!strcmp(flavor, "forever") && era) chosen = era;
        else if (!strcmp(flavor, "mainline") && plain) chosen = plain;
        else if (zips == 1) chosen = only;
    }
    if (!chosen) {
        rf.error = str8_pushf(a, "%.*s %.*s has no download for game version %u.%u.%u", S8F(repo), S8F(rf.version), cv.major, cv.minor, cv.patch);
        return rf;
    }
    rf.url = json_str(json_get(chosen, "browser_download_url"));
    rf.file_id = json_int(json_get(chosen, "id"));
    rf.ok = rf.url.size && rf.file_id;
    if (!rf.ok) rf.error = str8_lit("unexpected GitHub response");
    return rf;
}

// The newest release without the API: the tag comes from the redirect of
// github.com/<repo>/releases/latest and the file from that release's release.json (the BigWigs
// packager, which most addons on GitHub use, uploads one), downloaded from releases/download/.
// Neither counts against the API limit. Repositories without a release.json, tags that don't fit a
// URL, and anything unexpected fall back to the API. file_id stays 0: for gh: ids an update is a
// different tag (see mgr_has_update).
static RemoteFile gh_resolve(Arena *a, GameVersion cv, String8 repo)
{
    RemoteFile rf = { 0 };
    rf.id = str8_pushf(a, "gh:%.*s", S8F(repo));
    rf.website = str8_pushf(a, "https://github.com/%.*s", S8F(repo));
    rf.name = path_filename(repo);
    if (!cv.valid) return gh_resolve_api(a, cv, repo);

    String8 tag = { 0 };
    for (int hop = 0; hop < 2 && !tag.size; hop += 1) { // a renamed repository redirects once more
        HTTP_Redirect r = http_redirect_target(a, str8_pushf(a, "https://github.com/%.*s/releases/latest", S8F(repo)));
        if (!r.ok) return gh_resolve_api(a, cv, repo);
        if (r.status == 404) { rf.error = str8_pushf(a, "%.*s has no releases on GitHub (or the repository doesn't exist)", S8F(repo)); return rf; }
        if (r.status < 300 || r.status >= 400) return gh_resolve_api(a, cv, repo);
        String8 want = str8_pushf(a, "https://github.com/%.*s/releases/tag/", S8F(repo));
        String8 loc = r.location;
        if (str8_starts_with(loc, want, 1)) {
            tag = str8_skip(loc, want.size);
        } else if (str8_starts_with(loc, str8_lit("https://github.com/"), 1) && str8_ends_with(loc, str8_lit("/releases/latest"), 1)) {
            String8 moved = str8_chop(str8_skip(loc, 19), 16);
            if (str8_find(moved, 0, str8_lit("/"), 0) == moved.size) return gh_resolve_api(a, cv, repo);
            repo = moved;
        } else {
            return gh_resolve_api(a, cv, repo);
        }
    }
    // Tags with characters that would need care in a URL are rare; the API handles them.
    for (U64 i = 0; i < tag.size; i += 1) {
        U8 c = tag.str[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_' || c == '+')) return gh_resolve_api(a, cv, repo);
    }
    if (!tag.size) return gh_resolve_api(a, cv, repo);

    String8 base = str8_pushf(a, "https://github.com/%.*s/releases/download/%.*s/", S8F(repo), S8F(tag));
    HTTP_Result mr = http_get_url(a, str8_pushf(a, "%.*srelease.json", S8F(base)), str8(0, 0), 0, 0);
    if (!mr.ok) return gh_resolve_api(a, cv, repo); // no release.json: pick by zip names via the API
    String8 file = gh_pick_from_release_json(json_parse(a, mr.body), cv);
    if (!file.size) {
        rf.error = str8_pushf(a, "%.*s %.*s has no download for game version %u.%u.%u", S8F(repo), S8F(tag), cv.major, cv.minor, cv.patch);
        return rf;
    }
    for (U64 i = 0; i < file.size; i += 1) if (file.str[i] == '/' || file.str[i] == '\\' || file.str[i] == '?' || file.str[i] == '#') return gh_resolve_api(a, cv, repo);
    rf.website = str8_pushf(a, "https://github.com/%.*s", S8F(repo)); // the id stays the one the user installed
    rf.version = tag;
    rf.url = str8_pushf(a, "%.*s%.*s", S8F(base), S8F(url_encode(a, file)));
    rf.ok = 1;
    return rf;
}

//- Tukui (ElvUI, Tukui)
//
// api.tukui.org/v1/addons lists the addons Tukui hosts (a handful) with their version, the game
// patches they support, their folders and a download link. No key. The link carries a token that
// changes on every call, so it's fetched fresh before each download. One download serves every
// flavor (multi-.toc). An update is a different version (see mgr_has_update).

static SRWLOCK tukui_lock = SRWLOCK_INIT;
static U8 *tukui_cache;      // heap copy of the last list (search as you type asks often)
static U64 tukui_cache_size, tukui_cache_time;

static JSON *tukui_list(Arena *a, String8 *error)
{
    String8 body = { 0 };
    AcquireSRWLockShared(&tukui_lock);
    if (tukui_cache && GetTickCount64() - tukui_cache_time < 5 * 60 * 1000) body = str8_copy(a, str8(tukui_cache, tukui_cache_size));
    ReleaseSRWLockShared(&tukui_lock);
    if (!body.size) {
        HTTP_Result r = http_get_url(a, str8_lit("https://api.tukui.org/v1/addons"), str8_lit("Accept: application/json\r\n"), 0, 0);
        if (!r.ok) { if (error) *error = str8_pushf(a, "Tukui request failed: %.*s", S8F(r.error)); return &json_nil; }
        body = r.body;
        AcquireSRWLockExclusive(&tukui_lock);
        if (tukui_cache) HeapFree(GetProcessHeap(), 0, tukui_cache);
        tukui_cache = (U8 *)HeapAlloc(GetProcessHeap(), 0, body.size);
        if (tukui_cache) { memcpy(tukui_cache, body.str, body.size); tukui_cache_size = body.size; tukui_cache_time = GetTickCount64(); }
        ReleaseSRWLockExclusive(&tukui_lock);
    }
    JSON *list = json_parse(a, body);
    if (list->type != JSON_ARR && error) *error = str8_lit("unexpected Tukui response");
    return list;
}

// Built for this client? Same major version; Era (1.15) and Forever (1.60) are told apart by minor.
static B32 tukui_patch_ok(JSON *item, GameVersion cv)
{
    if (!cv.valid) return 1;
    json_each(p, json_get(item, "patch")) {
        U32 v[3]; parse_version3(json_str(p), v);
        if (v[0] != cv.major) continue;
        if (v[0] == 1 && (v[1] >= 60) != (cv.minor >= 60)) continue;
        return 1;
    }
    return 0;
}

static RemoteFile tukui_file_from_json(Arena *a, GameVersion cv, JSON *item)
{
    RemoteFile rf = { 0 };
    String8 slug = json_str(json_get(item, "slug"));
    rf.id = str8_pushf(a, "tukui:%.*s", S8F(slug));
    rf.name = json_str(json_get(item, "name"));
    rf.version = json_str(json_get(item, "version"));
    rf.website = json_str(json_get(item, "web_url"));
    rf.url = json_str(json_get(item, "url"));
    rf.date_days = iso_days(json_str(json_get(item, "last_update")));
    if (!tukui_patch_ok(item, cv)) {
        rf.error = str8_pushf(a, "%.*s has no version for game %u.%u.%u", S8F(rf.name), cv.major, cv.minor, cv.patch);
        return rf;
    }
    rf.ok = slug.size && rf.url.size && str8_starts_with(rf.url, str8_lit("https://"), 1);
    if (!rf.ok) rf.error = str8_pushf(a, "Tukui has no download for %.*s", S8F(slug));
    return rf;
}

static RemoteFile tukui_resolve(Arena *a, GameVersion cv, String8 slug)
{
    RemoteFile rf = { 0 };
    JSON *list = tukui_list(a, &rf.error);
    json_each(item, list) {
        if (str8_match(json_str(json_get(item, "slug")), slug, 1)) return tukui_file_from_json(a, cv, item);
    }
    if (!rf.error.size) rf.error = str8_pushf(a, "Tukui has no addon called '%.*s' (it hosts ElvUI and Tukui)", S8F(slug));
    return rf;
}

//- WoWInterface

static HTTP_Result wowi_details(Arena *a, S64 *ids, U64 count)
{
    Temp scratch = scratch_begin(a);
    String8List parts = { 0 };
    for (U64 i = 0; i < count; i += 1) str8_list_push(scratch.arena, &parts, str8_pushf(scratch.arena, "%lld", ids[i]));
    String8 url = str8_pushf(scratch.arena, "https://api.mmoui.com/v3/game/WOW/filedetails/%.*s.json",
                             S8F(str8_list_join(scratch.arena, &parts, str8_lit(","))));
    HTTP_Result r = http_get_url(a, url, str8_lit("Accept: application/json\r\n"), 0, 0);
    scratch_end(scratch);
    return r;
}

static RemoteFile wowi_file_from_json(Arena *a, JSON *item)
{
    RemoteFile rf = { 0 };
    S64 uid = json_int(json_get(item, "UID"));
    rf.id = str8_pushf(a, "wowi:%lld", uid);
    rf.file_id = json_int(json_get(item, "UIDate"));
    rf.name = json_str(json_get(item, "UIName"));
    rf.version = json_str(json_get(item, "UIVersion"));
    rf.url = json_str(json_get(item, "UIDownload"));
    rf.website = str8_pushf(a, "https://www.wowinterface.com/downloads/info%lld", uid);
    rf.ok = uid && rf.url.size;
    if (!rf.ok) rf.error = str8_pushf(a, "WoWInterface addon %lld has no download", uid);
    return rf;
}

static RemoteFile wowi_resolve(Arena *a, S64 id)
{
    HTTP_Result r = wowi_details(a, &id, 1);
    RemoteFile rf = { 0 };
    if (!r.ok) { rf.error = str8_pushf(a, "WoWInterface request failed: %.*s", S8F(r.error)); return rf; }
    JSON *list = json_parse(a, r.body);
    if (!list->first) { rf.error = str8_pushf(a, "WoWInterface addon %lld not found", id); return rf; }
    return wowi_file_from_json(a, list->first);
}

// The whole WoWInterface file list (about 8,000 addons, ~6 MB): WoWInterface has no search API, so
// wam searches this list locally. It names each addon's tested game versions.
static HTTP_Result wowi_filelist(Arena *a)
{
    return http_get_url(a, str8_lit("https://api.mmoui.com/v3/game/WOW/filelist.json"), str8_lit("Accept: application/json\r\n"), 0, 0);
}

// Tested on this client's game? Same major version (Era 1.15 and Forever 1.60 told apart by minor);
// on retail also the expansion before, as CurseForge files are picked (see mgr_pick_file).
static B32 wowi_compat_ok(JSON *item, GameVersion cv)
{
    if (!cv.valid) return 1;
    json_each(c, json_get(item, "UICompatibility")) {
        U32 v[3]; parse_version3(json_str(json_get(c, "version")), v);
        if (v[0] == 1 && cv.major == 1) { if ((v[1] >= 60) == (cv.minor >= 60)) return 1; continue; }
        if (v[0] == cv.major) return 1;
        if (cv.major >= 10 && v[0] >= 10 && v[0] + 1 == cv.major) return 1;
    }
    return 0;
}
