// main_test.c - wamtest.exe: offline unit tests (JSON, file picking, zip safety).
// Build with `build.bat test`.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <winhttp.h>

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

static int failures, checks;

#define CHECK(cond) do { checks += 1; if (!(cond)) { failures += 1; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void test_readable(Arena *a)
{
    CHECK(str8_match(str8_count_short(a, 999), str8_lit("999"), 0));
    CHECK(str8_match(str8_count_short(a, 12345), str8_lit("12K"), 0));
    CHECK(str8_match(str8_count_short(a, 1234567), str8_lit("1.2M"), 0));
    CHECK(str8_match(str8_thousands(a, 0), str8_lit("0"), 0));
    CHECK(str8_match(str8_thousands(a, 999), str8_lit("999"), 0));
    CHECK(str8_match(str8_thousands(a, 12345), str8_lit("12,345"), 0));
    CHECK(str8_match(str8_thousands(a, 1234567), str8_lit("1,234,567"), 0));
    CHECK(str8_match(str8_thousands(a, -1234), str8_lit("-1,234"), 0));
    CHECK(str8_match(str8_ago(a, 30), str8_lit("just now"), 0));
    CHECK(str8_match(str8_ago(a, 5 * 60), str8_lit("5 min ago"), 0));
    CHECK(str8_match(str8_ago(a, 3 * 3600), str8_lit("3 h ago"), 0));
    CHECK(str8_match(str8_ago(a, 86400 + 10), str8_lit("yesterday"), 0));
    CHECK(str8_match(str8_ago(a, 10 * 86400), str8_lit("10 days ago"), 0));
    CHECK(str8_match(str8_ago(a, 90 * 86400), str8_lit("3 months ago"), 0));
    CHECK(str8_match(str8_ago(a, 800 * 86400), str8_lit("2 years ago"), 0));
}

static void test_json(Arena *a)
{
    String8 text = str8_lit("{\"a\": 1, \"s\": \"x\\\"y\\\\z\\n\\u00e9\\ud83d\\ude00\", \"arr\": [1, 2.5, -3, true, false, null],"
                            " \"o\": {\"k\": \"v\"}, \"empty\": [], \"eo\": {}}");
    JSON *root = json_parse(a, text);
    CHECK(root->type == JSON_OBJ);
    CHECK(json_int(json_get(root, "a")) == 1);
    CHECK(str8_match(json_str(json_get(root, "s")), str8_lit("x\"y\\z\n\xC3\xA9\xF0\x9F\x98\x80"), 0));
    CHECK(json_get(root, "arr")->count == 6);
    CHECK(json_get(root, "arr")->first->next->num == 2.5);
    CHECK(json_int(json_get(root, "arr")->first->next->next) == -3);
    CHECK(str8_match(json_str(json_get(json_get(root, "o"), "k")), str8_lit("v"), 0));
    CHECK(json_get(root, "missing") == &json_nil);
    CHECK(json_get(json_get(root, "missing"), "deeper") == &json_nil);
    CHECK(json_get(root, "empty")->count == 0 && json_get(root, "eo")->type == JSON_OBJ);
    CHECK(json_parse(a, str8_lit("{\"a\": ")) == &json_nil);
    CHECK(json_parse(a, str8_lit("[1,]")) == &json_nil);
    String8 esc = json_escape(a, str8_lit("a\"b\\c\n\x01"));
    CHECK(str8_match(esc, str8_lit("a\\\"b\\\\c\\n\\u0001"), 0));
    JSON *round = json_parse(a, str8_pushf(a, "\"%.*s\"", S8F(esc)));
    CHECK(str8_match(json_str(round), str8_lit("a\"b\\c\n\x01"), 0));
}

static void test_pick(Arena *a)
{
    String8 mod_text = str8_lit(
        "{\"id\": 1, \"name\": \"Test\", \"latestFiles\": [], \"latestFilesIndexes\": ["
        "{\"gameVersion\": \"12.0.5\", \"fileId\": 500, \"releaseType\": 1},"
        "{\"gameVersion\": \"12.1.0\", \"fileId\": 520, \"releaseType\": 2},"
        "{\"gameVersion\": \"11.2.7\", \"fileId\": 450, \"releaseType\": 1},"
        "{\"gameVersion\": \"5.5.3\",  \"fileId\": 480, \"releaseType\": 1},"
        "{\"gameVersion\": \"2.5.4\",  \"fileId\": 470, \"releaseType\": 1},"
        "{\"gameVersion\": \"1.15.8\", \"fileId\": 460, \"releaseType\": 1},"
        "{\"gameVersion\": \"1.15.9\", \"fileId\": 461, \"releaseType\": 3}]}");
    JSON *mod = json_parse(a, mod_text);
    Mgr m = { 0 };
    m.arena = a;

    m.client = (GameVersion){ 12, 1, 5, 1 }; CHECK(mgr_pick_file(&m, mod) == 500);  // release beats newer beta
    m.client = (GameVersion){ 5, 5, 4, 1 };  CHECK(mgr_pick_file(&m, mod) == 480);
    m.client = (GameVersion){ 2, 5, 6, 1 };  CHECK(mgr_pick_file(&m, mod) == 470);
    m.client = (GameVersion){ 1, 15, 9, 1 }; CHECK(mgr_pick_file(&m, mod) == 460);  // release beats alpha
    m.client = (GameVersion){ 1, 60, 1, 1 }; CHECK(mgr_pick_file(&m, mod) == 460);  // Forever: same major, no minor match
    m.client = (GameVersion){ 3, 4, 0, 1 };  CHECK(mgr_pick_file(&m, mod) == 0);    // no Wrath file
    m.client = (GameVersion){ 13, 0, 0, 1 }; CHECK(mgr_pick_file(&m, mod) == 500);  // retail falls back to 12.x

    JSON *beta_only = json_parse(a, str8_lit("{\"latestFilesIndexes\": [{\"gameVersion\": \"12.1.5\", \"fileId\": 9, \"releaseType\": 2}]}"));
    m.client = (GameVersion){ 12, 1, 5, 1 }; CHECK(mgr_pick_file(&m, beta_only) == 9);

    JSON *files_only = json_parse(a, str8_lit("{\"latestFiles\": [{\"id\": 77, \"releaseType\": 1, \"gameVersions\": [\"5.5.4\"]}]}"));
    m.client = (GameVersion){ 5, 5, 4, 1 }; CHECK(mgr_pick_file(&m, files_only) == 77);

    // Retail falls back to an older retail major, never to a Classic file (Questie on retail).
    JSON *classic_only = json_parse(a, str8_lit("{\"latestFilesIndexes\": [{\"gameVersion\": \"5.5.3\", \"fileId\": 31, \"releaseType\": 1},"
                                                "{\"gameVersion\": \"1.15.8\", \"fileId\": 30, \"releaseType\": 1}]}"));
    m.client = (GameVersion){ 12, 1, 5, 1 }; CHECK(mgr_pick_file(&m, classic_only) == 0);
}

static void test_catalog(Arena *a)
{
    // WoWInterface compatibility: same major; Era and Forever apart; retail also the expansion before.
    JSON *item = json_parse(a, str8_lit("{\"UID\":\"5108\",\"UIName\":\"Clique\",\"UIAuthorName\":\"Cladhaire\",\"UIDownloadTotal\":\"3000000\","
                                        "\"UIDate\":1794369870000,\"UICompatibility\":[{\"version\":\"11.2.7\"},{\"version\":\"1.15.8\"}]}"));
    CHECK(wowi_compat_ok(item, (GameVersion){ 12, 1, 5, 1 }));
    CHECK(wowi_compat_ok(item, (GameVersion){ 1, 15, 9, 1 }));
    CHECK(!wowi_compat_ok(item, (GameVersion){ 1, 60, 1, 1 }));
    CHECK(!wowi_compat_ok(item, (GameVersion){ 5, 5, 4, 1 }));
    CHECK(!wowi_compat_ok(json_parse(a, str8_lit("{\"UICompatibility\":null}")), (GameVersion){ 12, 1, 5, 1 }));
    Mgr m = { 0 };
    m.arena = a;
    m.client = (GameVersion){ 12, 1, 5, 1 };
    SearchResult w = { 0 };
    wowi_search_result(&m, item, &w);
    CHECK(str8_match(w.id, str8_lit("wowi:5108"), 0) && w.downloads == 3000000 && w.updated == 1794369870 && w.available);

    // Site-only CurseForge addons: with a GitHub source link wam can try its releases; without, only the page.
    char *mods[3] = { "{\"id\":1,\"name\":\"A\",\"allowModDistribution\":true,\"links\":{\"sourceUrl\":\"\"}}",
                      "{\"id\":2,\"name\":\"B\",\"allowModDistribution\":false,\"links\":{\"sourceUrl\":\"https://github.com/o/b\"}}",
                      "{\"id\":3,\"name\":\"C\",\"allowModDistribution\":false,\"links\":{\"sourceUrl\":\"https://gitlab.com/o/c\"}}" };
    U8 want_site[3] = { 0, SITE_ONLY_GITHUB, SITE_ONLY_PAGE };
    for (int i = 0; i < 3; i += 1) {
        SearchResult cr = { 0 };
        cf_search_result(&m, json_parse(a, str8_cstring(mods[i])), &cr);
        CHECK(cr.site_only == want_site[i]);
    }

    // Dedup by id; a later sighting refreshes what it knows and keeps what it doesn't.
    Catalog c = { 0 };
    cat_init(&c, arena_alloc(GB(1)));
    SearchResult r1 = { str8_lit("cf:61284"), "CurseForge", str8_lit("Details! Damage Meter"), str8_lit("Complete combat\tanalysis"), str8_lit("Terciob"),
                        str8_lit("https://www.curseforge.com/wow/addons/details"), 376000000, 1794300000, 1300000000, 1, SITE_ONLY_GITHUB,
                        cat_bit_for_cf(1014) | cat_bit_for_cf(1019), cat_subbit_for_cf(1032) | cat_subbit_for_cf(1243), str8_lit("https://media.forgecdn.net/x.png"),
                        str8_lit("https://a/t1.png|https://a/f1.png https://a/t2.png") };
    SearchResult r2 = { str8_lit("cf:61284"), "CurseForge", str8_lit("Details! Damage Meter"), { 0 }, { 0 }, { 0 }, 377000000, 0, 0, 1, SITE_ONLY_GITHUB };
    CHECK(cat_put(&c, &r1) && cat_put(&c, &w) && cat_put(&c, &r2));
    CHECK(c.count == 2);
    SearchResult *d = cat_find(&c, str8_lit("cf:61284"));
    CHECK(d && d->downloads == 377000000 && d->author.size && d->updated == 1794300000);
    CHECK(!cat_find(&c, str8_lit("cf:1")));

    // The disk cache round-trips, and is ignored for another game version.
    String8 blob = cat_serialize(a, &c, m.client, 1800000000);
    Catalog c2 = { 0 };
    cat_init(&c2, arena_alloc(GB(1)));
    S64 saved = 0;
    CHECK(cat_parse(&c2, blob, m.client, &saved) && saved == 1800000000 && c2.count == 2);
    SearchResult *d2 = cat_find(&c2, str8_lit("cf:61284"));
    CHECK(d2 && d2->source && !strcmp(d2->source, "CurseForge") && str8_match(d2->summary, str8_lit("Complete combat analysis"), 0) && d2->created == 1300000000 && d2->available);
    CHECK(d2 && d2->site_only == SITE_ONLY_GITHUB && d2->cats == (cat_bit_for_cf(1014) | cat_bit_for_cf(1019)) && d2->subcats == (cat_subbit_for_cf(1032) | cat_subbit_for_cf(1243)) && str8_match(d2->icon, r1.icon, 0) && str8_match(d2->shots, r1.shots, 0));
    SearchResult *w2 = cat_find(&c2, str8_lit("wowi:5108"));
    CHECK(w2 && str8_match(w2->source ? str8_cstring(w2->source) : str8(0, 0), str8_lit("WoWInterface"), 0) && !w2->summary.size);
    Catalog c3 = { 0 };
    cat_init(&c3, arena_alloc(GB(1)));
    CHECK(!cat_parse(&c3, blob, (GameVersion){ 12, 1, 7, 1 }, 0) && !c3.count);
    CHECK(!cat_parse(&c3, str8_lit("not a catalog\n"), m.client, 0));
    CHECK(!cat_parse(&c3, str8_lit("wam-catalog\t2\t1800000000\t12.1.5\n"), m.client, 0)); // an older format
    // Whether it was fetched with Wago's lists round-trips too (a catalog without them is stale once there's a key).
    CHECK(!c2.has_wago);
    c2.has_wago = 1;
    Catalog c4 = { 0 };
    cat_init(&c4, arena_alloc(GB(1)));
    CHECK(cat_parse(&c4, cat_serialize(a, &c2, m.client, 1800000000), m.client, 0) && c4.has_wago && c4.count == 2);

    // A Wago search result: picture, categories (CurseForge's names; Transmog is Roleplay > Transmogrification), release.
    JSON *wj = json_parse(a, str8_lit("{\"id\":\"vEGPaRN1\",\"display_name\":\"Details!|Skada Covenants\",\"summary\":\"Covenant icons.\","
                                     "\"thumbnail_image\":\"https://cdn.wago.io/thumbnails/K i.png\",\"categories\":[{\"id\":19,\"display_name\":\"Combat\"},"
                                     "{\"id\":29,\"display_name\":\"Transmog\"},{\"id\":99,\"display_name\":\"Something New\"}],"
                                     "\"releases\":{\"stable\":{\"label\":\"1.2\",\"created_at\":\"2022-06-02T08:37:29.000000Z\"}},\"owner\":\"Xagtu\",\"authors\":[],"
                                     "\"download_count\":87414,\"website_url\":\"https://addons.wago.io/addons/details-covenants\"}"));
    SearchResult ws = { 0 };
    wago_search_result(&m, wj, &ws);
    CHECK(str8_match(ws.id, str8_lit("wago:vEGPaRN1"), 0) && ws.source && !strcmp(ws.source, "Wago") && ws.downloads == 87414 && ws.available && ws.updated == 1654159049);
    CHECK(str8_match(ws.icon, str8_lit("https://cdn.wago.io/thumbnails/K%20i.png"), 0) && str8_match(ws.author, str8_lit("Xagtu"), 0));
    CHECK(ws.cats == (cat_bit_for_cf(1019) | cat_bit_for_cf(1060)) && ws.subcats == cat_subbit_for_cf(1171));
    JSON *wj2 = json_parse(a, str8_lit("{\"id\":\"x\",\"display_name\":\"X\",\"releases\":{}}"));
    SearchResult ws2 = { 0 };
    wago_search_result(&m, wj2, &ws2);
    CHECK(!ws2.available && !ws2.icon.size && !ws2.cats);

    // Rate limits: after a 429 an API isn't asked again until its pause is over (Retry-After, else a
    // minute, kept within 5 s .. 5 min). Calls during the pause fail at once, without the network.
    HTTP_Pause hp = { 0 };
    CHECK(http_pause_left(&hp) == 0);
    http_pause_start(&hp, 0);
    CHECK(http_pause_left(&hp) >= 59 && http_pause_left(&hp) <= 60);
    http_pause_start(&hp, 2);                                  // a shorter pause doesn't cut a longer one
    CHECK(http_pause_left(&hp) >= 59);
    HTTP_Pause hp2 = { 0 };
    http_pause_start(&hp2, 1);                                 // at least 5 s
    CHECK(http_pause_left(&hp2) >= 4 && http_pause_left(&hp2) <= 5);
    HTTP_Pause hp3 = { 0 };
    http_pause_start(&hp3, 100000);                            // at most 5 min
    CHECK(http_pause_left(&hp3) >= 299 && http_pause_left(&hp3) <= 300);
    cf_pause = hp; wago_pause = hp;
    U64 tp = os_now_us();
    HTTP_Result rc = cf_call(a, str8_lit("k"), "GET", str8_lit("/v1/mods/1"), str8(0, 0));
    HTTP_Result rw = wago_call(a, str8_lit("k"), "GET", str8_lit("/addons/x"), str8(0, 0));
    CHECK(os_now_us() - tp < 50000);                            // no network
    CHECK(!rc.ok && rc.status == 429 && str8_find(rc.error, 0, str8_lit("CurseForge asked wam to slow down"), 0) < rc.error.size);
    CHECK(!rw.ok && rw.status == 429 && str8_find(rw.error, 0, str8_lit("60 requests a minute"), 0) < rw.error.size);
    cf_pause.until_ms = 0; wago_pause.until_ms = 0;

    // Categories: CurseForge subcategories count as their parent; WoWInterface ids map onto them.
    JSON *cats = json_parse(a, str8_lit("[{\"id\":1014,\"parentCategoryId\":1},{\"id\":1037,\"parentCategoryId\":1016},{\"id\":4675,\"parentCategoryId\":1}]"));
    U64 subs = 0;
    CHECK(cat_cf_bits(cats, &subs) == (cat_bit_for_cf(1014) | cat_bit_for_cf(1016)) && subs == cat_subbit_for_cf(1037));
    CHECK(cat_wowi_bits(21, &subs) == cat_bit_for_cf(1016) && subs == 0);
    CHECK(cat_wowi_bits(57, &subs) == cat_bit_for_cf(1020) && subs == cat_subbit_for_cf(1024)); // Hunter
    CHECK(cat_wowi_bits(99999, &subs) == 0 && subs == 0);
    CHECK(CAT_CATEGORY_COUNT <= 32 && CAT_SUBCATEGORY_COUNT <= 64);
    for (U32 i = 0; i < CAT_SUBCATEGORY_COUNT; i += 1) CHECK(cat_sub_parent(i) >= 0);
    CHECK(w.cats == 0 && !w.icon.size); // this sample has no category or thumbnails

    // Picture links with spaces (CurseForge keeps uploaders' file names) are encoded, so the
    // space-separated shots list keeps each link whole; lists saved before that are mended.
    CHECK(str8_match(url_escape_unsafe(a, str8_lit("https://x/a b|c\"d.png")), str8_lit("https://x/a%20b%7Cc%22d.png"), 0));
    CHECK(str8_match(url_escape_unsafe(a, str8_lit("https://x/a%20b.png?w=1&h=2")), str8_lit("https://x/a%20b.png?w=1&h=2"), 0));
    CHECK(str8_match(url_escape_unsafe(a, str8_lit("https://x/\xC3\xA5.png")), str8_lit("https://x/%C3%A5.png"), 0));
    JSON *spaced = json_parse(a, str8_lit("{\"id\":1,\"logo\":{\"thumbnailUrl\":\"https://m/l o.png\"},\"screenshots\":["
                                          "{\"thumbnailUrl\":\"https://m/t/MSBT 5.2.9 In Action.png\",\"url\":\"https://m/MSBT 5.2.9 In Action.png\"},"
                                          "{\"thumbnailUrl\":\"https://m/t/b.gif\",\"url\":\"https://m/b.gif\"}]}"));
    SearchResult sp = { 0 };
    cf_search_result(&m, spaced, &sp);
    CHECK(str8_match(sp.icon, str8_lit("https://m/l%20o.png"), 0));
    CHECK(str8_match(sp.shots, str8_lit("https://m/t/MSBT%205.2.9%20In%20Action.png|https://m/MSBT%205.2.9%20In%20Action.png https://m/t/b.gif|https://m/b.gif"), 0));
    CHECK(str8_match(shots_repair(a, str8_lit("https://m/t/MSBT 5.2.9 In Action.png|https://m/MSBT 5.2.9 In Action.png https://m/t/b.gif|https://m/b.gif")), sp.shots, 0));
    CHECK(str8_match(shots_repair(a, sp.shots), sp.shots, 0)); // whole lists stay as they are
}

static SearchResult *twin_put(Catalog *c, char *id, char *name, char *author, B32 available, U8 site_only)
{
    SearchResult r = { 0 };
    r.id = str8_cstring(id);
    r.source = id[0] == 'c' ? "CurseForge" : id[0] == 'w' && id[1] == 'a' ? "Wago" : "WoWInterface";
    r.name = str8_cstring(name);
    r.author = str8_cstring(author);
    r.available = available;
    r.site_only = site_only;
    r.downloads = 1000;
    return cat_put(c, &r);
}

static B32 twins(Catalog *c, char *x, char *y)
{
    SearchResult *a = cat_find(c, str8_cstring(x)), *b = cat_find(c, str8_cstring(y));
    return a && b && cat_twin(c, a) == b && cat_twin(c, b) == a;
}

static B32 single(Catalog *c, char *x) { return !cat_twin(c, cat_find(c, str8_cstring(x))); }

// One addon on CurseForge and Wago: which entries pair up, and where Install gets it from.
static void test_twins(Arena *a)
{
    Catalog c = { 0 };
    cat_init(&c, arena_alloc(GB(1)));
    // The same name, whatever the punctuation or the authors' user names.
    twin_put(&c, "cf:61284", "Details! Damage Meter", "Terciob", 1, SITE_ONLY_GITHUB);
    twin_put(&c, "wago:qv63A7Gb", "Details! Damage Meter", "Terciob", 1, 0);
    twin_put(&c, "cf:100547", "Plater Nameplates", "Terciob", 1, 0);
    twin_put(&c, "wago:kRNLep6o", "Plater-Nameplates", "Continuity", 1, 0);
    // Its plugins aren't it.
    twin_put(&c, "cf:2", "Details! Damage Meter: Chart Viewer (plugin)", "Terciob", 1, 0);
    twin_put(&c, "wago:2", "Details! Damage Meter: Time Line", "Terciob", 1, 0);
    // A shared author and one name's words all in the other's; the closest match wins.
    twin_put(&c, "cf:3358", "DBM - Deadly Boss Mods (DBM-Core)", "MysticalOS", 1, 0);
    twin_put(&c, "cf:61882", "DBM - PvP Mods (Requires Deadly Boss Mods) - DBM", "MysticalOS", 1, 0);
    twin_put(&c, "cf:29493", "Deadly Boss Mods (DBM) - Archaeology", "MysticalOS", 1, 0); // further than DBM-Core ("core" is filler)
    twin_put(&c, "wago:qv633o6b", "Deadly Boss Mods (DBM)", "Stanzilla, MysticalOS, QartemisT", 1, 0);
    twin_put(&c, "wago:Xb6Xe0Kp", "Deadly Boss Mods (DBM) - PvP Mods", "MysticalOS", 1, 0);
    twin_put(&c, "cf:306489", "OmniCD - Party Cooldown Tracker", "Treebonker", 1, 0);
    twin_put(&c, "wago:E6gLXAK1", "OmniCD", "Treebonker", 1, 0);
    twin_put(&c, "cf:5", "TSM Thing Helper", "Sapu94", 1, 0); // the same author as "Sapu"
    twin_put(&c, "wago:5", "TSM Thing", "Sapu", 1, 0);
    // Not with another author, nor without a shared one.
    twin_put(&c, "cf:6", "TomTom", "jnwhiteh", 1, 0);
    twin_put(&c, "wago:6", "TomTom Group Arrow", "datagutten", 1, 0);
    twin_put(&c, "cf:7", "Abc", "Abcd", 1, 0);
    twin_put(&c, "wago:7", "Abc Extra", "Xyz", 1, 0);
    // Sound packs, skins, ':' / '_' continuations: separate addons, even alone.
    twin_put(&c, "cf:83086", "Vocal Raid Assistant", "Nitrak", 1, 0);
    twin_put(&c, "wago:8a", "Vocal Raid Assistant - Sara Sound Pack [en-EN]", "Nitrak", 1, 0);
    twin_put(&c, "wago:8b", "Vocal Raid Assistant - Ana Sound Pack [en-EN]", "Nitrak", 1, 0);
    twin_put(&c, "cf:9", "Masque", "StormFX", 1, 0);
    twin_put(&c, "wago:9", "Masque: Caith", "StormFX", 1, 0);
    twin_put(&c, "cf:10", "Mounts", "Gello", 1, 0);
    twin_put(&c, "wago:10", "Mounts ElvUI Skin", "Gello", 1, 0);
    twin_put(&c, "cf:11", "BigWigs", "Funkeh", 1, 0);
    twin_put(&c, "wago:11", "BigWigs_Classic", "Funkeh", 1, 0);
    // Two candidates as close as each other: neither.
    twin_put(&c, "cf:12a", "Quest Helper Retail", "Kim", 1, 0);
    twin_put(&c, "cf:12b", "Quest Helper Classic", "Kim", 1, 0);
    twin_put(&c, "wago:12", "Quest Helper", "Kim", 1, 0);
    // Only CurseForge and Wago pair up; an installed addon's .toc ids pair whatever the names.
    twin_put(&c, "wowi:13", "Details! Damage Meter", "Terciob", 1, 0);
    twin_put(&c, "cf:14", "Some Name", "A", 0, 0);
    twin_put(&c, "wago:14", "Another Name", "B", 1, 0);

    CatPair known[] = { { str8_lit("wago:14"), str8_lit("cf:14") }, { str8_lit("cf:1"), str8_lit("wago:missing") } };
    U64 linked = cat_link(&c, known, ArrayCount(known));
    CHECK(twins(&c, "cf:61284", "wago:qv63A7Gb") && twins(&c, "cf:100547", "wago:kRNLep6o"));
    CHECK(single(&c, "cf:2") && single(&c, "wago:2"));
    CHECK(twins(&c, "cf:3358", "wago:qv633o6b") && twins(&c, "cf:61882", "wago:Xb6Xe0Kp"));
    CHECK(twins(&c, "cf:306489", "wago:E6gLXAK1") && twins(&c, "cf:5", "wago:5"));
    CHECK(single(&c, "cf:6") && single(&c, "wago:6") && single(&c, "cf:7") && single(&c, "wago:7"));
    CHECK(single(&c, "cf:83086") && single(&c, "wago:8a") && single(&c, "wago:8b"));
    CHECK(single(&c, "cf:9") && single(&c, "wago:9") && single(&c, "cf:10") && single(&c, "wago:10") && single(&c, "cf:11") && single(&c, "wago:11"));
    CHECK(single(&c, "cf:12a") && single(&c, "cf:12b") && single(&c, "wago:12"));
    CHECK(single(&c, "wowi:13") && twins(&c, "cf:14", "wago:14"));
    CHECK(linked == 7 && c.twin_count == 7);

    // Install: CurseForge, but Wago (with a key) when CurseForge's author allows only its website;
    // whichever has a file for this game version.
    SearchResult *details_cf = cat_find(&c, str8_lit("cf:61284")), *details_wago = cat_find(&c, str8_lit("wago:qv63A7Gb"));
    CHECK(cat_install_pick(&c, details_cf, 1) == details_wago && cat_install_pick(&c, details_wago, 1) == details_wago);
    CHECK(cat_install_pick(&c, details_wago, 0) == details_cf);
    SearchResult *plater = cat_find(&c, str8_lit("wago:kRNLep6o"));
    CHECK(cat_install_pick(&c, plater, 1) == cat_find(&c, str8_lit("cf:100547")));
    CHECK(cat_install_pick(&c, cat_find(&c, str8_lit("cf:14")), 1) == cat_find(&c, str8_lit("wago:14"))); // CurseForge has no file
    SearchResult *alone = cat_find(&c, str8_lit("cf:9"));
    CHECK(cat_install_pick(&c, alone, 1) == alone);

    // Linking again after the catalog grew: pairs found anew, nothing stale.
    twin_put(&c, "wago:15", "Masque", "StormFX", 1, 0);
    CHECK(cat_link(&c, 0, 0) == 7 && twins(&c, "cf:9", "wago:15") && single(&c, "cf:14")); // without the .toc pair this time

    // Wago lists its authors, then the owner if not among them (pairing looks for a shared one).
    Mgr m = { 0 };
    m.arena = a;
    SearchResult ws = { 0 };
    wago_search_result(&m, json_parse(a, str8_lit("{\"id\":\"q\",\"display_name\":\"DBM\",\"owner\":\"QartemisT\",\"authors\":[\"Stanzilla\",\"MysticalOS\"]}")), &ws);
    CHECK(str8_match(ws.author, str8_lit("Stanzilla, MysticalOS, QartemisT"), 0));
    wago_search_result(&m, json_parse(a, str8_lit("{\"id\":\"q\",\"display_name\":\"DBM\",\"owner\":\"mysticalos\",\"authors\":[\"Stanzilla\",\"MysticalOS\"]}")), &ws);
    CHECK(str8_match(ws.author, str8_lit("Stanzilla, MysticalOS"), 0));
}

static void test_markup(Arena *a)
{
    String8 html = markup_to_text(a, str8_lit("<h2>Hi&nbsp;there</h2>\n<p>One   <b>bold</b>\nline &amp; more</p><ul><li>first</li><li>second &#8211; x</li></ul>"
                                              "<script>alert(1)</script><p><img src=\"x.png\"><a href=\"https://x\">link</a></p>"), MARKUP_HTML);
    CHECK(str8_match(html, str8_lit("Hi there\n\nOne bold line & more\n\n\xE2\x80\xA2 first\n\xE2\x80\xA2 second \xE2\x80\x93 x\n\nlink"), 0));
    String8 md = markup_to_text(a, str8_lit("## Version 1.2\n\n- **Fixed** a [bug](https://x)\n* another\n\nDone `now`"), MARKUP_MARKDOWN);
    CHECK(str8_match(md, str8_lit("Version 1.2\n\n\xE2\x80\xA2 Fixed a bug\n\xE2\x80\xA2 another\n\nDone now"), 0));
    // Images, and images that are links, are dropped (Wago descriptions open with banners and donate buttons).
    String8 mdi = markup_to_text(a, str8_lit("![](https://i/x.png)\n\nText ![logo](https://i/y.png) here\n\n[![](https://i/p.png)](https://patreon) [![Pay](https://i/q.png)](https://pay?a=(b)\n\nEnd"), MARKUP_MARKDOWN);
    CHECK(str8_match(mdi, str8_lit("Text  here\n\nEnd"), 0));
    String8 bb = markup_to_text(a, str8_lit("[b]Clique[/b] binds [url=https://x]spells[/url].\n[list]\n[*]one\n[*]two\n[/list]\n[img]https://x/y.png[/img]"), MARKUP_BBCODE);
    CHECK(str8_match(bb, str8_lit("Clique binds spells.\n\xE2\x80\xA2 one\n\xE2\x80\xA2 two"), 0));
}

static void test_specs(Arena *a)
{
#define SPEC(in, out) CHECK(str8_match(mgr_parse_spec(a, str8_lit(in)), str8_lit(out), 0))
    SPEC("61284", "cf:61284");
    SPEC("cf:61284", "cf:61284");
    SPEC("BigWigsMods/BigWigs", "gh:BigWigsMods/BigWigs");
    SPEC("gh:BigWigsMods/BigWigs", "gh:BigWigsMods/BigWigs");
    SPEC("https://github.com/BigWigsMods/BigWigs", "gh:BigWigsMods/BigWigs");
    SPEC("https://github.com/BigWigsMods/BigWigs/releases/tag/v1", "gh:BigWigsMods/BigWigs");
    SPEC("github.com/Owner/Repo.git/", "gh:Owner/Repo");
    SPEC("https://www.wowinterface.com/downloads/info5108-Clique.html", "wowi:5108");
    SPEC("https://www.wowinterface.com/downloads/fileinfo.php?id=5108", "wowi:5108");
    SPEC("wowi:5108", "wowi:5108");
    SPEC("https://www.curseforge.com/wow/addons/details", "cfslug:details");
    SPEC("details", "");
    SPEC("deadly boss mods", "");
    SPEC("a/b/c", "");
    SPEC("wago:aNDmy96o", "wago:aNDmy96o");
    SPEC("https://addons.wago.io/addons/details", "wagoslug:details");
    SPEC("https://addons.wago.io/addons/DBM-Core/versions?stability=stable", "wagoslug:DBM-Core");
    SPEC("wago:a/b", "");
    SPEC("tukui:ElvUI", "tukui:elvui");
    SPEC("https://tukui.org/elvui", "tukui:elvui");
    SPEC("https://www.tukui.org/download.php?ui=tukui", "tukui:tukui");
    SPEC("https://api.tukui.org/v1/download/elvui/TOKEN", "tukui:elvui");
    SPEC("github.com/tukui-org/ElvUI", "gh:tukui-org/ElvUI");
#undef SPEC
}

static void test_deps(void)
{
    // 0:A needs 1:B and 2:C; 3:D needs C; B needs 4:E; 5:F needs nothing.
    U32 a_req[] = { 1, 2 }, b_req[] = { 4 }, d_req[] = { 2 };
    MgrUnit v[6] = { 0 };
    v[0].req = a_req; v[0].req_count = 2;
    v[1].req = b_req; v[1].req_count = 1;
    v[3].req = d_req; v[3].req_count = 1;
    MgrUnits u = { v, 6 };

    U8 mark[6] = { 1, 0, 0, 0, 0, 0 };           // remove A
    mgr_mark_unused_deps(&u, mark);
    CHECK(mark[1] == 2 && mark[4] == 2);          // B, and E through B
    CHECK(mark[2] == 0);                          // C is still needed by D
    CHECK(mark[3] == 0 && mark[5] == 0);

    U8 mark2[6] = { 1, 0, 0, 1, 0, 0 };          // remove A and D: C becomes unused too
    mgr_mark_unused_deps(&u, mark2);
    CHECK(mark2[2] == 2);

    U8 mark3[6] = { 0, 0, 1, 0, 0, 0 };          // remove C alone: A and D break
    CHECK(mgr_unit_needs_marked(&u, mark3, 0) && mgr_unit_needs_marked(&u, mark3, 3));
    CHECK(!mgr_unit_needs_marked(&u, mark3, 5));
}

static void test_zip_names(void)
{
    CHECK(zip_name_is_safe(str8_lit("Addon/file.lua")));
    CHECK(zip_name_is_safe(str8_lit("Addon/sub/..x.lua")));
    CHECK(!zip_name_is_safe(str8_lit("../evil.lua")));
    CHECK(!zip_name_is_safe(str8_lit("Addon/../../evil.lua")));
    CHECK(!zip_name_is_safe(str8_lit("/abs.lua")));
    CHECK(!zip_name_is_safe(str8_lit("C:/abs.lua")));
    CHECK(!zip_name_is_safe(str8_lit("Addon\\..\\evil.lua")));
    CHECK(crc32(str8_lit("123456789")) == 0xCBF43926u);
    CHECK(iso_days(str8_lit("1970-01-01T00:00:00Z")) == 0);
    CHECK(iso_days(str8_lit("2000-03-01")) == 11017);
    CHECK(iso_days(str8_lit("2026-11-07T06:53:14.123Z")) - iso_days(str8_lit("2026-10-24T00:00:00Z")) == 14);
    CHECK(iso_days(str8_lit("garbage")) == 0);
    CHECK(archive_kind(str8_lit("PK\x03\x04rest")) == ARCHIVE_ZIP);
    CHECK(archive_kind(str8_lit("PK\x05\x06")) == ARCHIVE_ZIP);
    CHECK(archive_kind(str8_lit("Rar!\x1a\x07\x00x")) == ARCHIVE_RAR);
    CHECK(archive_kind(str8_lit("Rar!\x1a\x07\x01\x00")) == ARCHIVE_RAR);
    CHECK(archive_kind(str8_lit("7z\xbc\xaf\x27\x1c\x00\x04")) == ARCHIVE_7Z);
    CHECK(archive_kind(str8_lit("# README")) == ARCHIVE_UNKNOWN);
}

static void test_wago(Arena *a)
{
    char h[33];
    md5_hex(str8_lit(""), h);    CHECK(!strcmp(h, "d41d8cd98f00b204e9800998ecf8427e"));
    md5_hex(str8_lit("abc"), h); CHECK(!strcmp(h, "900150983cd24fb0d6963f7d28e17f72"));
    md5_hex(str8_lit("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"), h); CHECK(!strcmp(h, "8215ef0796a20bcaaae116d3876c664a"));
    md5_hex(str8_lit("12345678901234567890123456789012345678901234567890123456789012345678901234567890"), h);
    CHECK(!strcmp(h, "57edf4a22be3c955ac49da2e2107b67a"));

    CHECK(iso_seconds(str8_lit("1970-01-02T00:00:01Z")) == 86401);
    CHECK(iso_seconds(str8_lit("2024-05-01 12:00:00")) == 1714564800);
    CHECK(iso_seconds(str8_lit("garbage")) == 0);

    CHECK(!strcmp(wago_game_version((GameVersion){ 12, 0, 5, 1 }), "retail"));
    CHECK(!strcmp(wago_game_version((GameVersion){ 1, 60, 1, 1 }), "forever"));
    CHECK(!strcmp(wago_game_version((GameVersion){ 1, 15, 7, 1 }), "classic"));
    CHECK(!strcmp(wago_game_version((GameVersion){ 2, 5, 5, 1 }), "bc"));
    CHECK(!strcmp(wago_game_version((GameVersion){ 5, 5, 0, 1 }), "mop"));

    // A /_recents response: keyed by id, stable release picked, token appended to the link.
    JSON *root = json_parse(a, str8_lit(
        "{\"addons\":{\"abc123\":{\"name\":\"Details!\",\"website_url\":\"https://addons.wago.io/addons/details\","
        "\"recent_releases\":{\"beta\":{\"label\":\"b2\",\"created_at\":\"2024-06-01T00:00:00Z\",\"link\":\"https://x/b\"},"
        "\"stable\":{\"label\":\"v1.0\",\"created_at\":\"2024-05-01T12:00:00Z\",\"link\":\"https://x/dl?f=1\"}}},"
        "\"nostable\":{\"name\":\"N\",\"recent_releases\":{\"alpha\":{\"label\":\"a\",\"created_at\":\"2024-01-01T00:00:00Z\",\"link\":\"https://x/a\"}}}}}"));
    JSON *item = json_get(root, "addons")->first;
    RemoteFile rf = wago_file_from_scan(a, str8_lit("k+y"), item);
    CHECK(rf.ok && str8_match(rf.id, str8_lit("wago:abc123"), 0) && str8_match(rf.version, str8_lit("v1.0"), 0));
    CHECK(rf.file_id == 1714564800 && str8_match(rf.url, str8_lit("https://x/dl?f=1&token=k%2By"), 0));
    RemoteFile none = wago_file_from_scan(a, str8_lit("k"), item->next);
    CHECK(!none.ok && none.error.size);
    CHECK(wago_key_looks_valid(str8_lit("0123456789abcdefghijklmnop")) && !wago_key_looks_valid(str8_lit("short")));
    CHECK(!wago_key_looks_valid(str8_lit("0123456789abcdef ghijklmnop")));

    // The key file round-trips through DPAPI.
    String8 blob = os_protect(a, str8_lit("secret-key-value"));
    CHECK(blob.size && !str8_match(blob, str8_lit("secret-key-value"), 0));
    CHECK(str8_match(os_unprotect(a, blob), str8_lit("secret-key-value"), 0));
}

static char pin_status[512];
static void pin_status_cb(void *user, String8 msg) { (void)user; U64 n = Min(msg.size, sizeof(pin_status) - 1); memcpy(pin_status, msg.str, n); pin_status[n] = 0; }

static void test_pins(Arena *a)
{
    // A pinned addon stays pinned through wam.json, and updates leave it alone.
    WCHAR tmp16[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp16);
    String8 dir = path_join(a, os_str8_from_16(a, (U16 *)tmp16, wcslen(tmp16)), str8_pushf(a, "wampin-%llu", os_now_us()));
    String8 addons = path_join(a, dir, str8_lit("Interface\\AddOns"));
    CHECK(os_make_dirs(path_join(a, addons, str8_lit("Foo"))));
    Mgr *m = mgr_open(a, addons, str8(0, 0));
    MgrAddon *e = mgr_add(m);
    e->id = str8_lit("x:1"); e->name = str8_lit("Foo"); e->version = str8_lit("1.0");
    e->file_id = 1; e->latest_file_id = 2; e->latest_version = str8_lit("2.0");
    str8_list_push(a, &e->folders, str8_lit("Foo"));
    CHECK(mgr_save(m));
    String8 saved = os_read_file(a, m->state_path);
    CHECK(saved.size && str8_find(saved, 0, str8_lit("pinned"), 0) >= saved.size); // only written for pinned addons
    CHECK(mgr_set_pinned(m, str8_lit("x:1"), 1));
    m = mgr_open(a, addons, str8(0, 0));
    CHECK(m->count == 1 && m->addons[0].pinned && mgr_has_update(&m->addons[0]));
    m->status = pin_status_cb;
    // Update all (and naming it) skip it: no download is tried (x: isn't a real source, so one would fail).
    CHECK(mgr_update_list_ex(m, 0, 0));
    CHECK(strstr(pin_status, "1 pinned addon kept its version") != 0);
    String8List only = { 0 };
    str8_list_push(a, &only, str8_lit("x:1"));
    CHECK(mgr_update_list_ex(m, &only, 1));
    CHECK(strstr(pin_status, "Kept Foo as is: pinned") != 0);
    CHECK(mgr_set_pinned(m, str8_lit("x:1"), 0));
    m = mgr_open(a, addons, str8(0, 0));
    CHECK(m->count == 1 && !m->addons[0].pinned);
    CHECK(!mgr_set_pinned(m, str8_lit("x:2"), 1)); // not installed
    os_delete_tree(dir);

    // A changelog that covers several versions is cut where it names the installed one (whole versions only).
    String8 log = str8_lit("Version 2.0\n- a\nVersion 1.10\n- b\nVersion 1.1\n- c");
    CHECK(str8_match(changelog_until(log, str8_lit("1.1")), str8_lit("Version 2.0\n- a\nVersion 1.10\n- b"), 0));
    CHECK(str8_match(changelog_until(log, str8_lit("v1.10")), str8_lit("Version 2.0\n- a"), 0));
    CHECK(str8_match(changelog_until(log, str8_lit("3.0")), log, 0));                       // not mentioned: all of it
    CHECK(str8_match(changelog_until(str8_lit("1.1 notes\nmore"), str8_lit("1.1")), str8_lit("1.1 notes\nmore"), 0)); // never the first line

    // CurseForge HTML that wraps pasted Markdown gets the Markdown pass too; real HTML doesn't need it.
    String8 md = markup_cf_html(a, str8_lit("<p># Changelog</p><p>Now **in-game**, see [the notes](https://x.y/z).</p>"));
    CHECK(str8_starts_with(md, str8_lit("Changelog"), 0) && str8_find(md, 0, str8_lit("Now in-game, see the notes."), 0) < md.size);
    CHECK(str8_find(md, 0, str8_lit("#"), 0) >= md.size && str8_find(md, 0, str8_lit("**"), 0) >= md.size);
    CHECK(str8_match(markup_cf_html(a, str8_lit("<p>Fixed <b>bars</b>.</p>")), str8_lit("Fixed bars."), 0));
}

static void test_fingerprint_cache(Arena *a)
{
    // Fingerprints come from the cache while a folder's files are unchanged, and are the same.
    WCHAR tmp16[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp16);
    String8 dir = path_join(a, os_str8_from_16(a, (U16 *)tmp16, wcslen(tmp16)), str8_pushf(a, "wamfp-%llu", os_now_us()));
    String8 addons = path_join(a, dir, str8_lit("Interface\\AddOns"));
    CHECK(os_make_dirs(path_join(a, addons, str8_lit("Foo"))));
    CHECK(os_write_file(path_join(a, addons, str8_lit("Foo\\Foo.toc")), str8_lit("## Interface: 110000\n## Title: Foo\nFoo.lua\n")));
    CHECK(os_write_file(path_join(a, addons, str8_lit("Foo\\Foo.lua")), str8_lit("print('foo')\n")));
    WCHAR old_local[MAX_PATH] = { 0 };
    DWORD had = GetEnvironmentVariableW(L"LOCALAPPDATA", old_local, MAX_PATH);
    SetEnvironmentVariableW(L"LOCALAPPDATA", (WCHAR *)os_str16_from_8(a, dir).str);

    AddonArray plain = addons_scan(a, addons);
    addons_fingerprint(&plain);
    Mgr *m = mgr_open(a, addons, str8(0, 0));
    U64 cached = 99;
    AddonArray s1 = addons_scan(a, addons);
    mgr_fingerprint(m, &s1, &cached);
    CHECK(cached == 0 && s1.count == 1 && s1.v[0].fingerprint == plain.v[0].fingerprint && !strcmp(s1.v[0].wago_hash, plain.v[0].wago_hash));
    AddonArray s2 = addons_scan(a, addons);
    mgr_fingerprint(m, &s2, &cached);
    CHECK(cached == 1 && s2.v[0].fingerprint == plain.v[0].fingerprint && !strcmp(s2.v[0].wago_hash, plain.v[0].wago_hash) && s2.files_hashed == 0);
    CHECK(os_write_file(path_join(a, addons, str8_lit("Foo\\Foo.lua")), str8_lit("print('changed')\n")));
    AddonArray s3 = addons_scan(a, addons);
    mgr_fingerprint(m, &s3, &cached);
    CHECK(cached == 0 && s3.v[0].fingerprint != plain.v[0].fingerprint);

    SetEnvironmentVariableW(L"LOCALAPPDATA", had ? old_local : 0);
    os_delete_tree(dir);
}

static void test_keep_wago(Arena *a)
{
    // A refresh whose Wago part failed keeps last time's Wago entries (from the saved catalog).
    GameVersion cv = { 12, 1, 5, 1 };
    Catalog old = { 0 };
    cat_init(&old, arena_alloc(GB(1)));
    SearchResult w = { 0 }, c1 = { 0 }, c2 = { 0 };
    w.id = str8_lit("wago:abc"); w.name = str8_lit("Wago Thing"); w.source = "Wago"; w.available = 1;
    c1.id = str8_lit("cf:1"); c1.name = str8_lit("Old CF"); c1.source = "CurseForge"; c1.available = 1;
    cat_put(&old, &w); cat_put(&old, &c1);
    old.has_wago = 1;
    String8 file = cat_serialize(a, &old, cv, 1000);
    Catalog fresh = { 0 };
    cat_init(&fresh, arena_alloc(GB(1)));
    c2.id = str8_lit("cf:2"); c2.name = str8_lit("New CF"); c2.source = "CurseForge"; c2.available = 1;
    cat_put(&fresh, &c2);
    cat_keep_wago(&fresh, file, cv);
    CHECK(fresh.has_wago && cat_find(&fresh, str8_lit("wago:abc")) && cat_find(&fresh, str8_lit("cf:2")) && !cat_find(&fresh, str8_lit("cf:1")));
    arena_release(old.arena);
    arena_release(fresh.arena);
}

static void test_unpacked_hashes(Arena *a)
{
    // An installed zip's checksums from the unpacked data equal the ones read back from disk.
    WCHAR tmp16[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp16);
    String8 dir = path_join(a, os_str8_from_16(a, (U16 *)tmp16, wcslen(tmp16)), str8_pushf(a, "wamzip-%llu", os_now_us()));
    String8 src = path_join(a, dir, str8_lit("src")), addons = path_join(a, dir, str8_lit("Interface\\AddOns"));
    CHECK(os_make_dirs(path_join(a, src, str8_lit("Foo\\Sub"))) && os_make_dirs(addons));
    CHECK(os_write_file(path_join(a, src, str8_lit("Foo\\Foo.toc")), str8_lit("## Interface: 110000\nSub\\A.lua\n")));
    CHECK(os_write_file(path_join(a, src, str8_lit("Foo\\Sub\\A.lua")), str8_lit("print('a')\n")));
    String8 zip = path_join(a, dir, str8_lit("foo.zip"));
    String8 tar = path_join(a, os_system_dir(a), str8_lit("tar.exe"));
    OS_RunResult r = os_run(a, tar, str8_pushf(a, "\"%.*s\" -a -c -f \"%.*s\" -C \"%.*s\" Foo", S8F(tar), S8F(zip), S8F(src)), 60000);
    CHECK(r.started && r.exit_code == 0);
    Mgr *m = mgr_open(a, addons, str8(0, 0));
    String8List folders = { 0 };
    CHECK(mgr_install_archive(m, os_read_file(a, zip), &folders) && folders.count == 1);
    CHECK(m->unpacked.count == 2); // Foo.toc and Sub/A.lua, hashed while unpacking
    char sig1[33], hash1[33], sig2[33], hash2[33];
    folder_check_known(path_join(a, addons, str8_lit("Foo")), sig1, hash1, 1, &m->unpacked);
    folder_check(path_join(a, addons, str8_lit("Foo")), sig2, hash2, 1);
    CHECK(!strcmp(sig1, sig2) && !strcmp(hash1, hash2));
    os_delete_tree(dir);
}

static void test_checks(Arena *a)
{
    // A scratch folder under %TEMP%: content changes change the hash, timestamps alone don't.
    WCHAR tmp16[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp16);
    String8 dir = path_join(a, os_str8_from_16(a, (U16 *)tmp16, wcslen(tmp16)), str8_pushf(a, "wamtest-%llu", os_now_us()));
    CHECK(os_make_dirs(path_join(a, dir, str8_lit("Sub"))));
    CHECK(os_write_file(path_join(a, dir, str8_lit("A.lua")), str8_lit("print(1)")));
    CHECK(os_write_file(path_join(a, dir, str8_lit("Sub\\B.xml")), str8_lit("<Ui/>")));
    char sig1[33], hash1[33], sig2[33], hash2[33];
    folder_check(dir, sig1, hash1, 1);
    folder_check(dir, sig2, hash2, 1);
    CHECK(!strcmp(sig1, sig2) && !strcmp(hash1, hash2));
    CHECK(os_write_file(path_join(a, dir, str8_lit("A.lua")), str8_lit("print(2)"))); // same size, new contents
    folder_check(dir, sig2, hash2, 1);
    CHECK(strcmp(hash1, hash2) != 0);
    CHECK(os_write_file(path_join(a, dir, str8_lit("A.lua")), str8_lit("print(1)")));
    folder_check(dir, sig2, hash2, 1);
    CHECK(!strcmp(hash1, hash2)); // back to the original contents
    CHECK(os_write_file(path_join(a, dir, str8_lit("Sub\\C.lua")), str8_lit("")));   // an added file counts
    folder_check(dir, sig2, hash2, 1);
    CHECK(strcmp(hash1, hash2) != 0 && strcmp(sig1, sig2) != 0);

    // settings.json keeps each setting when another one is saved (APPDATA pointed at the scratch folder).
    WCHAR old_appdata[MAX_PATH] = { 0 };
    DWORD had = GetEnvironmentVariableW(L"APPDATA", old_appdata, MAX_PATH);
    SetEnvironmentVariableW(L"APPDATA", (WCHAR *)os_str16_from_8(a, dir).str);
    CHECK(wam_save_root(str8_lit("C:\\Games\\World of Warcraft")));
    CHECK(wam_save_detail_share(0.375));
    WamSettings st = wam_settings_load(a);
    CHECK(str8_match(st.wow_root, str8_lit("C:\\Games\\World of Warcraft"), 0) && st.detail_share == 0.375);
    CHECK(wam_save_root(str8_lit("D:\\WoW")));
    st = wam_settings_load(a);
    CHECK(str8_match(st.wow_root, str8_lit("D:\\WoW"), 0) && st.detail_share == 0.375);
    CHECK(wam_save_window((WamWindow){ -1200, 40, 1300, 800, 1 }));
    st = wam_settings_load(a);
    CHECK(st.window.x == -1200 && st.window.y == 40 && st.window.w == 1300 && st.window.h == 800 && st.window.maximized);
    CHECK(str8_match(st.wow_root, str8_lit("D:\\WoW"), 0) && st.detail_share == 0.375);
    CHECK(wam_save_theme(str8_lit("void")));
    st = wam_settings_load(a);
    CHECK(str8_match(st.theme, str8_lit("void"), 0) && st.window.w == 1300 && str8_match(st.wow_root, str8_lit("D:\\WoW"), 0));
    SetEnvironmentVariableW(L"APPDATA", had ? old_appdata : 0);
    os_delete_tree(dir);
}

static void test_fuzzy(void)
{
    FuzzyMatch f = fuzzy_match(str8_lit("dbm"), str8_lit("Deadly Boss Mods"));
    CHECK(f.score > 0 && f.count == 3 && f.pos[0] == 0 && f.pos[1] == 7 && f.pos[2] == 12); // word starts, not "d..b..m" anywhere
    CHECK(fuzzy_match(str8_lit("xyz"), str8_lit("Deadly Boss Mods")).score < 0);
    CHECK(fuzzy_match(str8_lit("modsdeadly"), str8_lit("Deadly Boss Mods")).score < 0); // order matters
    CHECK(fuzzy_match(str8_lit(""), str8_lit("Details")).score < 0);
    f = fuzzy_match(str8_lit("hnotes"), str8_lit("HandyNotes"));
    CHECK(f.score > 0 && f.pos[0] == 0 && f.pos[1] == 5); // camelCase hump
    // Contiguous and short beats scattered or long.
    S32 exact = fuzzy_match(str8_lit("details"), str8_lit("Details! Damage Meter")).score;
    S32 longer = fuzzy_match(str8_lit("details"), str8_lit("Details! Mythic+ Scoreboard for every dungeon run")).score;
    S32 scattered = fuzzy_match(str8_lit("details"), str8_lit("Deadly Experimental Tail Sails")).score;
    CHECK(exact > longer && exact > scattered && scattered >= 0);
    CHECK(fuzzy_match(str8_lit("DEADLY boss"), str8_lit("deadly boss mods")).score > 0); // case and spaces ignored
    CHECK(fuzzy_match(str8_lit("weak"), str8_lit("WeakAuras")).score > fuzzy_match(str8_lit("weak"), str8_lit("Tweaks for World Quests")).score);
}

static void test_tukui(Arena *a)
{
    JSON *item = json_parse(a, str8_lit("{\"slug\":\"elvui\",\"name\":\"ElvUI\",\"version\":\"16.01\",\"url\":\"https://api.tukui.org/v1/download/elvui/x\","
                                        "\"web_url\":\"https://tukui.org/elvui\",\"patch\":[\"12.1.5\",\"1.60.1\",\"5.5.4\",\"1.15.9\"],\"directories\":[\"ElvUI\"]}"));
    CHECK(tukui_patch_ok(item, (GameVersion){ 12, 0, 7, 1 }));  // Retail: same major
    CHECK(tukui_patch_ok(item, (GameVersion){ 1, 60, 1, 1 }));  // Forever
    CHECK(tukui_patch_ok(item, (GameVersion){ 1, 15, 8, 1 }));  // Era
    CHECK(!tukui_patch_ok(item, (GameVersion){ 3, 4, 3, 1 }));  // no Wrath build
    JSON *era_only = json_parse(a, str8_lit("{\"patch\":[\"1.15.7\"]}"));
    CHECK(!tukui_patch_ok(era_only, (GameVersion){ 1, 60, 1, 1 })); // Era build isn't a Forever build
    RemoteFile rf = tukui_file_from_json(a, (GameVersion){ 1, 60, 1, 1 }, item);
    CHECK(rf.ok && str8_match(rf.id, str8_lit("tukui:elvui"), 0) && str8_match(rf.version, str8_lit("16.01"), 0));

    // gh: and tukui: updates are a different version; others compare file ids.
    MgrAddon e = { 0 };
    e.id = str8_lit("tukui:elvui"); e.version = str8_lit("v16.01"); e.latest_version = str8_lit("16.01");
    CHECK(!mgr_has_update(&e));
    e.latest_version = str8_lit("16.02");
    CHECK(mgr_has_update(&e));
    e.id = str8_lit("gh:BigWigsMods/BigWigs"); e.version = str8_lit("v427.9"); e.latest_version = str8_lit("v427.9"); e.file_id = 5; e.latest_file_id = 0;
    CHECK(!mgr_has_update(&e));
    e.id = str8_lit("cf:123"); e.file_id = 5; e.latest_file_id = 6;
    CHECK(mgr_has_update(&e));
}

int main(void)
{
    Arena *a = arena_alloc(GB(1));
    test_json(a);
    test_readable(a);
    test_pick(a);
    test_specs(a);
    test_deps();
    test_wago(a);
    test_checks(a);
    test_pins(a);
    test_fingerprint_cache(a);
    test_unpacked_hashes(a);
    test_keep_wago(a);
    test_fuzzy();
    test_tukui(a);
    test_catalog(a);
    test_twins(a);
    test_markup(a);
    test_zip_names();
    printf("%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
