// curseforge.c - CurseForge API calls.
//
// The official API (https://docs.curseforge.com) needs an API key in the
// `x-api-key` header. Release builds embed the app's key (see tools\gen_key.ps1);
// the CURSEFORGE_API_KEY environment variable overrides it.
//
// Files whose `downloadUrl` is null (the author disabled third-party
// distribution) are never fetched; the user is pointed to the website instead.

#include "cf_key.h"

#define CF_HOST     "api.curseforge.com"
#define CF_GAME_WOW 1

static B32 cf_key_is_users; // the key came from CURSEFORGE_API_KEY, not this build
static HTTP_Pause cf_pause;  // CurseForge said 429

// The env var if set, else the key embedded at build time, else empty.
static String8 cf_api_key(Arena *a)
{
    String8 env = os_get_env(a, "CURSEFORGE_API_KEY");
    cf_key_is_users = env.size != 0;
    if (env.size) return env;
    U8 *key = push_array_nz(a, U8, wam_cf_key_len + 1);
    for (U32 i = 0; i < wam_cf_key_len; i += 1) key[i] = wam_cf_key_obf[i] ^ (U8)((0x5A + i * 7) & 0xFF);
    key[wam_cf_key_len] = 0;
    return str8(key, wam_cf_key_len);
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
    Temp scratch = scratch_begin(a);
    String8 headers = str8_pushf(scratch.arena, "x-api-key: %.*s\r\nAccept: application/json\r\nContent-Type: application/json\r\n", S8F(api_key));
    HTTP_Result r = http_request_ex(a, method, str8_lit(CF_HOST), INTERNET_DEFAULT_HTTPS_PORT, path, headers, body, 0, 0);
    if (r.status == 403 || r.status == 401) {
        r.error = cf_key_is_users ? str8_lit("CurseForge rejected your API key (CURSEFORGE_API_KEY)")
                : str8_lit("CurseForge rejected the API key built into this wam. A newer wam may come with a new one; "
                           "meanwhile, your own CurseForge API key works when set in CURSEFORGE_API_KEY");
    } else if (r.status == 429) {
        http_pause_start(&cf_pause, r.retry_after);
        r.error = str8_pushf(a, "CurseForge asked wam to slow down; it waits %u s before asking again", http_pause_left(&cf_pause));
    }
    scratch_end(scratch);
    return r;
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
