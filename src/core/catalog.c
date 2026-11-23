// catalog.c - the addon catalog that Get addons browses and searches locally.
//
// For each flavor, wam keeps several thousand addons on hand, so browsing, filtering, sorting and
// searching as you type never wait on the network:
//   CurseForge    the most downloaded, most recently updated and newest addons (paged searches,
//                 fetched in parallel; scrolling past the end fetches more)
//   WoWInterface  its whole file list (one request; it has no search API)
//   Tukui         its whole list (ElvUI, Tukui)
//   Wago          (with the user's key) its featured addons and each category's most popular (it
//                 has no browse API: one search per category); others join through searches
// Each entry says whether it has a file for this game version. The catalog is cached on disk per
// flavor (%LOCALAPPDATA%\wam\catalog\<flavor>.tsv; WAM_CATALOG_DIR overrides the folder), so Get
// addons shows it at once on the next start. It's refetched in the background when older than
// CAT_MAX_AGE or when the game version changed.

#define CAT_MAX           (1 << 15)
#define CAT_FORMAT        4
#define CAT_MAX_AGE       (12 * 3600)
#define CAT_PAGES_DOWNLOADS 20 // CurseForge pages prefetched per sort (CF_PAGE_SIZE each)
#define CAT_PAGES_UPDATED    6
#define CAT_PAGES_RELEASED   4

// Entries with a hash index on id. Strings live in the catalog's arena.
typedef struct Catalog {
    Arena *arena;
    SearchResult *v;
    U64 count;
    U32 *slots;      // 2 * CAT_MAX open-addressed slots: entry index + 1, 0 = empty
    U32 version;     // bumped on every change (the UI re-sorts when it moves)
    B32 has_wago;    // fetched with Wago's lists (without a Wago key it has only what searches found)
    U32 *twin;       // per entry: the same addon on its other source (index + 1, 0 = none; see cat_link)
    U64 twin_count;  // pairs linked
} Catalog;

static void cat_init(Catalog *c, Arena *a)
{
    U32 version = c->version;
    memset(c, 0, sizeof(*c));
    c->arena = a;
    c->v = push_array(a, SearchResult, CAT_MAX);
    c->slots = push_array(a, U32, CAT_MAX * 2);
    c->twin = push_array(a, U32, CAT_MAX);
    c->version = version + 1;
}

static U64 cat_hash(String8 s)
{
    U64 h = 14695981039346656037ull;
    for (U64 i = 0; i < s.size; i += 1) { h ^= s.str[i]; h *= 1099511628211ull; }
    return h;
}

static SearchResult *cat_find(Catalog *c, String8 id)
{
    if (!c->slots || !id.size) return 0;
    U64 mask = CAT_MAX * 2 - 1;
    for (U64 h = cat_hash(id) & mask; c->slots[h]; h = (h + 1) & mask) {
        SearchResult *e = &c->v[c->slots[h] - 1];
        if (str8_match(e->id, id, 0)) return e;
    }
    return 0;
}

// Adds r, or refreshes the entry with the same id (newer information wins; empty fields keep the old).
static SearchResult *cat_put(Catalog *c, SearchResult *r)
{
    if (!c->slots || !r->id.size || !r->name.size) return 0;
    Arena *a = c->arena;
    U64 mask = CAT_MAX * 2 - 1, h = cat_hash(r->id) & mask;
    for (; c->slots[h]; h = (h + 1) & mask) {
        SearchResult *e = &c->v[c->slots[h] - 1];
        if (!str8_match(e->id, r->id, 0)) continue;
        if (!str8_match(e->name, r->name, 0)) e->name = str8_copy(a, r->name);
        if (r->summary.size && !str8_match(e->summary, r->summary, 0)) e->summary = str8_copy(a, r->summary);
        if (r->author.size && !str8_match(e->author, r->author, 0)) e->author = str8_copy(a, r->author);
        if (r->website.size && !str8_match(e->website, r->website, 0)) e->website = str8_copy(a, r->website);
        if (r->icon.size && !str8_match(e->icon, r->icon, 0)) e->icon = str8_copy(a, r->icon);
        if (r->shots.size && !str8_match(e->shots, r->shots, 0)) e->shots = str8_copy(a, r->shots);
        if (r->cats) { e->cats = r->cats; e->subcats = r->subcats; }
        if (r->source) e->source = r->source;
        if (r->downloads) e->downloads = r->downloads;
        if (r->updated) e->updated = r->updated;
        if (r->created) e->created = r->created;
        e->available = r->available;
        if (r->source) e->site_only = r->site_only;
        c->version += 1;
        return e;
    }
    if (c->count >= CAT_MAX) return 0;
    SearchResult *e = &c->v[c->count];
    *e = *r;
    e->id = str8_copy(a, r->id);
    e->name = str8_copy(a, r->name);
    e->summary = str8_copy(a, r->summary);
    e->author = str8_copy(a, r->author);
    e->website = str8_copy(a, r->website);
    e->icon = str8_copy(a, r->icon);
    e->shots = str8_copy(a, r->shots);
    c->slots[h] = (U32)++c->count;
    c->version += 1;
    return e;
}

//- Twins: one addon published on both CurseForge and Wago
//
// Many addons (Details!, DBM, WeakAuras, BigWigs, Plater...) are on both sites, so the catalog has
// an entry from each; Get addons shows them as one row with both sources. Neither site names the
// other's id in what the catalog fetches (Wago's lists carry no source link, CurseForge's no Wago
// id), so cat_link pairs entries by name:
//   - the same name, ignoring case, spaces and punctuation ("Plater-Nameplates" = "Plater Nameplates"),
//     whoever the authors are (people often use another user name on each site: Sapu / Sapu94), or
//   - a shared author (ignoring case and punctuation; "Jordon" also matches "Jordons") and the words of
//     one name all in the other ("OmniCD" / "OmniCD - Party Cooldown Tracker", "Deadly Boss Mods (DBM) -
//     PvP Mods" / "DBM - PvP Mods (Requires Deadly Boss Mods) - DBM"), unless the longer name continues
//     the shorter like a separate addon does ("Masque: Caith", "BigWigs_Classic") or its extra words say
//     it's something for the addon (a skin, a sound pack, a plugin).
// A pair is kept only when each is the other's single best match: "Vocal Raid Assistant" isn't linked to
// any of its sound packs, nor "Details! Damage Meter" to its plugins. Ids an installed addon's .toc names
// (## X-Curse-Project-ID and ## X-Wago-ID) are the same addon for sure: they're linked first.

typedef struct CatPair { String8 a, b; } CatPair; // two ids known to be the same addon

#define TWIN_WORDS 24
typedef struct TwinKey {
    String8 name;            // lowercase letters and digits only
    U64 word[TWIN_WORDS];    // the name's distinct words, hashed (not filler words: twin_filler)
    U32 words;
    String8 author[4];       // lowercase letters and digits only
    U32 authors;
} TwinKey;

static B32 twin_char(U8 ch) { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch >= 0x80; }
static U8 twin_lower(U8 ch) { return ch >= 'A' && ch <= 'Z' ? ch + 32 : ch; }

static String8 twin_squash(Arena *a, String8 s)
{
    U8 *out = push_array_nz(a, U8, s.size + 1);
    U64 n = 0;
    for (U64 i = 0; i < s.size; i += 1) if (twin_char(s.str[i])) out[n++] = twin_lower(s.str[i]);
    return str8(out, n);
}

static U64 twin_hash(char *word)
{
    U64 h = 14695981039346656037ull;
    for (; *word; word += 1) { h ^= (U8)*word; h *= 1099511628211ull; }
    return h;
}

// Words that don't tell addons apart ("DBM-Core" is DBM; "... (Requires Deadly Boss Mods)").
static B32 twin_filler(U64 h)
{
    static char *words[] = { "core", "requires", "addon", "the", "and", "of", "for", "a", "an", "by", "with", "wow" };
    static U64 hashes[ArrayCount(words)];
    if (!hashes[0]) for (U32 i = 0; i < ArrayCount(words); i += 1) hashes[i] = twin_hash(words[i]);
    for (U32 i = 0; i < ArrayCount(words); i += 1) if (hashes[i] == h) return 1;
    return 0;
}

static void twin_key(Arena *a, SearchResult *e, TwinKey *k)
{
    memset(k, 0, sizeof(*k));
    k->name = twin_squash(a, e->name);
    for (U64 i = 0; i < e->name.size && k->words < TWIN_WORDS;) {
        if (!twin_char(e->name.str[i])) { i += 1; continue; }
        U64 h = 14695981039346656037ull;
        for (; i < e->name.size && twin_char(e->name.str[i]); i += 1) { h ^= twin_lower(e->name.str[i]); h *= 1099511628211ull; }
        B32 dup = twin_filler(h);
        for (U32 w = 0; w < k->words && !dup; w += 1) dup = k->word[w] == h;
        if (!dup) k->word[k->words++] = h;
    }
    for (String8 rest = e->author; rest.size && k->authors < ArrayCount(k->author);) { // Wago lists several, comma-separated
        U64 comma = str8_find(rest, 0, str8_lit(","), 0);
        String8 one = twin_squash(a, str8_prefix(rest, comma));
        rest = str8_skip(rest, comma + 1);
        if (one.size) k->author[k->authors++] = one;
    }
}

static B32 twin_same_author(TwinKey *x, TwinKey *y)
{
    for (U32 i = 0; i < x->authors; i += 1) {
        for (U32 j = 0; j < y->authors; j += 1) {
            String8 p = x->author[i], q = y->author[j];
            if (p.size > q.size) { String8 t = p; p = q; q = t; }
            if (str8_match(p, q, 0) || (p.size >= 4 && str8_starts_with(q, p, 0))) return 1;
        }
    }
    return 0;
}

static B32 twin_has_word(TwinKey *k, U64 h)
{
    for (U32 i = 0; i < k->words; i += 1) if (k->word[i] == h) return 1;
    return 0;
}

// How well two entries match: 0 = same name, more = more words apart; -1 = not the same addon.
static S32 twin_score(SearchResult *ex, TwinKey *x, SearchResult *ey, TwinKey *y)
{
    if (!x->name.size || !y->name.size) return -1;
    if (str8_match(x->name, y->name, 0)) return 0;
    if (!twin_same_author(x, y)) return -1;
    SearchResult *el = ey;
    if (x->words > y->words) { TwinKey *t = x; x = y; y = t; el = ex; } // x: the shorter name
    if (!x->words) return -1;
    for (U32 i = 0; i < x->words; i += 1) if (!twin_has_word(y, x->word[i])) return -1;
    // Its extra words say it's for the addon, not the addon itself.
    static char *addon_for[] = { "skin", "skins", "plugin", "plugins", "module", "modules", "theme", "themes",
                                 "sound", "sounds", "voice", "voices", "profile", "profiles", "media" };
    for (U32 i = 0; i < ArrayCount(addon_for); i += 1) {
        U64 h = twin_hash(addon_for[i]);
        if (twin_has_word(y, h) && !twin_has_word(x, h)) return -1;
    }
    // The longer name continues the shorter with ':' or '_': a separate addon ("Masque: Caith").
    if (str8_starts_with(y->name, x->name, 0)) {
        U64 seen = 0, i = 0;
        for (; i < el->name.size && seen < x->name.size; i += 1) seen += twin_char(el->name.str[i]);
        while (i < el->name.size && el->name.str[i] == ' ') i += 1;
        if (i < el->name.size && (el->name.str[i] == ':' || el->name.str[i] == '_')) return -1;
    }
    return 1 + (S32)(y->words - x->words);
}

static void twin_set(Catalog *c, U64 i, U64 j)
{
    c->twin[i] = (U32)j + 1;
    c->twin[j] = (U32)i + 1;
    c->twin_count += 1;
}

// Pairs CurseForge and Wago entries that are the same addon (see above). known: id pairs to link
// first, whatever their names. Returns how many pairs it linked.
static U64 cat_link(Catalog *c, CatPair *known, U64 known_count)
{
    if (!c->twin) return 0;
    memset(c->twin, 0, sizeof(U32) * c->count);
    c->twin_count = 0;
    for (U64 k = 0; k < known_count; k += 1) {
        SearchResult *x = cat_find(c, known[k].a), *y = cat_find(c, known[k].b);
        if (!x || !y) continue;
        U64 i = (U64)(x - c->v), j = (U64)(y - c->v);
        B32 pair = (id_is(x->id, "cf:") && id_is(y->id, "wago:")) || (id_is(x->id, "wago:") && id_is(y->id, "cf:"));
        if (pair && !c->twin[i] && !c->twin[j]) twin_set(c, i, j);
    }

    Temp scratch = scratch_begin(0);
    Arena *a = scratch.arena;
    U32 *cf = push_array(a, U32, c->count + 1), *wago = push_array(a, U32, c->count + 1);
    U64 ncf = 0, nwago = 0;
    for (U64 i = 0; i < c->count; i += 1) {
        if (c->twin[i]) continue;
        if (id_is(c->v[i].id, "cf:")) cf[ncf++] = (U32)i;
        else if (id_is(c->v[i].id, "wago:")) wago[nwago++] = (U32)i;
    }
    if (!ncf || !nwago) { scratch_end(scratch); return c->twin_count; }
    TwinKey *kcf = push_array(a, TwinKey, ncf), *kwago = push_array(a, TwinKey, nwago);
    for (U64 i = 0; i < ncf; i += 1) twin_key(a, &c->v[cf[i]], &kcf[i]);
    for (U64 i = 0; i < nwago; i += 1) twin_key(a, &c->v[wago[i]], &kwago[i]);

    // CurseForge's entries by name and by each author's first four letters (all a match can share),
    // so each Wago entry looks only at those.
    U64 slots = 1;
    while (slots < ncf * 8) slots <<= 1;
    U32 *by_name = push_array(a, U32, slots), *by_author = push_array(a, U32, slots); // list heads: node + 1
    typedef struct Node { U32 f, next; } Node;
    Node *nodes = push_array(a, Node, ncf * 5);
    U64 nnodes = 0;
    for (U64 f = 0; f < ncf; f += 1) {
        U64 h = cat_hash(kcf[f].name) & (slots - 1);
        nodes[nnodes] = (Node){ (U32)f, by_name[h] }; by_name[h] = (U32)++nnodes;
        for (U32 k = 0; k < kcf[f].authors; k += 1) {
            h = cat_hash(str8_prefix(kcf[f].author[k], 4)) & (slots - 1);
            nodes[nnodes] = (Node){ (U32)f, by_author[h] }; by_author[h] = (U32)++nnodes;
        }
    }

    // Every candidate pair, and each entry's best score and how many candidates share it.
    typedef struct Cand { U32 w, f; S32 score; } Cand;
    U64 cap = 1 << 16, n = 0;
    Cand *cand = push_array(a, Cand, cap);
    S32 *best_w = push_array(a, S32, nwago), *best_f = push_array(a, S32, ncf);
    U32 *ties_w = push_array(a, U32, nwago), *ties_f = push_array(a, U32, ncf), *seen = push_array(a, U32, ncf);
    for (U64 i = 0; i < nwago; i += 1) best_w[i] = 0x7fffffff;
    for (U64 i = 0; i < ncf; i += 1) best_f[i] = 0x7fffffff;
    for (U64 w = 0; w < nwago; w += 1) {
        for (U32 k = 0; k <= kwago[w].authors; k += 1) { // the name's list, then each author's
            U32 at = k == 0 ? by_name[cat_hash(kwago[w].name) & (slots - 1)] : by_author[cat_hash(str8_prefix(kwago[w].author[k - 1], 4)) & (slots - 1)];
            for (; at && n < cap; at = nodes[at - 1].next) {
                U32 f = nodes[at - 1].f;
                if (seen[f] == w + 1) continue;
                seen[f] = (U32)w + 1;
                S32 sc = twin_score(&c->v[wago[w]], &kwago[w], &c->v[cf[f]], &kcf[f]);
                if (sc < 0) continue;
                cand[n++] = (Cand){ (U32)w, f, sc };
                if (sc < best_w[w]) { best_w[w] = sc; ties_w[w] = 1; } else if (sc == best_w[w]) ties_w[w] += 1;
                if (sc < best_f[f]) { best_f[f] = sc; ties_f[f] = 1; } else if (sc == best_f[f]) ties_f[f] += 1;
            }
        }
    }
    for (U64 k = 0; k < n; k += 1) {
        Cand *p = &cand[k];
        if (p->score == best_w[p->w] && ties_w[p->w] == 1 && p->score == best_f[p->f] && ties_f[p->f] == 1) twin_set(c, wago[p->w], cf[p->f]);
    }
    scratch_end(scratch);
    return c->twin_count;
}

// The same addon on its other source, or 0.
static SearchResult *cat_twin(Catalog *c, SearchResult *e)
{
    if (!c->twin || !e || e < c->v || e >= c->v + c->count) return 0;
    U32 t = c->twin[e - c->v];
    return t && t - 1 < c->count ? &c->v[t - 1] : 0;
}

// Where Install gets an addon from by default: e, or its twin. CurseForge, unless its author lets only
// CurseForge's own site serve the files (site_only): then Wago, which has no such limit (as when sync
// recognizes installed addons, Wago comes first for those). Wago only with a key; whichever has a file
// for this game version beats one that doesn't.
static SearchResult *cat_install_pick(Catalog *c, SearchResult *e, B32 wago_key)
{
    SearchResult *t = cat_twin(c, e);
    if (!t) return e;
    SearchResult *cf = id_is(e->id, "cf:") ? e : t, *wago = cf == e ? t : e;
    if (!wago_key) return cf;
    if (cf->available != wago->available) return cf->available ? cf : wago;
    return cf->site_only ? wago : cf;
}

//- Categories: CurseForge's, with WoWInterface's mapped onto them
//
// Top-level categories are bits in SearchResult.cats (cat_categories), subcategories bits in
// SearchResult.subcats (cat_subcategories). An addon in a subcategory is in its parent too.

typedef struct CatCategory { S32 cf_id; char *name; } CatCategory;
static CatCategory cat_categories[] = {
    { 1018, "Action Bars" }, { 1067, "Achievements" }, { 1006, "Artwork" }, { 1002, "Auction & Economy" },
    { 1003, "Audio & Video" }, { 1009, "Bags & Inventory" }, { 1014, "Boss Encounters" }, { 1005, "Buffs & Debuffs" },
    { 11739, "Camping" }, { 1001, "Chat & Communication" }, { 1020, "Class" }, { 1019, "Combat" },
    { 1085, "Companions" }, { 1007, "Data Export" }, { 1031, "Development Tools" }, { 1469, "Garrison" },
    { 1008, "Guild" }, { 9338, "Housing" }, { 1010, "Libraries" }, { 1012, "Mail" },
    { 1011, "Map & Minimap" }, { 1038, "Minigames" }, { 1017, "Miscellaneous" }, { 1063, "Plugins" },
    { 1015, "Professions" }, { 1004, "PvP" }, { 1013, "Quests & Leveling" }, { 1060, "Roleplay" },
    { 1055, "Tooltip" }, { 1016, "Unit Frames" },
};
#define CAT_CATEGORY_COUNT ((U32)ArrayCount(cat_categories))

typedef struct CatSubcategory { S32 cf_id, parent_cf_id; char *name; } CatSubcategory;
static CatSubcategory cat_subcategories[] = {
    { 1036, 1020, "Death Knight" }, { 1502, 1020, "Demon Hunter" }, { 1023, 1020, "Druid" }, { 6137, 1020, "Evoker" },
    { 1024, 1020, "Hunter" }, { 1021, 1020, "Mage" }, { 1242, 1020, "Monk" }, { 1022, 1020, "Paladin" },
    { 1026, 1020, "Priest" }, { 1027, 1020, "Rogue" }, { 1025, 1020, "Shaman" }, { 1029, 1020, "Warlock" },
    { 1028, 1020, "Warrior" },
    { 1034, 1019, "Caster" }, { 1035, 1019, "Damage Dealer" }, { 1032, 1019, "Healer" }, { 1033, 1019, "Tank" },
    { 1243, 1085, "Battle Pets" },
    { 1066, 1063, "Data Broker" }, { 1064, 1063, "FuBar" }, { 1065, 1063, "Titan Panel" },
    { 1042, 1015, "Alchemy" }, { 1103, 1015, "Archaeology" }, { 1043, 1015, "Blacksmithing" }, { 1044, 1015, "Cooking" },
    { 1045, 1015, "Enchanting" }, { 1046, 1015, "Engineering" }, { 1047, 1015, "First Aid" }, { 1048, 1015, "Fishing" },
    { 1049, 1015, "Herbalism" }, { 1059, 1015, "Inscription" }, { 1050, 1015, "Jewelcrafting" }, { 1051, 1015, "Leatherworking" },
    { 1052, 1015, "Mining" }, { 1053, 1015, "Skinning" }, { 1054, 1015, "Tailoring" },
    { 1040, 1004, "Arena" }, { 1041, 1004, "Battleground" },
    { 1171, 1060, "Transmogrification" },
    { 1039, 1016, "HUDs" }, { 1037, 1016, "Raid Frames" },
};
#define CAT_SUBCATEGORY_COUNT ((U32)ArrayCount(cat_subcategories))

// What to ask CurseForge for to get a category's addons: its id and its subcategories' (an addon may be
// tagged with only one of those). Empty when that's more ids than it takes: then ask for all addons.
static String8 cat_cf_query(Arena *a, S64 cf_id)
{
    if (!cf_id) return str8(0, 0);
    String8List ids = { 0 };
    str8_list_push(a, &ids, str8_pushf(a, "%lld", cf_id));
    for (U32 i = 0; i < CAT_SUBCATEGORY_COUNT; i += 1) if (cat_subcategories[i].parent_cf_id == cf_id) str8_list_push(a, &ids, str8_pushf(a, "%d", cat_subcategories[i].cf_id));
    return ids.count <= 10 ? str8_list_join(a, &ids, str8_lit(",")) : str8(0, 0);
}

static U32 cat_bit_for_cf(S64 cf_id)
{
    for (U32 i = 0; i < CAT_CATEGORY_COUNT; i += 1) if (cat_categories[i].cf_id == cf_id) return 1u << i;
    return 0;
}

static U64 cat_subbit_for_cf(S64 cf_id)
{
    for (U32 i = 0; i < CAT_SUBCATEGORY_COUNT; i += 1) if (cat_subcategories[i].cf_id == cf_id) return 1ull << i;
    return 0;
}

// The subcategory's parent, as an index into cat_categories (-1 if none).
static S32 cat_sub_parent(U32 sub)
{
    for (U32 i = 0; i < CAT_CATEGORY_COUNT; i += 1) if (cat_categories[i].cf_id == cat_subcategories[sub].parent_cf_id) return (S32)i;
    return -1;
}

// A mod's "categories": top-level bits (a subcategory counts as its parent too) and subcategory bits.
static U32 cat_cf_bits(JSON *categories, U64 *subs)
{
    U32 bits = 0;
    U64 sb = 0;
    json_each(c, categories) {
        S64 id = json_int(json_get(c, "id")), parent = json_int(json_get(c, "parentCategoryId"));
        bits |= cat_bit_for_cf(parent > 1 ? parent : id);
        if (parent > 1) sb |= cat_subbit_for_cf(id);
    }
    if (subs) *subs = sb;
    return bits;
}

// WoWInterface's category ids (UICATID; it publishes no list, these are its site's names): the
// CurseForge category each maps to, and the subcategory where there is one (classes, roles, plugin bars).
static U32 cat_wowi_bits(S64 c, U64 *subs)
{
    static const S32 map[][3] = {
        { 19, 1018, 0 }, { 17, 1006, 0 }, { 147, 1006, 0 }, { 94, 1002, 0 }, { 114, 1060, 0 }, { 20, 1009, 0 }, { 45, 1014, 0 },
        { 22, 1005, 0 }, { 55, 1001, 0 },
        { 57, 1020, 1024 }, { 61, 1020, 1027 }, { 58, 1020, 1021 }, { 62, 1020, 1025 }, { 120, 1020, 1036 }, { 63, 1020, 1029 },
        { 56, 1020, 1023 }, { 59, 1020, 1022 }, { 64, 1020, 1028 }, { 60, 1020, 1026 }, { 152, 1020, 1242 },
        { 25, 1019, 0 }, { 112, 1019, 0 }, { 151, 1019, 1033 }, { 150, 1019, 1032 }, { 149, 1019, 1035 },
        { 146, 1085, 0 }, { 26, 1007, 0 }, { 35, 1031, 0 }, { 155, 1469, 0 }, { 95, 1008, 0 }, { 53, 1010, 0 }, { 47, 1010, 0 },
        { 97, 1012, 0 }, { 24, 1011, 0 }, { 138, 1011, 0 }, { 100, 1038, 0 },
        { 27, 1017, 0 }, { 111, 1017, 0 }, { 154, 1017, 0 }, { 113, 1017, 0 }, { 160, 1017, 0 }, { 161, 1017, 0 }, { 162, 1017, 0 }, { 163, 1017, 0 },
        { 33, 1063, 0 }, { 108, 1063, 1066 }, { 99, 1063, 1065 }, { 86, 1063, 1064 },
        { 135, 1063, 0 }, { 136, 1063, 0 }, { 134, 1063, 0 }, { 133, 1063, 0 }, { 130, 1063, 0 }, { 132, 1063, 0 },
        { 40, 1015, 0 }, { 96, 1004, 0 }, { 18, 1013, 0 }, { 98, 1055, 0 }, { 21, 1016, 0 }, { 127, 1016, 0 }, { 128, 1016, 0 }, { 129, 1016, 0 },
    };
    if (subs) *subs = 0;
    for (U32 i = 0; i < ArrayCount(map); i += 1) {
        if (map[i][0] != c) continue;
        if (subs && map[i][2]) *subs = cat_subbit_for_cf(map[i][2]);
        return cat_bit_for_cf(map[i][1]);
    }
    return 0;
}

// Wago's categories carry CurseForge's names, except Transmog (CurseForge: Roleplay › Transmogrification).
static U32 cat_wago_bits(JSON *categories, U64 *subs)
{
    U32 bits = 0;
    U64 sb = 0;
    json_each(c, categories) {
        String8 name = json_str(json_get(c, "display_name"));
        if (str8_match(name, str8_lit("Transmog"), 1)) { bits |= cat_bit_for_cf(1060); sb |= cat_subbit_for_cf(1171); continue; }
        for (U32 i = 0; i < CAT_CATEGORY_COUNT; i += 1) if (str8_match(name, str8_cstring(cat_categories[i].name), 1)) bits |= 1u << i;
    }
    if (subs) *subs = sb;
    return bits;
}

static String8 shots_join(Arena *a, String8List *l) { return str8_list_join(a, l, str8_lit(" ")); }

// Mends a shots list saved before wam encoded spaces in picture links: there, "https://.../MSBT 5.2.9
// In Action.png" came apart at its spaces. A piece that doesn't start a link (with http) belongs to
// the one before it, joined back with %20. Returns s itself when there's nothing to mend.
static String8 shots_repair(Arena *a, String8 s)
{
    B32 broken = 0;
    for (U64 i = 0; i + 1 < s.size && !broken; i += 1) broken = s.str[i] == ' ' && !str8_starts_with(str8_skip(s, i + 1), str8_lit("http"), 1);
    if (!broken) return s;
    String8List out = { 0 };
    String8 cur = { 0 };
    while (s.size) {
        U64 sp = str8_find(s, 0, str8_lit(" "), 0);
        String8 piece = str8_prefix(s, sp);
        s = str8_skip(s, sp + 1);
        if (!piece.size) continue;
        if (cur.size && !str8_starts_with(piece, str8_lit("http"), 1)) cur = str8_pushf(a, "%.*s%%20%.*s", S8F(cur), S8F(piece));
        else { if (cur.size) str8_list_push(a, &out, cur); cur = piece; }
    }
    if (cur.size) str8_list_push(a, &out, cur);
    return shots_join(a, &out);
}

static S64 cat_now(void)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (S64)(((((U64)ft.dwHighDateTime << 32) | ft.dwLowDateTime) - 116444736000000000ull) / 10000000ull);
}

static char *cat_source_name(String8 s)
{
    static char *names[] = { "CurseForge", "Wago", "WoWInterface", "Tukui", "GitHub" };
    for (U32 i = 0; i < ArrayCount(names); i += 1) if (str8_match(s, str8_cstring(names[i]), 0)) return names[i];
    return 0;
}

//- Disk cache: a header line, then one tab-separated line per entry
//   wam-catalog  4  <saved, unix seconds>  <game version major.minor.patch>  [wago]
// ("wago": fetched with Wago's lists.)
//   id  source  name  author  website  downloads  updated  created  available  site_only  cats  subcats  icon  shots  summary

static String8 cat_cache_path(Arena *a, char *flavor_folder)
{
    String8 dir = os_get_env(a, "WAM_CATALOG_DIR");
    if (!dir.size) {
        String8 local = os_get_env(a, "LOCALAPPDATA");
        if (!local.size) return str8(0, 0);
        dir = path_join(a, path_join(a, local, str8_lit("wam")), str8_lit("catalog"));
    }
    return path_join(a, dir, str8_pushf(a, "%s.tsv", flavor_folder));
}

static void cat_put_field(String8List *l, Arena *a, String8 s)
{
    U8 *out = push_array_nz(a, U8, s.size + 1);
    for (U64 i = 0; i < s.size; i += 1) { U8 ch = s.str[i]; out[i] = ch == '\t' || ch == '\n' || ch == '\r' ? ' ' : ch; }
    str8_list_push(a, l, str8(out, s.size));
}

static String8 cat_serialize(Arena *a, Catalog *c, GameVersion cv, S64 saved)
{
    String8List l = { 0 };
    str8_list_push(a, &l, str8_pushf(a, "wam-catalog\t%d\t%lld\t%u.%u.%u%s\n", CAT_FORMAT, saved, cv.major, cv.minor, cv.patch, c->has_wago ? "\twago" : ""));
    for (U64 i = 0; i < c->count; i += 1) {
        SearchResult *e = &c->v[i];
        cat_put_field(&l, a, e->id);           str8_list_push(a, &l, str8_lit("\t"));
        str8_list_push(a, &l, str8_cstring(e->source ? e->source : "")); str8_list_push(a, &l, str8_lit("\t"));
        cat_put_field(&l, a, e->name);         str8_list_push(a, &l, str8_lit("\t"));
        cat_put_field(&l, a, e->author);       str8_list_push(a, &l, str8_lit("\t"));
        cat_put_field(&l, a, e->website);
        str8_list_push(a, &l, str8_pushf(a, "\t%lld\t%lld\t%lld\t%d\t%d\t%x\t%llx\t", e->downloads, e->updated, e->created, e->available ? 1 : 0, (int)e->site_only, e->cats, e->subcats));
        cat_put_field(&l, a, e->icon);         str8_list_push(a, &l, str8_lit("\t"));
        cat_put_field(&l, a, e->shots);        str8_list_push(a, &l, str8_lit("\t"));
        cat_put_field(&l, a, e->summary);      str8_list_push(a, &l, str8_lit("\n"));
    }
    return str8_list_join(a, &l, str8(0, 0));
}

// Reads a cache into c (merging). Returns 0 when it isn't one, or was made for another game version.
static B32 cat_parse(Catalog *c, String8 data, GameVersion cv, S64 *saved)
{
    String8 header = str8_pop_line(&data);
    String8 f[5] = { 0 };
    U32 n = 0;
    while (header.size && n < 5) {
        U64 tab = str8_find(header, 0, str8_lit("\t"), 0);
        f[n++] = str8_trim(str8_prefix(header, tab));
        header = str8_skip(header, tab + 1);
    }
    if (n < 4 || !str8_match(f[0], str8_lit("wam-catalog"), 0) || str8_to_s64(f[1]) != CAT_FORMAT) return 0;
    U32 v[3]; parse_version3(f[3], v);
    if (cv.valid && (v[0] != cv.major || v[1] != cv.minor || v[2] != cv.patch)) return 0;
    if (saved) *saved = str8_to_s64(f[2]);
    if (str8_match(f[4], str8_lit("wago"), 0)) c->has_wago = 1;
    while (data.size) {
        String8 line = str8_pop_line(&data);
        String8 col[15] = { 0 };
        U32 k = 0;
        while (k < 14) {
            U64 tab = str8_find(line, 0, str8_lit("\t"), 0);
            if (tab >= line.size) break;
            col[k++] = str8_prefix(line, tab);
            line = str8_skip(line, tab + 1);
        }
        if (k < 14) continue;
        col[14] = line;
        SearchResult r = { 0 };
        r.id = col[0]; r.source = cat_source_name(col[1]); r.name = col[2]; r.author = col[3]; r.website = col[4];
        r.downloads = str8_to_s64(col[5]); r.updated = str8_to_s64(col[6]); r.created = str8_to_s64(col[7]);
        r.available = str8_to_s64(col[8]) != 0; r.site_only = (U8)str8_to_s64(col[9]);
        r.cats = (U32)strtoul((char *)str8_copy(c->arena, col[10]).str, 0, 16);
        r.subcats = _strtoui64((char *)str8_copy(c->arena, col[11]).str, 0, 16);
        r.icon = url_escape_unsafe(c->arena, col[12]); r.shots = shots_repair(c->arena, col[13]); r.summary = str8_trim(col[14]);
        cat_put(c, &r);
    }
    return 1;
}

static B32 cat_save(Catalog *c, String8 path, GameVersion cv, S64 saved)
{
    if (!path.size) return 0;
    Temp scratch = scratch_begin(0);
    os_make_dirs(path_dirname(path));
    String8 tmp = str8_pushf(scratch.arena, "%.*s.tmp", S8F(path));
    B32 ok = os_write_file(tmp, cat_serialize(scratch.arena, c, cv, saved)) && os_replace_file(tmp, path);
    scratch_end(scratch);
    return ok;
}

//- Fetching

// wago: 0 = not Wago, else Wago's list for category wago - 1 (0: the featured list). tukui: only
// fetches Tukui's list into its cache (tukui_list), read when the results are merged.
typedef struct CatJob { int cf_sort; U64 index; B32 wowi, tukui; U32 wago; Arena *arena; HTTP_Result r; } CatJob;
typedef struct CatFetch { CatJob *jobs; String8 api_key, wago_key; GameVersion cv; } CatFetch;

static void cat_fetch_one(void *user, U64 i)
{
    CatFetch *f = (CatFetch *)user;
    CatJob *j = &f->jobs[i];
    j->arena = arena_alloc(GB(1));
    if (j->tukui) { tukui_list(j->arena, 0); j->r.ok = 1; return; }
    j->r = j->wowi ? wowi_filelist(j->arena)
         : j->wago ? wago_browse(j->arena, f->wago_key, f->cv, j->wago - 1)
         : cf_search_page(j->arena, f->api_key, str8(0, 0), str8(0, 0), str8(0, 0), j->cf_sort, j->index);
}

// Fetches the whole catalog for m's flavor into c (merging). Requests run in parallel: about 60 of
// them with a Wago key, which mostly wait on the network, so on up to 24 threads (8, the CPU-bound
// limit, made a Wago key cost a second and a half more). Returns the number of entries it got;
// *error says what failed (partial results still count).
// Which parts cat_fetch_parts asks for: CurseForge, WoWInterface and Tukui; and Wago (with a key),
// which answers slowest, so the GUI fetches it beside the rest and shows the rest first.
#define CAT_PART_CORE 1
#define CAT_PART_WAGO 2

static U64 cat_fetch_parts(Mgr *m, Catalog *c, String8 *error, U32 parts)
{
    static const struct { int sort; U32 pages; } plan[] = {
        { CF_SORT_DOWNLOADS, CAT_PAGES_DOWNLOADS }, { CF_SORT_UPDATED, CAT_PAGES_UPDATED }, { CF_SORT_RELEASED, CAT_PAGES_RELEASED },
    };
    CatJob jobs[2 + CAT_PAGES_DOWNLOADS + CAT_PAGES_UPDATED + CAT_PAGES_RELEASED + 1 + WAGO_CATEGORY_COUNT] = { 0 };
    U64 njobs = 0;
    if (parts & CAT_PART_CORE) {
        jobs[njobs++].wowi = 1;
        jobs[njobs++].tukui = 1;
    }
    if ((parts & CAT_PART_CORE) && m->api_key.size) {
        for (U32 p = 0; p < ArrayCount(plan); p += 1) {
            for (U32 k = 0; k < plan[p].pages; k += 1) { jobs[njobs].cf_sort = plan[p].sort; jobs[njobs].index = (U64)k * CF_PAGE_SIZE; njobs += 1; }
        }
    }
    // Wago: 30 searches, half of its 60 requests a minute, once per catalog refresh.
    if ((parts & CAT_PART_WAGO) && m->wago_key.size) for (U32 k = 0; k <= WAGO_CATEGORY_COUNT; k += 1) jobs[njobs++].wago = k + 1;
    CatFetch f = { jobs, m->api_key, m->wago_key, m->client };
    os_parallel_for_threads(njobs, cat_fetch_one, &f, 24);

    U64 got = 0;
    String8 first_error = { 0 };
    for (U64 i = 0; i < njobs; i += 1) {
        CatJob *j = &jobs[i];
        if (!j->r.ok) {
            if (!first_error.size) first_error = str8_pushf(m->arena, "%s: %.*s", j->wowi ? "WoWInterface" : j->wago ? "Wago" : "CurseForge", S8F(j->r.error));
            arena_release(j->arena);
            continue;
        }
        if (j->tukui) { arena_release(j->arena); continue; } // (its list: below, from the cache it filled)
        JSON *root = json_parse(j->arena, j->r.body);
        JSON *list = j->wowi ? root : json_get(root, "data");
        json_each(item, list) {
            if (item->type != JSON_OBJ) continue;
            SearchResult sr = { 0 };
            if (j->wowi) wowi_search_result(m, item, &sr);
            else if (j->wago) wago_search_result(m, item, &sr);
            else cf_search_result(m, item, &sr);
            got += cat_put(c, &sr) != 0;
        }
        if (j->wago) c->has_wago = 1;
        arena_release(j->arena);
    }
    JSON *tukui = parts & CAT_PART_CORE ? tukui_list(m->arena, 0) : &json_nil;
    json_each(item, tukui) {
        SearchResult sr = { 0 };
        tukui_search_result(m, item, &sr);
        got += cat_put(c, &sr) != 0;
    }
    if (error) *error = first_error;
    return got;
}

static U64 cat_fetch(Mgr *m, Catalog *c, String8 *error) { return cat_fetch_parts(m, c, error, CAT_PART_CORE | CAT_PART_WAGO); }

// Wago's entries from an older copy of the catalog (its cache file), when fetching them failed (a
// rate limit, say): better last time's Wago addons than none until the next refresh.
static void cat_keep_wago(Catalog *c, String8 old_file, GameVersion cv)
{
    Arena *a = arena_alloc(GB(1));
    Catalog old = { 0 };
    cat_init(&old, a);
    if (cat_parse(&old, old_file, cv, 0)) {
        for (U64 i = 0; i < old.count; i += 1) if (id_is(old.v[i].id, "wago:") && !cat_find(c, old.v[i].id)) cat_put(c, &old.v[i]);
        c->has_wago = old.has_wago;
    }
    arena_release(a);
}

//- Details: what the GUI's detail panel shows for one addon

#define CHANGELOG_MAX KB(40)

typedef struct AddonDetails {
    B32 ok;
    String8 error;
    String8 id, version;
    Rich *description, *changelog;               // 0 or empty: none (markup.c)
    String8 shots;                               // like SearchResult.shots
    String8 website, source_url, issues_url, donate_url;
    S64 favorites;
} AddonDetails;

typedef struct DetailJob { String8 api_key; String8 path; Arena *arena; HTTP_Result r; } DetailJob;

static void detail_fetch_one(void *user, U64 i)
{
    DetailJob *j = &((DetailJob *)user)[i];
    j->r = cf_call(j->arena, j->api_key, "GET", j->path, str8(0, 0));
}

static AddonDetails cat_details_get(Mgr *m, String8 id)
{
    Arena *a = m->arena;
    AddonDetails d = { 0 };
    d.id = id;
    if (id_is(id, "cf:")) {
        if (!m->api_key.size) { d.error = str8_lit("CurseForge details need an API key"); return d; }
        S64 mod_id = id_number(id);
        DetailJob jobs[2] = { { m->api_key, str8_pushf(a, "/v1/mods/%lld", mod_id), a, { 0 } },
                              { m->api_key, str8_pushf(a, "/v1/mods/%lld/description", mod_id), arena_alloc(GB(1)), { 0 } } };
        os_parallel_for(2, detail_fetch_one, jobs);
        JSON *mod = json_get(json_parse(a, jobs[0].r.body), "data");
        if (!jobs[0].r.ok || mod->type != JSON_OBJ) { d.error = str8_pushf(a, "CurseForge: %.*s", S8F(jobs[0].r.error)); arena_release(jobs[1].arena); return d; }
        if (jobs[1].r.ok) d.description = markup_cf(a, json_str(json_get(json_parse(jobs[1].arena, jobs[1].r.body), "data")), RICH_TEXT_MAX);
        arena_release(jobs[1].arena);
        SearchResult sr = { 0 };
        cf_search_result(m, mod, &sr);
        d.shots = sr.shots;
        JSON *links = json_get(mod, "links");
        d.website = json_str(json_get(links, "websiteUrl"));
        d.source_url = json_str(json_get(links, "sourceUrl"));
        d.issues_url = json_str(json_get(links, "issuesUrl"));
        d.favorites = json_int(json_get(mod, "thumbsUpCount"));
        S64 file_id = mgr_pick_file(m, mod);
        if (file_id) {
            json_each(f, json_get(mod, "latestFiles")) if (json_int(json_get(f, "id")) == file_id) d.version = json_str(json_get(f, "displayName"));
            HTTP_Result r = cf_call(a, m->api_key, "GET", str8_pushf(a, "/v1/mods/%lld/files/%lld/changelog", mod_id, file_id), str8(0, 0));
            if (r.ok) d.changelog = markup_cf(a, json_str(json_get(json_parse(a, r.body), "data")), CHANGELOG_MAX);
        }
        d.ok = 1;
    } else if (id_is(id, "wowi:")) {
        S64 uid = id_number(id);
        HTTP_Result r = wowi_details(a, &uid, 1);
        JSON *item = json_parse(a, r.body)->first;
        if (!r.ok || !item || item->type != JSON_OBJ) { d.error = str8_pushf(a, "WoWInterface: %.*s", S8F(r.ok ? str8_lit("not found") : r.error)); return d; }
        d.version = json_str(json_get(item, "UIVersion"));
        d.description = rich_parse(a, json_str(json_get(item, "UIDescription")), MARKUP_BBCODE, RICH_TEXT_MAX);
        d.changelog = rich_parse(a, json_str(json_get(item, "UIChangeLog")), MARKUP_BBCODE, CHANGELOG_MAX);
        String8List shots = { 0 };
        json_each(img, json_get(item, "UIIMGs")) if (shots.count < SHOTS_MAX && json_str(img).size) shots_push(a, &shots, json_str(img), str8(0, 0));
        d.shots = shots_join(a, &shots); // (empty: the catalog's are shown)
        d.favorites = json_int(json_get(item, "UIFavoriteTotal"));
        d.website = str8_pushf(a, "https://www.wowinterface.com/downloads/info%lld", uid);
        d.ok = 1;
    } else if (id_is(id, "wago:")) {
        if (!m->wago_key.size) { d.error = str8_lit("Wago details need your Wago API key (add it in Settings)"); return d; }
        HTTP_Result r = wago_addon(a, m->wago_key, m->client, id_rest(id));
        JSON *item = json_parse(a, r.body);
        if (!r.ok || item->type != JSON_OBJ) { d.error = str8_pushf(a, "Wago: %.*s", S8F(r.ok ? str8_lit("unexpected response") : r.error)); return d; }
        d.description = rich_parse(a, json_str(json_get(item, "description")), MARKUP_MARKDOWN, RICH_TEXT_MAX);
        if (rich_empty(d.description)) d.description = rich_parse(a, json_str(json_get(item, "summary")), MARKUP_MARKDOWN, RICH_TEXT_MAX);
        JSON *rel = wago_stable_release(json_get(item, "recent_release"));
        if (rel) {
            d.version = json_str(json_get(rel, "label"));
            d.changelog = rich_parse(a, json_str(json_get(rel, "changelog")), MARKUP_MARKDOWN, CHANGELOG_MAX);
        }
        String8List shots = { 0 };
        json_each(g, json_get(item, "gallery")) {
            String8 type = json_str(json_get(g, "type")), path = json_str(json_get(g, "path"));
            if (shots.count < SHOTS_MAX && (!type.size || str8_match(type, str8_lit("image"), 1)) && str8_starts_with(path, str8_lit("https://"), 1)) shots_push(a, &shots, path, str8(0, 0));
        }
        d.shots = shots_join(a, &shots);
        d.website = json_str(json_get(item, "website_url"));
        // "website" is the author's own link: usually the source repository.
        String8 site = json_str(json_get(item, "website"));
        if (str8_starts_with(site, str8_lit("https://"), 1)) d.source_url = site;
        d.ok = 1;
    } else if (id_is(id, "tukui:")) {
        JSON *list = tukui_list(a, &d.error);
        json_each(item, list) {
            if (!str8_match(json_str(json_get(item, "slug")), id_rest(id), 1)) continue;
            SearchResult sr = { 0 };
            tukui_search_result(m, item, &sr);
            d.shots = sr.shots;
            d.version = json_str(json_get(item, "version"));
            d.description = rich_parse(a, json_str(json_get(item, "desc")), MARKUP_MARKDOWN, RICH_TEXT_MAX);
            d.website = json_str(json_get(item, "web_url"));
            d.source_url = json_str(json_get(item, "git_url"));
            d.issues_url = json_str(json_get(item, "ticket_url"));
            d.donate_url = json_str(json_get(item, "donate_url"));
            String8 cl = json_str(json_get(item, "changelog_url"));
            if (str8_starts_with(cl, str8_lit("https://"), 1)) {
                HTTP_Result r = http_get_url(a, cl, str8(0, 0), 0, 0);
                if (r.ok) {
                    JSON *j = json_parse(a, r.body); // the changelog may come as text or wrapped in JSON
                    String8 text = j->type == JSON_STR ? j->str : j->type == JSON_OBJ ? json_str(json_get(j, "changelog")) : r.body;
                    d.changelog = rich_parse(a, text, MARKUP_MARKDOWN, CHANGELOG_MAX);
                }
            }
            d.ok = 1;
        }
        if (!d.ok && !d.error.size) d.error = str8_lit("Tukui doesn't list it anymore");
    } else {
        d.error = str8_lit("This source has no details to show here; its page has them");
    }
    return d;
}

static AddonDetails cat_details(Mgr *m, String8 id)
{
    m->all_shots = 1;
    AddonDetails d = cat_details_get(m, id);
    m->all_shots = 0;
    return d;
}

//- Changes: what an update would bring
//
// For an installed addon with an update: the releases after the installed one, newest first, each
// with its changelog. CurseForge and GitHub list every release; WoWInterface, Wago and Tukui have
// one changelog, which usually covers several versions: it's cut where it mentions the installed
// version, when it does.

#define CHANGES_MAX 10

typedef struct ChangeEntry { String8 version, date; Rich *text; } ChangeEntry; // date: YYYY-MM-DD (or empty)

typedef struct AddonChanges {
    B32 ok;
    String8 error;
    String8 id, name, from, to;   // installed version, the version an update installs
    ChangeEntry v[CHANGES_MAX];
    U32 count;
    B32 more;                     // there are older releases after the installed one than shown
    B32 single;                   // the source's one changelog (not one per release)
} AddonChanges;

typedef struct ChangelogJob { String8 api_key; S64 mod_id, file_id; Arena *arena; HTTP_Result r; } ChangelogJob;

static void changelog_fetch_one(void *user, U64 i)
{
    ChangelogJob *j = &((ChangelogJob *)user)[i];
    j->r = cf_call(j->arena, j->api_key, "GET", str8_pushf(j->arena, "/v1/mods/%lld/files/%lld/changelog", j->mod_id, j->file_id), str8(0, 0));
}

static void changes_cf(Mgr *m, MgrAddon *e, AddonChanges *c)
{
    Arena *a = m->arena;
    if (!m->api_key.size) { c->error = str8_lit("CurseForge needs an API key"); return; }
    S64 mod_id = id_number(e->id);
    // The project's files, newest first, 50 a page: until the installed one (an addon that also
    // ships Classic builds may have many newer files for other games), at most 4 pages.
    JSON *all[200]; U32 nall = 0;
    S64 oldest_listed = 0;
    B32 listed_all = 0;
    for (U32 page = 0; page < 4; page += 1) {
        HTTP_Result r = cf_call(a, m->api_key, "GET", str8_pushf(a, "/v1/mods/%lld/files?pageSize=50&index=%u", mod_id, page * 50), str8(0, 0));
        JSON *files = json_get(json_parse(a, r.body), "data");
        if (!r.ok || files->type != JSON_ARR) {
            if (page == 0) { c->error = str8_pushf(a, "CurseForge: %.*s", S8F(r.ok ? str8_lit("unexpected response") : r.error)); return; }
            break;
        }
        json_each(f, files) {
            S64 id = json_int(json_get(f, "id"));
            if (!oldest_listed || id < oldest_listed) oldest_listed = id;
            if (nall < ArrayCount(all)) all[nall++] = f;
        }
        if (files->count < 50) { listed_all = 1; break; }
        if (oldest_listed && oldest_listed <= e->file_id) break;
    }
    // Only files for this game (not a Classic build of a retail addon): the game's major version
    // (retail: any retail one, as the update's file picking allows), else the last check's file's.
    // And only as stable as that file: releases, unless it was a beta or an alpha.
    JSON *latest = 0;
    for (U32 i = 0; i < nall; i += 1) if (json_int(json_get(all[i], "id")) == e->latest_file_id) latest = all[i];
    S64 max_type = latest ? Max(json_int(json_get(latest, "releaseType")), 1) : 1;
    U32 majors[16]; U32 nmajors = 0;
    if (m->client.valid && m->client.major >= 10) for (U32 v = 10; v <= m->client.major && nmajors < ArrayCount(majors); v += 1) majors[nmajors++] = v;
    else if (m->client.valid) majors[nmajors++] = m->client.major;
    else if (latest) json_each(gv, json_get(latest, "gameVersions")) {
        U32 v[3]; parse_version3(json_str(gv), v);
        if (v[0] && nmajors < ArrayCount(majors)) majors[nmajors++] = v[0];
    }
    // Newest first: the files after the installed one (up to the newest: what an update installs,
    // which may be newer than the last check's).
    JSON *pick[64]; U32 npick = 0;
    for (U32 i = 0; i < nall; i += 1) {
        JSON *f = all[i];
        S64 id = json_int(json_get(f, "id"));
        if (id <= e->file_id) continue;
        if (json_int(json_get(f, "releaseType")) > max_type) continue;
        B32 same_game = nmajors == 0;
        json_each(gv, json_get(f, "gameVersions")) {
            U32 v[3]; parse_version3(json_str(gv), v);
            for (U32 k = 0; k < nmajors; k += 1) same_game |= v[0] == majors[k];
        }
        if (same_game && npick < ArrayCount(pick)) pick[npick++] = f;
    }
    for (U32 i = 1; i < npick; i += 1) { // by id, newest first (a few dozen at most)
        for (U32 k = i; k > 0 && json_int(json_get(pick[k], "id")) > json_int(json_get(pick[k - 1], "id")); k -= 1) {
            JSON *t = pick[k]; pick[k] = pick[k - 1]; pick[k - 1] = t;
        }
    }
    c->more = npick > CHANGES_MAX || (!listed_all && oldest_listed > e->file_id);
    U32 n = Min(npick, CHANGES_MAX);
    ChangelogJob jobs[CHANGES_MAX];
    for (U32 i = 0; i < n; i += 1) {
        jobs[i].api_key = m->api_key; jobs[i].mod_id = mod_id; jobs[i].file_id = json_int(json_get(pick[i], "id"));
        jobs[i].arena = arena_alloc(GB(1)); jobs[i].r = (HTTP_Result){ 0 };
    }
    os_parallel_for(n, changelog_fetch_one, jobs);
    for (U32 i = 0; i < n; i += 1) {
        ChangeEntry *ce = &c->v[c->count++];
        ce->version = json_str(json_get(pick[i], "displayName"));
        ce->date = str8_prefix(json_str(json_get(pick[i], "fileDate")), 10);
        if (jobs[i].r.ok) ce->text = markup_cf(a, json_str(json_get(json_parse(jobs[i].arena, jobs[i].r.body), "data")), CHANGELOG_MAX);
        arena_release(jobs[i].arena);
    }
    c->ok = 1;
}

static void changes_github(Mgr *m, MgrAddon *e, AddonChanges *c)
{
    Arena *a = m->arena;
    String8 repo = id_rest(e->id);
    HTTP_Result r = http_get_url(a, str8_pushf(a, "https://api.github.com/repos/%.*s/releases?per_page=30", S8F(repo)), str8_lit(GH_HEADERS), 0, 0);
    JSON *list = json_parse(a, r.body);
    if (!r.ok || list->type != JSON_ARR) {
        c->error = r.status == 403 || r.status == 429 ? str8_lit("GitHub's limit for apps without an account is reached; try again in a while")
                 : str8_pushf(a, "GitHub: %.*s", S8F(r.ok ? str8_lit("unexpected response") : r.error));
        return;
    }
    B32 reached = 0;
    json_each(rel, list) {
        if (json_bool(json_get(rel, "draft"), 0) || json_bool(json_get(rel, "prerelease"), 0)) continue;
        String8 tag = json_str(json_get(rel, "tag_name")), title = json_str(json_get(rel, "name"));
        if (versions_equal(tag, e->version) || versions_equal(title, e->version)) { reached = 1; break; }
        if (c->count == CHANGES_MAX) { c->more = 1; break; }
        ChangeEntry *ce = &c->v[c->count++];
        ce->version = tag.size ? tag : title;
        ce->date = str8_prefix(json_str(json_get(rel, "published_at")), 10);
        ce->text = rich_parse(a, json_str(json_get(rel, "body")), MARKUP_MARKDOWN, CHANGELOG_MAX);
    }
    if (!reached && c->count == CHANGES_MAX) c->more = 1;
    c->ok = 1;
}

static AddonChanges mgr_changes(Mgr *m, String8 id)
{
    Arena *a = m->arena;
    AddonChanges c = { 0 };
    c.id = id;
    MgrAddon *e = mgr_find(m, id);
    if (!e) { c.error = str8_pushf(a, "%.*s isn't installed by wam", S8F(id)); return c; }
    c.name = e->name.size ? e->name : e->id;
    c.from = e->version;
    c.to = e->latest_version;
    if (id_is(id, "cf:")) changes_cf(m, e, &c);
    else if (id_is(id, "gh:")) changes_github(m, e, &c);
    else {
        AddonDetails d = cat_details(m, id);
        if (!d.ok) { c.error = d.error; return c; }
        ChangeEntry *ce = &c.v[c.count++];
        ce->version = d.version.size ? d.version : e->latest_version;
        rich_until(d.changelog, e->version);
        ce->text = d.changelog;
        c.single = 1;
        c.ok = 1;
    }
    // An update installs the newest release, which may be newer than the last check found.
    if (c.count && c.v[0].version.size && !c.single) c.to = c.v[0].version;
    return c;
}

// Adds the CurseForge mods with these ids that c doesn't have yet (one request per 50). Used for
// installed addons: the prefetch covers the popular ones, not necessarily these. Returns how many.
static U64 cat_add_cf_mods(Mgr *m, Catalog *c, S64 *ids, U64 count)
{
    if (!m->api_key.size) return 0;
    Temp scratch = scratch_begin(m->arena);
    S64 *missing = push_array(scratch.arena, S64, count + 1);
    U64 n = 0;
    for (U64 i = 0; i < count; i += 1) {
        if (ids[i] <= 0 || cat_find(c, str8_pushf(scratch.arena, "cf:%lld", ids[i]))) continue;
        B32 dup = 0;
        for (U64 k = 0; k < n && !dup; k += 1) dup = missing[k] == ids[i];
        if (!dup) missing[n++] = ids[i];
    }
    U64 added = 0;
    for (U64 at = 0; at < n; at += 50) {
        HTTP_Result r = cf_get_mods(m->arena, m->api_key, missing + at, Min(50, n - at));
        if (!r.ok) break;
        json_each(mod, json_get(json_parse(m->arena, r.body), "data")) {
            if (mod->type != JSON_OBJ) continue;
            SearchResult sr = { 0 };
            cf_search_result(m, mod, &sr);
            added += cat_put(c, &sr) != 0;
        }
    }
    scratch_end(scratch);
    return added;
}
