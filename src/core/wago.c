// wago.c - Wago Addons (addons.wago.io), with the user's own Wago API key.
//
// Wago's API needs a token. Wago subscribers get a personal API key; wam stores it
// encrypted for the current Windows user (DPAPI) in %APPDATA%\wam\wago.key, and the
// WAGO_API_KEY environment variable overrides it. Without a key, Wago is simply off.
//
//   wago:<id>      Wago addon id (the same id as the .toc's ## X-Wago-ID)
//   file_id        release time (unix seconds), which grows with each release
//
// Endpoints (https://addons.wago.io/api/external, "Authorization: Bearer <key>"):
//   POST /addons/_match    {game_version, addons:[{name, hash, wago?}]}  fingerprint lookup
//   POST /addons/_recents  {game_version, addons:[ids]}                  latest releases
//   GET  /addons/_search?query=&game_version=&stability=stable[&categories[]=N]
//        about 20 results, no paging; an empty query gives Wago's ~17 featured addons, or with
//        categories[] that category's most popular (ids 1..WAGO_CATEGORY_COUNT, named like CurseForge's)
//   GET  /addons/<id>?game_version=                                    one addon: description
//        (Markdown), gallery, website, the latest stable release and its changelog
// Download links are fetched with ?token=<key>. The API allows 60 requests a minute per key.

#define WAGO_HOST "addons.wago.io"
#define WAGO_API  "/api/external"

//- Key

static String8 wago_key_path(Arena *a)
{
    String8 appdata = os_get_env(a, "APPDATA");
    if (!appdata.size) return str8(0, 0);
    return path_join(a, path_join(a, appdata, str8_lit("wam")), str8_lit("wago.key"));
}

static B32 wago_key_looks_valid(String8 key)
{
    if (key.size <= 20 || key.size > 512) return 0;
    for (U64 i = 0; i < key.size; i += 1) if (key.str[i] <= ' ' || key.str[i] > '~') return 0;
    return 1;
}

typedef enum WagoKeySource { WAGO_KEY_NONE, WAGO_KEY_ENV, WAGO_KEY_SAVED } WagoKeySource;

// The env var if set, else the saved key, else empty.
static String8 wago_api_key_ex(Arena *a, WagoKeySource *source)
{
    WagoKeySource src = WAGO_KEY_NONE;
    String8 key = str8_trim(os_get_env(a, "WAGO_API_KEY"));
    if (key.size) src = WAGO_KEY_ENV;
    else {
        String8 path = wago_key_path(a);
        key = path.size ? str8_trim(os_unprotect(a, os_read_file(a, path))) : str8(0, 0);
        if (key.size) src = WAGO_KEY_SAVED;
    }
    if (source) *source = src;
    return key;
}

static String8 wago_api_key(Arena *a) { return wago_api_key_ex(a, 0); }

// Saves the key (encrypted for this Windows user); an empty key deletes it.
static B32 wago_save_key(String8 key)
{
    Temp scratch = scratch_begin(0);
    String8 path = wago_key_path(scratch.arena);
    B32 ok = 0;
    if (path.size) {
        if (!key.size) {
            ok = !os_file_exists(path) || DeleteFileW((WCHAR *)os_path16(scratch.arena, path).str);
        } else {
            String8 blob = os_protect(scratch.arena, key);
            ok = blob.size && os_make_dirs(path_dirname(path)) && os_write_file(path, blob);
        }
    }
    scratch_end(scratch);
    return ok;
}

//- API

static char *wago_game_version(GameVersion v)
{
    if (!v.valid || v.major >= 10) return "retail";
    if (v.major == 1) return v.minor >= 60 ? "forever" : "classic";
    if (v.major == 2) return "bc";
    if (v.major == 3) return "wotlk";
    if (v.major == 4) return "cata";
    if (v.major == 5) return "mop";
    return "retail";
}

static HTTP_Pause wago_pause; // Wago said 429 (its limit: 60 requests a minute per key)

static HTTP_Result wago_call(Arena *a, String8 key, char *method, String8 path, String8 body)
{
    U32 paused = http_pause_left(&wago_pause);
    if (paused) {
        HTTP_Result r = { 0 };
        r.status = 429;
        r.error = str8_pushf(a, "Wago's limit of 60 requests a minute is reached; wam waits %u s before asking again", paused);
        return r;
    }
    Temp scratch = scratch_begin(a);
    String8 headers = str8_pushf(scratch.arena, "Authorization: Bearer %.*s\r\nAccept: application/json\r\nContent-Type: application/json\r\n", S8F(key));
    HTTP_Result r = http_request_ex(a, method, str8_lit(WAGO_HOST), INTERNET_DEFAULT_HTTPS_PORT,
                                    str8_pushf(scratch.arena, WAGO_API "%.*s", S8F(path)), headers, body, 0, 0);
    if (r.status == 401 || r.status == 403) r.error = str8_lit("Wago rejected the API key (wrong key, or no active Wago subscription)");
    else if (r.status == 429) {
        http_pause_start(&wago_pause, r.retry_after);
        r.error = str8_pushf(a, "Wago's limit of 60 requests a minute is reached; wam waits %u s before asking again", http_pause_left(&wago_pause));
    }
    scratch_end(scratch);
    return r;
}

static HTTP_Result wago_recents(Arena *a, String8 key, GameVersion cv, String8 *ids, U64 count)
{
    Temp scratch = scratch_begin(a);
    String8List parts = { 0 };
    for (U64 i = 0; i < count; i += 1) str8_list_push(scratch.arena, &parts, str8_pushf(scratch.arena, "\"%.*s\"", S8F(json_escape(scratch.arena, ids[i]))));
    String8 body = str8_pushf(scratch.arena, "{\"game_version\":\"%s\",\"addons\":[%.*s]}", wago_game_version(cv),
                              S8F(str8_list_join(scratch.arena, &parts, str8_lit(","))));
    HTTP_Result r = wago_call(a, key, "POST", str8_lit("/addons/_recents"), body);
    scratch_end(scratch);
    return r;
}

// Wago has no browse API; its catalog comes from the featured list (category 0) and each category's
// most popular addons, one search per category.
#define WAGO_CATEGORY_COUNT 29

static HTTP_Result wago_browse(Arena *a, String8 key, GameVersion cv, U32 category)
{
    Temp scratch = scratch_begin(a);
    String8 path = str8_pushf(scratch.arena, "/addons/_search?query=&game_version=%s&stability=stable", wago_game_version(cv));
    if (category) path = str8_pushf(scratch.arena, "%.*s&categories%%5B%%5D=%u", S8F(path), category);
    HTTP_Result r = wago_call(a, key, "GET", path, str8(0, 0));
    scratch_end(scratch);
    return r;
}

// One addon, for the detail panel.
static HTTP_Result wago_addon(Arena *a, String8 key, GameVersion cv, String8 id)
{
    Temp scratch = scratch_begin(a);
    String8 path = str8_pushf(scratch.arena, "/addons/%.*s?game_version=%s", S8F(url_encode(scratch.arena, id)), wago_game_version(cv));
    HTTP_Result r = wago_call(a, key, "GET", path, str8(0, 0));
    scratch_end(scratch);
    return r;
}

static HTTP_Result wago_search(Arena *a, String8 key, GameVersion cv, String8 query)
{
    Temp scratch = scratch_begin(a);
    String8 path = str8_pushf(scratch.arena, "/addons/_search?query=%.*s&game_version=%s&stability=stable",
                              S8F(url_encode(scratch.arena, query)), wago_game_version(cv));
    HTTP_Result r = wago_call(a, key, "GET", path, str8(0, 0));
    scratch_end(scratch);
    return r;
}

// "2024-05-01T12:34:56Z" / "2024-05-01 12:34:56" -> unix seconds (0 if unparseable).
static S64 iso_seconds(String8 s)
{
    S64 days = iso_days(s);
    if (!days) return 0;
    S64 secs = days * 86400;
    if (s.size >= 19 && (s.str[10] == 'T' || s.str[10] == ' ') && s.str[13] == ':' && s.str[16] == ':') {
        secs += str8_to_s64(str8_substr(s, 11, 13)) * 3600 + str8_to_s64(str8_substr(s, 14, 16)) * 60 + str8_to_s64(str8_substr(s, 17, 19));
    }
    return secs;
}

static String8 wago_download_url(Arena *a, String8 link, String8 key)
{
    if (!link.size) return link;
    B32 has_query = str8_find(link, 0, str8_lit("?"), 0) < link.size;
    return str8_pushf(a, "%.*s%stoken=%.*s", S8F(link), has_query ? "&" : "?", S8F(url_encode(a, key)));
}

// The stable release from a recent_releases / releases object (wam follows the stable channel).
static JSON *wago_stable_release(JSON *releases)
{
    JSON *r = json_get(releases, "stable");
    return r->type == JSON_OBJ ? r : 0;
}

// A RemoteFile from one /_recents or /_match addon object.
static RemoteFile wago_file_from_scan(Arena *a, String8 key, JSON *item)
{
    RemoteFile rf = { 0 };
    String8 id = json_str(json_get(item, "id"));
    if (!id.size) id = item->key; // _recents is keyed by id
    rf.id = str8_pushf(a, "wago:%.*s", S8F(id));
    rf.name = json_str(json_get(item, "name"));
    rf.website = json_str(json_get(item, "website_url"));
    if (!rf.website.size) rf.website = str8_lit("https://addons.wago.io");
    JSON *rel = wago_stable_release(json_get(item, "recent_releases"));
    if (!rel) {
        rf.error = str8_pushf(a, "%.*s has no stable release on Wago for this game version", S8F(rf.name.size ? rf.name : id));
        rf.error_url = rf.website;
        return rf;
    }
    rf.version = json_str(json_get(rel, "label"));
    rf.file_id = iso_seconds(json_str(json_get(rel, "created_at")));
    rf.date_days = iso_days(json_str(json_get(rel, "created_at")));
    String8 link = json_str(json_get(rel, "link"));
    if (!link.size) link = json_str(json_get(rel, "download_link"));
    rf.url = wago_download_url(a, link, key);
    rf.ok = id.size && rf.url.size && rf.file_id;
    if (!rf.ok) rf.error = str8_pushf(a, "unexpected Wago response for %.*s", S8F(id));
    return rf;
}

static RemoteFile wago_resolve(Arena *a, String8 key, GameVersion cv, String8 id)
{
    RemoteFile rf = { 0 };
    HTTP_Result r = wago_recents(a, key, cv, &id, 1);
    if (!r.ok) { rf.error = str8_pushf(a, "Wago request failed: %.*s", S8F(r.error)); return rf; }
    JSON *addons = json_get(json_parse(a, r.body), "addons");
    json_each(item, addons) {
        if (item->type == JSON_OBJ && (str8_match(json_str(json_get(item, "id")), id, 0) || str8_match(item->key, id, 0))) return wago_file_from_scan(a, key, item);
    }
    rf.error = str8_pushf(a, "Wago addon %.*s not found (or not available for this game version)", S8F(id));
    rf.error_url = str8_lit("https://addons.wago.io");
    return rf;
}

// addons.wago.io/addons/<slug> -> addon id, via search (the website uses slugs, the API ids).
static String8 wago_id_from_slug(Arena *a, String8 key, GameVersion cv, String8 slug, String8 *error)
{
    HTTP_Result r = wago_search(a, key, cv, slug);
    if (!r.ok) { *error = str8_pushf(a, "Wago search failed: %.*s", S8F(r.error)); return str8(0, 0); }
    String8 tail = str8_pushf(a, "/addons/%.*s", S8F(slug));
    json_each(item, json_get(json_parse(a, r.body), "data")) {
        String8 url = json_str(json_get(item, "website_url"));
        while (url.size && url.str[url.size - 1] == '/') url.size -= 1;
        if (str8_ends_with(url, tail, 1)) return json_str(json_get(item, "id"));
    }
    *error = str8_pushf(a, "Wago addon '%.*s' not found for this game version", S8F(slug));
    return str8(0, 0);
}
