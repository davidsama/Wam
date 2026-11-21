// gui/session.c - where the user left off, so wam reopens the same way: the view, the flavor, the
// Get addons search, its sorts and filters, and the selected row. Saved on exit to
// %APPDATA%\wam\session.json, next to settings.json. Part of main_gui.c's unity build.
//
// Everything is stored by name or CurseForge id rather than by index, so a later build that
// reorders a list still reads it; anything unknown is ignored.

static char *session_view_names[VIEW_COUNT] = { "installed", "search", "downloads" };
static char *session_sort_names[SORT_COUNT] = { "best", "downloads", "updated", "new" };

static String8 session_path(Arena *a)
{
    String8 settings = wam_settings_path(a);
    return settings.size ? path_join(a, path_dirname(settings), str8_lit("session.json")) : settings;
}

static S32 session_name_index(char **names, S32 count, String8 name)
{
    for (S32 i = 0; i < count; i += 1) if (str8_match(name, str8_cstring(names[i]), 1)) return i;
    return -1;
}

static JSON *session_json(Arena *a)
{
    String8 path = session_path(a);
    return path.size ? json_parse(a, os_read_file(a, path)) : &json_nil;
}

// Before the first frame: the flavor, if this install still has it (call after detect_flavors).
static void session_restore_flavor(void)
{
    Temp t = temp_begin(app.arena);
    String8 folder = json_str(json_get(session_json(t.arena), "flavor"));
    int f = folder.size ? wow_flavor_index((char *)str8_copy(t.arena, folder).str) : -1;
    if (f >= 0 && f < FLAVOR_COUNT && app.flavor_exists[f]) app.flavor = f;
    temp_end(t);
}

// The rest, once the window is up and the catalog has been reset for the flavor.
static void session_restore(void)
{
    Temp t = temp_begin(app.arena);
    JSON *j = session_json(t.arena);

    S32 v = session_name_index(session_view_names, VIEW_COUNT, json_str(json_get(j, "view")));
    if (v >= 0) app.view = (View)v;
    S32 sb = session_name_index(session_sort_names, SORT_COUNT, json_str(json_get(j, "sortBrowse")));
    if (sb > SORT_BEST) app.sort_browse = (BrowseSort)sb; // Best match needs a query
    S32 sq = session_name_index(session_sort_names, SORT_COUNT, json_str(json_get(j, "sortQuery")));
    if (sq >= 0) app.sort_query = (BrowseSort)sq;

    app.src_hidden = 0;
    json_each(it, json_get(j, "hiddenSources")) {
        S32 src = session_name_index(source_names, SRC_COUNT, json_str(it));
        if (src >= 0) app.src_hidden |= 1u << src;
    }
    app.hide_installed = json_bool(json_get(j, "hideInstalled"), 0);

    S64 cat_id = json_int(json_get(j, "category")), sub_id = json_int(json_get(j, "subcategory"));
    for (U32 i = 0; i < CAT_SUBCATEGORY_COUNT && sub_id; i += 1) {
        if (cat_subcategories[i].cf_id == sub_id) { app.subcategory = (S32)i; app.category = cat_sub_parent(i); }
    }
    for (U32 i = 0; i < CAT_CATEGORY_COUNT && cat_id && app.subcategory < 0; i += 1) {
        if (cat_categories[i].cf_id == cat_id) app.category = (S32)i;
    }

    String8 q = json_str(json_get(j, "query"));
    if (q.size && q.size < sizeof(app.query.s)) {
        memcpy(app.query.s, q.str, q.size);
        app.query.len = (U32)q.size;
        query_changed(); // asks the network for it too
    }
    String8 sel = json_str(json_get(j, "selected"));
    if (sel.size < sizeof(app.sel_restore)) copy_cstr(app.sel_restore, sizeof(app.sel_restore), sel);
    temp_end(t);
}

static void session_save(void)
{
    Temp t = temp_begin(app.arena);
    String8 path = session_path(t.arena);
    if (!path.size) { temp_end(t); return; }
    String8List l = { 0 };
    str8_list_push(t.arena, &l, str8_pushf(t.arena, "  \"view\": \"%s\"", session_view_names[app.view]));
    str8_list_push(t.arena, &l, str8_pushf(t.arena, "  \"flavor\": \"%s\"", wow_flavors[app.flavor].folder));
    String8 q = browse_query(); // empty while the field takes a Wago key: that's never written here
    if (q.size) str8_list_push(t.arena, &l, str8_pushf(t.arena, "  \"query\": \"%.*s\"", S8F(json_escape(t.arena, q))));
    str8_list_push(t.arena, &l, str8_pushf(t.arena, "  \"sortBrowse\": \"%s\"", session_sort_names[app.sort_browse]));
    str8_list_push(t.arena, &l, str8_pushf(t.arena, "  \"sortQuery\": \"%s\"", session_sort_names[app.sort_query]));
    String8List hidden = { 0 };
    for (S32 src = 0; src < SRC_COUNT; src += 1) {
        if (app.src_hidden & (1u << src)) str8_list_push(t.arena, &hidden, str8_pushf(t.arena, "\"%s\"", source_names[src]));
    }
    str8_list_push(t.arena, &l, str8_pushf(t.arena, "  \"hiddenSources\": [%.*s]", S8F(str8_list_join(t.arena, &hidden, str8_lit(", ")))));
    str8_list_push(t.arena, &l, str8_pushf(t.arena, "  \"hideInstalled\": %s", app.hide_installed ? "true" : "false"));
    if (app.subcategory >= 0) str8_list_push(t.arena, &l, str8_pushf(t.arena, "  \"subcategory\": %d", cat_subcategories[app.subcategory].cf_id));
    else if (app.category >= 0) str8_list_push(t.arena, &l, str8_pushf(t.arena, "  \"category\": %d", cat_categories[app.category].cf_id));
    char *sel = app.sel_id[0] ? app.sel_id : app.sel_restore; // not yet back in the list: keep it for next time
    if (sel[0]) str8_list_push(t.arena, &l, str8_pushf(t.arena, "  \"selected\": \"%.*s\"", S8F(json_escape(t.arena, str8_cstring(sel)))));
    String8 body = str8_pushf(t.arena, "{\n%.*s\n}\n", S8F(str8_list_join(t.arena, &l, str8_lit(",\n"))));
    if (os_make_dirs(path_dirname(path))) os_write_file(path, body);
    temp_end(t);
}
