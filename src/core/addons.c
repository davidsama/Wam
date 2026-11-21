// addons.c

//- Locating WoW

//- Finding World of Warcraft
//
// A WoW root is the folder holding the flavor folders (_retail_, _classic_era_, ...). Sources, in
// order: the WAM_WOW_DIR environment variable, the folder the user picked last time (saved in
// %APPDATA%\wam\settings.json), Windows' list of installed programs (Battle.net records WoW's
// install folder there), then the usual folders on every drive.

static B32 wow_is_root(String8 dir)
{
    if (!dir.size) return 0;
    Temp t = scratch_begin(0);
    B32 found = 0;
    for (int f = 0; f < (int)ArrayCount(wow_flavors) && !found; f += 1) {
        found = os_dir_exists(path_join(t.arena, dir, str8_cstring(wow_flavors[f].folder)));
    }
    scratch_end(t);
    return found;
}

// Accepts the root itself, a flavor folder, its Interface or Interface\AddOns, or the .exe's folder;
// returns the root, or empty if `path` isn't inside a WoW install.
static String8 wow_normalize_root(Arena *a, String8 path)
{
    path = str8_trim(path);
    while (path.size && (path.str[path.size - 1] == '\\' || path.str[path.size - 1] == '/')) path.size -= 1;
    String8 p = path_normalize_slashes(a, path);
    for (int up = 0; up < 4 && p.size; up += 1) {
        if (!os_dir_exists(p)) break;
        if (wow_is_root(p)) return str8_copy(a, p);
        String8 parent = path_dirname(p);
        while (parent.size && parent.str[parent.size - 1] == '\\') parent.size -= 1;
        if (parent.size >= p.size) break;
        p = parent;
    }
    return str8(0, 0);
}

static String8 wam_settings_path(Arena *a)
{
    String8 appdata = os_get_env(a, "APPDATA");
    if (!appdata.size) return str8(0, 0);
    return path_join(a, path_join(a, appdata, str8_lit("wam")), str8_lit("settings.json"));
}

// %APPDATA%\wam\settings.json: what the user chose.
//   wowRoot      the World of Warcraft folder to use
//   detailShare  the Get addons detail panel's share of the view's width (GUI, 0..1)
//   window       the GUI window's last restored rectangle in screen pixels {x, y, width, height}
//                and whether it was maximized
typedef struct WamWindow { S32 x, y, w, h; B32 maximized; } WamWindow; // w = 0: none saved
typedef struct WamSettings { String8 wow_root; F64 detail_share; WamWindow window; } WamSettings;

static WamSettings wam_settings_load(Arena *a)
{
    WamSettings st = { 0 };
    String8 path = wam_settings_path(a);
    if (!path.size) return st;
    JSON *j = json_parse(a, os_read_file(a, path));
    st.wow_root = str8_copy(a, json_str(json_get(j, "wowRoot")));
    JSON *w = json_get(j, "detailShare");
    if (w->type == JSON_NUM && w->num > 0 && w->num < 1) st.detail_share = w->num;
    JSON *win = json_get(j, "window");
    S64 ww = json_int(json_get(win, "width")), wh = json_int(json_get(win, "height"));
    if (ww > 0 && wh > 0 && ww < 100000 && wh < 100000) {
        st.window = (WamWindow){ (S32)json_int(json_get(win, "x")), (S32)json_int(json_get(win, "y")), (S32)ww, (S32)wh,
                                 json_bool(json_get(win, "maximized"), 0) };
    }
    return st;
}

static B32 wam_settings_save(WamSettings *st)
{
    Temp t = scratch_begin(0);
    String8 path = wam_settings_path(t.arena);
    String8List l = { 0 }; // only what's set
    if (st->wow_root.size) str8_list_push(t.arena, &l, str8_pushf(t.arena, "  \"wowRoot\": \"%.*s\"", S8F(json_escape(t.arena, st->wow_root))));
    if (st->detail_share > 0) str8_list_push(t.arena, &l, str8_pushf(t.arena, "  \"detailShare\": %.4f", st->detail_share));
    WamWindow *win = &st->window;
    if (win->w > 0 && win->h > 0) {
        str8_list_push(t.arena, &l, str8_pushf(t.arena, "  \"window\": { \"x\": %d, \"y\": %d, \"width\": %d, \"height\": %d, \"maximized\": %s }",
                                               win->x, win->y, win->w, win->h, win->maximized ? "true" : "false"));
    }
    String8 body = str8_pushf(t.arena, "{\n%.*s\n}\n", S8F(str8_list_join(t.arena, &l, str8_lit(",\n"))));
    B32 ok = path.size && os_make_dirs(path_dirname(path)) && os_write_file(path, body);
    scratch_end(t);
    return ok;
}

static String8 wam_saved_root(Arena *a) { return wam_settings_load(a).wow_root; }

static B32 wam_save_root(String8 root)
{
    Temp t = scratch_begin(0);
    WamSettings st = wam_settings_load(t.arena);
    st.wow_root = root;
    B32 ok = wam_settings_save(&st);
    scratch_end(t);
    return ok;
}

static B32 wam_save_window(WamWindow window)
{
    Temp t = scratch_begin(0);
    WamSettings st = wam_settings_load(t.arena);
    st.window = window;
    B32 ok = wam_settings_save(&st);
    scratch_end(t);
    return ok;
}

static B32 wam_save_detail_share(F64 share)
{
    Temp t = scratch_begin(0);
    WamSettings st = wam_settings_load(t.arena);
    st.detail_share = share;
    B32 ok = wam_settings_save(&st);
    scratch_end(t);
    return ok;
}

static void roots_add(Arena *a, String8List *l, String8 candidate)
{
    String8 root = wow_normalize_root(a, candidate);
    if (!root.size) return;
    for (String8Node *n = l->first; n; n = n->next) if (str8_match(n->string, root, 1)) return;
    str8_list_push(a, l, root);
}

// Every WoW install wam can find (not counting WAM_WOW_DIR or the saved choice).
static String8List wow_find_roots(Arena *a)
{
    String8List roots = { 0 };
    String8List installed = os_installed_program_dirs(a, "World of Warcraft");
    for (String8Node *n = installed.first; n; n = n->next) roots_add(a, &roots, n->string);

    static char *candidates[] = {
        "Wow\\World of Warcraft",
        "World of Warcraft",
        "Program Files (x86)\\World of Warcraft",
        "Program Files\\World of Warcraft",
        "Games\\World of Warcraft",
        "Blizzard\\World of Warcraft",
        "Battle.net\\World of Warcraft",
    };
    DWORD drives = GetLogicalDrives();
    for (int d = 2; d < 26; d += 1) { // skip A: and B:
        if (!(drives & (1u << d))) continue;
        WCHAR drive[4] = { (WCHAR)('A' + d), ':', '\\', 0 };
        UINT type = GetDriveTypeW(drive);
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) continue; // network drives can stall
        for (int c = 0; c < (int)ArrayCount(candidates); c += 1) {
            String8 cand = str8_pushf(a, "%c:\\%s", 'A' + d, candidates[c]);
            if (os_dir_exists(cand)) roots_add(a, &roots, cand); // one check before the per-flavor ones
        }
    }
    return roots;
}

// The install to use: WAM_WOW_DIR, else the saved choice (if it's still there), else the first found.
static String8 wow_find_root(Arena *a)
{
    String8 env = wow_normalize_root(a, os_get_env(a, "WAM_WOW_DIR"));
    if (env.size) return env;
    String8 saved = wow_normalize_root(a, wam_saved_root(a));
    if (saved.size) return saved;
    String8List roots = wow_find_roots(a);
    return roots.first ? roots.first->string : str8(0, 0);
}

static String8 wow_resolve_addons_dir(Arena *a, String8 input, char *flavor_folder)
{
    if (!input.size) return input;
    while (input.size && (input.str[input.size - 1] == '\\' || input.str[input.size - 1] == '/')) input.size -= 1;
    if (str8_ends_with(input, str8_lit("AddOns"), 1) && os_dir_exists(input)) return str8_copy(a, input);

    String8 tries[2];
    tries[0] = path_join(a, input, str8_lit("Interface\\AddOns"));
    tries[1] = str8_pushf(a, "%.*s\\%s\\Interface\\AddOns", S8F(input), flavor_folder ? flavor_folder : "_retail_");
    for (int i = 0; i < 2; i += 1) {
        if (os_dir_exists(tries[i])) return tries[i];
    }
    return str8(0, 0);
}

// The flavor folder's game client (Wow.exe, WowClassic.exe, ...) carries the game version.
static GameVersion wow_client_version(String8 addons_dir)
{
    GameVersion v = { 0 };
    Temp scratch = scratch_begin(0);
    String8 flavor_dir = path_dirname(path_dirname(addons_dir));
    OS_FileInfoArray files = os_list_dir(scratch.arena, flavor_dir);
    for (U64 i = 0; i < files.count && !v.valid; i += 1) {
        String8 n = files.v[i].name;
        if (files.v[i].is_dir || !str8_starts_with(n, str8_lit("Wow"), 1) || !str8_ends_with(n, str8_lit(".exe"), 1)) continue;
        if (str8_find(n, 0, str8_lit("Error"), 1) < n.size) continue;
        U32 parts[4];
        if (os_file_version(path_join(scratch.arena, flavor_dir, n), parts) && parts[0] + parts[1] > 0) {
            v.major = parts[0]; v.minor = parts[1]; v.patch = parts[2];
            v.valid = 1;
        }
    }
    scratch_end(scratch);
    return v;
}

//- .toc helpers

static String8 wow_strip_escapes(Arena *a, String8 s)
{
    U8 *out = push_array_nz(a, U8, s.size + 1);
    U64 n = 0;
    for (U64 i = 0; i < s.size; i += 1) {
        U8 c = s.str[i];
        if (c == '|' && i + 1 < s.size) {
            U8 k = s.str[i + 1];
            if ((k == 'c' || k == 'C') && i + 10 <= s.size) { i += 9; continue; }      // |cAARRGGBB
            if (k == 'r' || k == 'R') { i += 1; continue; }                          // |r
            if (k == 'T') {                                                          // |T...|t
                U64 end = str8_find(s, i + 2, str8_lit("|t"), 0);
                i = (end < s.size) ? end + 1 : s.size;
                continue;
            }
            if (k == '|') { out[n++] = '|'; i += 1; continue; }
        }
        out[n++] = c;
    }
    out[n] = 0;
    return str8_trim(str8(out, n));
}

static B32 toc_name_matches_folder(String8 file, String8 folder)
{
    static char *flavors[] = { "mainline", "bcc", "tbc", "classic", "vanilla", "wrath", "wotlkc", "cata", "mists", "forever", "camelot" };
    if (!str8_ends_with(file, str8_lit(".toc"), 1)) return 0;
    String8 stem = str8_chop(file, 4);
    if (str8_match(stem, folder, 1)) return 1;
    if (!str8_starts_with(stem, folder, 1) || stem.size < folder.size + 2) return 0;
    U8 sep = stem.str[folder.size];
    if (sep != '-' && sep != '_') return 0;
    String8 suffix = str8_skip(stem, folder.size + 1);
    for (int i = 0; i < (int)ArrayCount(flavors); i += 1) {
        if (str8_match(suffix, str8_cstring(flavors[i]), 1)) return 1;
    }
    return 0;
}

static String8 skip_bom(String8 s)
{
    if (s.size >= 3 && s.str[0] == 0xEF && s.str[1] == 0xBB && s.str[2] == 0xBF) return str8_skip(s, 3);
    return s;
}

static B32 list_has_name(String8List *l, String8 s)
{
    for (String8Node *n = l->first; n; n = n->next) if (str8_match(n->string, s, 1)) return 1;
    return 0;
}

static void addon_parse_toc(Arena *a, Addon *addon, String8 text)
{
    String8 rest = skip_bom(text);
    while (rest.size) {
        String8 line = str8_trim(str8_pop_line(&rest));
        if (!str8_starts_with(line, str8_lit("##"), 0)) continue;
        line = str8_skip(line, 2);
        U64 colon = str8_find(line, 0, str8_lit(":"), 0);
        if (colon == line.size) continue;
        String8 key = str8_trim(str8_prefix(line, colon));
        String8 value = str8_trim(str8_skip(line, colon + 1));
        String8 *dst = 0;
        // WoW treats any key starting with "Dep" (Dependencies, Dep1, ...) and RequiredDeps as hard dependencies.
        if (str8_starts_with(key, str8_lit("Dep"), 1) || str8_match(key, str8_lit("RequiredDeps"), 1)) {
            String8 deps = value;
            while (deps.size) {
                U64 comma = str8_find(deps, 0, str8_lit(","), 0);
                String8 dep = str8_trim(str8_prefix(deps, comma));
                deps = str8_skip(deps, comma + 1);
                if (dep.size && !list_has_name(&addon->required_deps, dep)) str8_list_push(a, &addon->required_deps, str8_copy(a, dep));
            }
            continue;
        }
        if      (str8_match(key, str8_lit("Title"), 1))              { addon->title = wow_strip_escapes(a, value); continue; }
        else if (str8_match(key, str8_lit("Version"), 1))            dst = &addon->version;
        else if (str8_match(key, str8_lit("Interface"), 1))          dst = &addon->interface_version;
        else if (str8_match(key, str8_lit("Author"), 1))             dst = &addon->author;
        else if (str8_match(key, str8_lit("Notes"), 1))              { addon->notes = wow_strip_escapes(a, value); continue; }
        else if (str8_match(key, str8_lit("X-Curse-Project-ID"), 1)) dst = &addon->curse_project_id;
        else if (str8_match(key, str8_lit("X-Wago-ID"), 1))          dst = &addon->wago_id;
        else if (str8_match(key, str8_lit("X-WoWI-ID"), 1))          dst = &addon->wowi_id;
        if (dst) *dst = str8_copy(a, value);
    }
}

// Reads the .toc metadata of one installed addon folder (no fingerprinting). Returns 0 if it has no .toc.
static B32 addon_folder_toc(Arena *a, String8 addons_dir, String8 folder, Addon *out)
{
    memset(out, 0, sizeof(*out));
    Temp scratch = scratch_begin(a);
    String8 dir = path_join(scratch.arena, addons_dir, folder);
    OS_FileInfoArray files = os_list_dir(scratch.arena, dir);
    B32 found = 0;
    // Prefer <Folder>.toc, else any flavor-specific one.
    for (int pass = 0; pass < 2 && !found; pass += 1) {
        for (U64 i = 0; i < files.count && !found; i += 1) {
            String8 n = files.v[i].name;
            if (files.v[i].is_dir || !toc_name_matches_folder(n, folder)) continue;
            if (pass == 0 && !str8_match(str8_chop(n, 4), folder, 1)) continue;
            addon_parse_toc(a, out, os_read_file(scratch.arena, path_join(scratch.arena, dir, n)));
            found = 1;
        }
    }
    scratch_end(scratch);
    return found;
}

// The cleaned-up ## Title of one installed addon folder (empty if none).
static String8 addon_folder_title(Arena *a, String8 addons_dir, String8 folder)
{
    Addon tmp;
    addon_folder_toc(a, addons_dir, folder, &tmp);
    return tmp.title;
}

//- Fingerprinting a folder
//
// Files that count: the folder's own .toc files and Bindings.xml, plus everything
// the .toc files include (.lua/.xml lines), plus everything those .xml files
// include via <Script file=".."/> and <Include file=".."/>, recursively.
// Each file is hashed; hashes are sorted, concatenated as decimal text, and hashed again.

static B32 list_contains_ci(String8List *l, String8 s)
{
    for (String8Node *n = l->first; n; n = n->next) if (str8_match(n->string, s, 1)) return 1;
    return 0;
}

static B32 include_path_ok(String8 p)
{
    return p.size && str8_find(p, 0, str8_lit(".."), 0) == p.size;
}

static void toc_collect_includes(Arena *a, String8List *queue, String8 dir, String8 text)
{
    String8 rest = skip_bom(text);
    while (rest.size) {
        String8 line = str8_pop_line(&rest);
        U64 hash = str8_find(line, 0, str8_lit("#"), 0);
        line = str8_trim(str8_prefix(line, hash));
        if (!include_path_ok(line)) continue;
        if (!str8_ends_with(line, str8_lit(".lua"), 1) && !str8_ends_with(line, str8_lit(".xml"), 1)) continue;
        str8_list_push(a, queue, path_join(a, dir, path_normalize_slashes(a, line)));
    }
}

static U64 skip_ws(String8 s, U64 i) { while (i < s.size && char_is_space(s.str[i])) i += 1; return i; }

static void xml_collect_includes(Arena *a, String8List *queue, String8 dir, String8 text)
{
    // Strip <!-- comments --> first.
    U8 *buf = push_array_nz(a, U8, text.size + 1);
    U64 n = 0;
    for (U64 i = 0; i < text.size;) {
        if (i + 4 <= text.size && memcmp(text.str + i, "<!--", 4) == 0) {
            U64 end = str8_find(text, i + 4, str8_lit("-->"), 0);
            i = (end < text.size) ? end + 3 : text.size;
            continue;
        }
        buf[n++] = text.str[i++];
    }
    String8 s = str8(buf, n);

    for (U64 i = 0; i < s.size; i += 1) {
        if (s.str[i] != '<') continue;
        U64 j = i + 1;
        String8 tail = str8_skip(s, j);
        if (str8_starts_with(tail, str8_lit("Include"), 1)) j += 7;
        else if (str8_starts_with(tail, str8_lit("Script"), 1)) j += 6;
        else continue;
        U64 k = skip_ws(s, j);
        if (k == j) continue; // need at least one space
        if (!str8_starts_with(str8_skip(s, k), str8_lit("file="), 1)) continue;
        k += 5;
        if (k >= s.size || (s.str[k] != '"' && s.str[k] != '\'')) continue;
        U8 quote = s.str[k++];
        U64 start = k;
        while (k < s.size && s.str[k] != quote && s.str[k] != '\n') k += 1;
        if (k >= s.size || s.str[k] != quote) continue;
        String8 file = str8_substr(s, start, k);
        k = skip_ws(s, k + 1);
        if (!str8_starts_with(str8_skip(s, k), str8_lit("/>"), 0)) continue;
        if (!include_path_ok(file)) continue;
        str8_list_push(a, queue, path_join(a, dir, path_normalize_slashes(a, file)));
        i = k + 1;
    }
}

static int u32_compare(const void *a, const void *b)
{
    U32 x = *(const U32 *)a, y = *(const U32 *)b;
    return (x > y) - (x < y);
}

typedef struct Md5Hex { char s[33]; } Md5Hex;
static int md5_str_compare(const void *a, const void *b) { return memcmp(a, b, 32); }

// CurseForge fingerprint of the folder; also writes the Wago fingerprint (same files, MD5) to wago_hash.
static U32 addon_folder_fingerprint(String8 addon_dir, String8 folder_name, OS_FileInfoArray files, U32 *out_file_count, char wago_hash[33])
{
    Temp scratch = scratch_begin(0);
    String8List matched = { 0 };
    String8List queue = { 0 };

    for (U64 i = 0; i < files.count; i += 1) {
        OS_FileInfo *f = &files.v[i];
        if (f->is_dir) continue;
        String8 full = path_join(scratch.arena, addon_dir, f->name);
        if (toc_name_matches_folder(f->name, folder_name)) str8_list_push(scratch.arena, &queue, full);
        else if (str8_match(f->name, str8_lit("Bindings.xml"), 1)) str8_list_push(scratch.arena, &matched, full);
    }

    while (queue.first) {
        String8 path = queue.first->string;
        queue.first = queue.first->next;
        if (!queue.first) queue.last = 0;

        if (list_contains_ci(&matched, path) || !os_file_exists(path)) continue;
        str8_list_push(scratch.arena, &matched, path);

        String8 ext = path_ext(path);
        B32 is_toc = str8_match(ext, str8_lit(".toc"), 1);
        B32 is_xml = str8_match(ext, str8_lit(".xml"), 1);
        if (!is_toc && !is_xml) continue;
        String8 text = os_read_file(scratch.arena, path);
        String8 dir = path_dirname(path);
        if (is_toc) toc_collect_includes(scratch.arena, &queue, dir, text);
        else        xml_collect_includes(scratch.arena, &queue, dir, text);
    }

    U32 *hashes = push_array(scratch.arena, U32, matched.count);
    Md5Hex *md5s = push_array(scratch.arena, Md5Hex, matched.count + 1);
    U64 count = 0;
    for (String8Node *n = matched.first; n; n = n->next) {
        Temp t = temp_begin(scratch.arena);
        String8 data = os_read_file(scratch.arena, n->string);
        U32 h = cf_fingerprint(data);
        md5_hex(data, md5s[count].s);
        temp_end(t);
        hashes[count++] = h;
    }
    qsort(hashes, count, sizeof(U32), u32_compare);

    // Wago: sorted lowercase hex digests, concatenated, MD5 again.
    qsort(md5s, count, sizeof(md5s[0]), md5_str_compare);
    U8 *cat = push_array_nz(scratch.arena, U8, count * 32 + 1);
    for (U64 i = 0; i < count; i += 1) memcpy(cat + i * 32, md5s[i].s, 32);
    md5_hex(str8(cat, count * 32), wago_hash);

    U8 *digits = push_array_nz(scratch.arena, U8, count * 10 + 1);
    U64 len = 0;
    for (U64 i = 0; i < count; i += 1) len += (U64)sprintf((char *)digits + len, "%u", hashes[i]);
    U32 result = cf_fingerprint(str8(digits, len));

    if (out_file_count) *out_file_count = (U32)count;
    scratch_end(scratch);
    return result;
}

//- Scanning

static int addon_compare(const void *a, const void *b)
{
    const Addon *x = (const Addon *)a, *y = (const Addon *)b;
    String8 s = x->title.size ? x->title : x->folder;
    String8 t = y->title.size ? y->title : y->folder;
    U64 n = Min(s.size, t.size);
    for (U64 i = 0; i < n; i += 1) {
        int d = (int)char_lower(s.str[i]) - (int)char_lower(t.str[i]);
        if (d) return d;
    }
    return (s.size > t.size) - (s.size < t.size);
}

static AddonArray addons_scan(Arena *a, String8 addons_dir)
{
    AddonArray result = { 0 };
    Temp scratch = scratch_begin(a);
    OS_FileInfoArray entries = os_list_dir(scratch.arena, addons_dir);

    U64 dir_count = 0;
    for (U64 i = 0; i < entries.count; i += 1) dir_count += entries.v[i].is_dir;
    result.v = push_array(a, Addon, dir_count);

    for (U64 i = 0; i < entries.count; i += 1) {
        OS_FileInfo *e = &entries.v[i];
        if (!e->is_dir || (e->name.size && e->name.str[0] == '.')) continue; // skip .wam-staging etc.
        Addon *addon = &result.v[result.count++];
        addon->folder = str8_copy(a, e->name);
        addon->path = path_join(a, addons_dir, e->name);

        Temp t = temp_begin(scratch.arena);
        OS_FileInfoArray files = os_list_dir(scratch.arena, addon->path);

        // Prefer <Folder>.toc for metadata, else the first flavor-specific one.
        OS_FileInfo *toc = 0;
        for (U64 j = 0; j < files.count; j += 1) {
            if (files.v[j].is_dir || !toc_name_matches_folder(files.v[j].name, e->name)) continue;
            if (!toc || str8_match(str8_chop(files.v[j].name, 4), e->name, 1)) toc = &files.v[j];
        }
        if (toc) {
            addon->toc_name = str8_copy(a, toc->name);
            String8 toc_path = path_join(scratch.arena, addon->path, toc->name);
            addon_parse_toc(a, addon, os_read_file(scratch.arena, toc_path));
        }
        temp_end(t);
    }

    qsort(result.v, result.count, sizeof(Addon), addon_compare);
    scratch_end(scratch);
    return result;
}

// Fingerprinting reads (and hashes twice) every file each .toc pulls in: by far the slowest part
// of a scan, and only needed to recognize addons. So it's a separate pass, spread over threads.
static void addon_fingerprint_one(void *user, U64 i)
{
    Addon *addon = &((AddonArray *)user)->v[i];
    Temp scratch = scratch_begin(0);
    OS_FileInfoArray files = os_list_dir(scratch.arena, addon->path);
    addon->fingerprint = addon_folder_fingerprint(addon->path, addon->folder, files, &addon->fingerprint_files, addon->wago_hash);
    scratch_end(scratch);
}

static void addons_fingerprint(AddonArray *scan)
{
    if (scan->fingerprinted) return;
    os_parallel_for(scan->count, addon_fingerprint_one, scan);
    scan->files_hashed = 0;
    for (U64 i = 0; i < scan->count; i += 1) scan->files_hashed += scan->v[i].fingerprint_files;
    scan->fingerprinted = 1;
}
