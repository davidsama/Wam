// curseforge.c - CurseForge API calls.
//
// The official API (https://docs.curseforge.com) needs an API key in the
// `x-api-key` header. Release builds embed the app's key (see tools\gen_key.ps1). The user's own
// key, saved in Settings, is used instead when there is one, and the CURSEFORGE_API_KEY environment
// variable overrides both.
//
// Files whose `downloadUrl` is null (the author disabled third-party
// distribution) are never fetched; the user is pointed to the website instead.

#include "cf_key.h"

#define CF_HOST     "api.curseforge.com"
#define CF_CDN_HOST "edge.forgecdn.net" // where downloadUrl points; it takes the key too
#define CF_GAME_WOW 1
#define CF_KEY_FILE "curseforge.key"

static HTTP_Pause cf_pause;  // CurseForge said 429

typedef enum CfKeySource { CF_KEY_NONE, CF_KEY_BUILTIN, CF_KEY_SAVED, CF_KEY_ENV } CfKeySource;
static B32 cf_key_env; // CURSEFORGE_API_KEY is set (read once, at startup)

// The key embedded at build time; empty without one.
static String8 cf_builtin_key(Arena *a)
{
    U8 *key = push_array_nz(a, U8, wam_cf_key_len + 1);
    for (U32 i = 0; i < wam_cf_key_len; i += 1) key[i] = wam_cf_key_obf[i] ^ (U8)((0x5A + i * 7) & 0xFF);
    key[wam_cf_key_len] = 0;
    return str8(key, wam_cf_key_len);
}

// The env var if set, else the saved key, else the embedded one, else empty.
static String8 cf_api_key(Arena *a, CfKeySource *source)
{
    *source = CF_KEY_ENV;
    String8 key = str8_trim(os_get_env(a, "CURSEFORGE_API_KEY"));
    cf_key_env = key.size != 0;
    if (!key.size) { *source = CF_KEY_SAVED; key = key_load(a, CF_KEY_FILE); }
    if (!key.size) { *source = CF_KEY_BUILTIN; key = cf_builtin_key(a); }
    if (!key.size) *source = CF_KEY_NONE;
    return key;
}

// What to tell the user when CurseForge refuses this key.
static String8 cf_rejected_error(String8 api_key)
{
    Temp scratch = scratch_begin(0);
    B32 builtin = str8_match(api_key, cf_builtin_key(scratch.arena), 0);
    scratch_end(scratch);
    return builtin ? str8_lit("CurseForge rejected the API key built into this wam. A newer wam may come with a new one; "
                              "meanwhile, you can use your own CurseForge API key (add it in Settings)")
         : cf_key_env ? str8_lit("CurseForge rejected your API key (CURSEFORGE_API_KEY)")
         : str8_lit("CurseForge rejected your API key (replace or remove it in Settings)");
}

static HTTP_Result cf_request(Arena *a, String8 api_key, char *method, String8 path, String8 body)
{
    Temp scratch = scratch_begin(a);
    String8 headers = str8_pushf(scratch.arena, "x-api-key: %.*s\r\nAccept: application/json\r\nContent-Type: application/json\r\n", S8F(api_key));
    HTTP_Result r = http_request_ex(a, method, str8_lit(CF_HOST), INTERNET_DEFAULT_HTTPS_PORT, path, headers, body, 0, 0);
    scratch_end(scratch);
    return r;
}

// Whether CurseForge takes a key the user is about to save: one small request. Not held up by a
// pause, which was for the key used so far.
static HTTP_Result cf_check_key(Arena *a, String8 api_key)
{
    return cf_request(a, api_key, "GET", str8_lit("/v1/games/1"), str8(0, 0));
}

static HTTP_Result cf_call(Arena *a, String8 api_key, char *method, String8 path, String8 body)
{
    U32 paused = http_pause_left(&cf_pause);
    if (paused) {
        HTTP_Result r = { 0 };
        r.status = 429;
        r.error = str8_pushf(a, "CurseForge asked wam to slow down; it waits %u s before asking again", paused);
        return r;
    }
    HTTP_Result r = cf_request(a, api_key, method, path, body);
    if (r.status == 403 || r.status == 401) {
        r.error = cf_rejected_error(api_key);
    } else if (r.status == 429) {
        http_pause_start(&cf_pause, r.retry_after);
        r.error = str8_pushf(a, "CurseForge asked wam to slow down; it waits %u s before asking again", http_pause_left(&cf_pause));
    }
    return r;
}

//- Downloads
//
// CurseForge's CDN takes the key on downloads (announced to become required, with 401 without
// one). It answers with a redirect to another host, so redirects are followed by hand: the key
// goes only to CF_CDN_HOST, never along to where it points.

static B32 cf_cdn_wants_key(String8 url)
{
    Temp scratch = scratch_begin(0);
    String16 url16 = os_str16_from_8(scratch.arena, url_escape_unsafe(scratch.arena, url));
    URL_COMPONENTSW uc = { sizeof(uc) };
    uc.dwHostNameLength = (DWORD)-1;
    B32 yes = 0;
    if (url16.size && WinHttpCrackUrl((WCHAR *)url16.str, (DWORD)url16.size, 0, &uc) && uc.nScheme == INTERNET_SCHEME_HTTPS && uc.nPort == INTERNET_DEFAULT_HTTPS_PORT) {
        yes = str8_match(os_str8_from_16(scratch.arena, (U16 *)uc.lpszHostName, uc.dwHostNameLength), str8_lit(CF_CDN_HOST), 1);
    }
    scratch_end(scratch);
    return yes;
}

// A redirect's Location from CF_CDN_HOST as a full https URL; empty if it isn't one.
static String8 cf_redirect_url(Arena *a, String8 location)
{
    if (str8_starts_with(location, str8_lit("https://"), 1)) return location;
    if (str8_starts_with(location, str8_lit("/"), 0) && !str8_starts_with(location, str8_lit("//"), 0)) {
        return str8_pushf(a, "https://" CF_CDN_HOST "%.*s", S8F(location));
    }
    return str8(0, 0);
}

// Downloads a file. A URL that isn't on CF_CDN_HOST (any other source's) is fetched without the key.
static HTTP_Result cf_download(Arena *a, String8 api_key, String8 url, HTTP_ProgressFunc *progress, void *user)
{
    HTTP_Result r = { 0 };
    for (int hop = 0;; hop += 1) {
        if (!api_key.size || !cf_cdn_wants_key(url)) return http_get_url(a, url, str8(0, 0), progress, user);
        String8 location = { 0 };
        r = http_get_url_ex(a, url, str8_pushf(a, "x-api-key: %.*s\r\n", S8F(api_key)), progress, user, &location);
        if (r.status == 401) r.error = cf_rejected_error(api_key);
        if (!location.size) return r;
        url = cf_redirect_url(a, location);
        if (!url.size || hop == 5) { r.error = str8_lit("bad redirect"); return r; }
    }
}

static String8 s64_json_array(Arena *a, char *key, S64 *values, U64 count)
{
    U8 *buf = push_array_nz(a, U8, count * 21 + 64);
    U64 n = (U64)sprintf((char *)buf, "{\"%s\":[", key);
    for (U64 i = 0; i < count; i += 1) n += (U64)sprintf((char *)buf + n, "%s%lld", i ? "," : "", values[i]);
    n += (U64)sprintf((char *)buf + n, "]}");
    return str8(buf, n);
}

// POST /v1/fingerprints/{gameId}
static HTTP_Result cf_match_fingerprints(Arena *a, String8 api_key, S64 *fingerprints, U64 count)
{
    Temp scratch = scratch_begin(a);
    String8 body = s64_json_array(scratch.arena, "fingerprints", fingerprints, count);
    HTTP_Result r = cf_call(a, api_key, "POST", str8_pushf(scratch.arena, "/v1/fingerprints/%d", CF_GAME_WOW), body);
    scratch_end(scratch);
    return r;
}

// POST /v1/mods  {"modIds":[...]}
static HTTP_Result cf_get_mods(Arena *a, String8 api_key, S64 *mod_ids, U64 count)
{
    Temp scratch = scratch_begin(a);
    String8 body = s64_json_array(scratch.arena, "modIds", mod_ids, count);
    HTTP_Result r = cf_call(a, api_key, "POST", str8_lit("/v1/mods"), body);
    scratch_end(scratch);
    return r;
}

static HTTP_Result cf_get_mod(Arena *a, String8 api_key, S64 mod_id)
{
    Temp scratch = scratch_begin(a);
    HTTP_Result r = cf_call(a, api_key, "GET", str8_pushf(scratch.arena, "/v1/mods/%lld", mod_id), str8(0, 0));
    scratch_end(scratch);
    return r;
}

static HTTP_Result cf_get_file(Arena *a, String8 api_key, S64 mod_id, S64 file_id)
{
    Temp scratch = scratch_begin(a);
    HTTP_Result r = cf_call(a, api_key, "GET", str8_pushf(scratch.arena, "/v1/mods/%lld/files/%lld", mod_id, file_id), str8(0, 0));
    scratch_end(scratch);
    return r;
}

// GET /v1/mods/search sort fields (ModsSearchSortField).
#define CF_SORT_POPULARITY 2
#define CF_SORT_UPDATED    3
#define CF_SORT_DOWNLOADS  6
#define CF_SORT_RELEASED   11
#define CF_PAGE_SIZE       50  // the API's maximum
#define CF_INDEX_MAX       (10000 - CF_PAGE_SIZE) // index + pageSize may not pass 10000

// GET /v1/mods/search: one page of results, newest / biggest first. An empty term lists everything
// (browsing). Pass a slug to look up one project by its URL name, categories ("1006,1039", ten at most) to list only theirs.
// The term matches addons with any of its words, so a common one ("or") matches thousands.
static HTTP_Result cf_search_page(Arena *a, String8 api_key, String8 term, String8 slug, String8 categories, int sort_field, U64 index)
{
    Temp scratch = scratch_begin(a);
    String8 filter = slug.size ? str8_pushf(scratch.arena, "&slug=%.*s", S8F(url_encode(scratch.arena, slug)))
                   : term.size ? str8_pushf(scratch.arena, "&searchFilter=%.*s", S8F(url_encode(scratch.arena, term)))
                   : str8(0, 0);
    if (categories.size) filter = str8_pushf(scratch.arena, "%.*s&categoryIds=%%5B%.*s%%5D", S8F(filter), S8F(categories));
    String8 path = str8_pushf(scratch.arena, "/v1/mods/search?gameId=%d%.*s&sortField=%d&sortOrder=desc&pageSize=%d&index=%llu",
                              CF_GAME_WOW, S8F(filter), sort_field, CF_PAGE_SIZE, Min(index, (U64)CF_INDEX_MAX));
    HTTP_Result r = cf_call(a, api_key, "GET", path, str8(0, 0));
    scratch_end(scratch);
    return r;
}

static HTTP_Result cf_search(Arena *a, String8 api_key, String8 term, String8 slug)
{
    return cf_search_page(a, api_key, term, slug, str8(0, 0), CF_SORT_POPULARITY, 0);
}
