// gui/settings.c - the settings popup: a panel in the middle of the window over a dimmed backdrop,
// opened with the cog in the header (or Ctrl+,). It has what can be set (the Wago and
// CurseForge keys, the WoW folder, the theme) and the folders wam keeps things in.
// Esc, the close button or a click beside it closes it. Part of main_gui.c's unity build.

static void browse_for_root(void); // frame.c

static void settings_open(void)
{
    app.settings_open = 1;
    app.settings_fresh = 1;
    app.settings_note[0] = app.cf_note[0] = 0;
    app.settings_scroll = (UI_Scroll){ 0 };
    app.settings_in_cf = app.wago_from_env;
    ui_text_clear(&app.wago_entry);
    ui_text_clear(&app.cf_entry);
    wm_request_frame();
}

static void settings_close(void)
{
    app.settings_open = 0;
    ui_text_clear(&app.wago_entry); // they may hold a key
    ui_text_clear(&app.cf_entry);
    wm_request_frame();
}

static void settings_note(char *text, B32 is_error)
{
    copy_cstr(app.settings_note, sizeof(app.settings_note), str8_cstring(text));
    app.settings_note_error = is_error;
}

static void settings_cf_note(char *text, B32 is_error)
{
    copy_cstr(app.cf_note, sizeof(app.cf_note), str8_cstring(text));
    app.cf_note_error = is_error;
}

// Saves the typed Wago key (an empty field does nothing; Remove deletes the saved one).
static void settings_save_wago(B32 remove)
{
    String8 key = remove ? str8(0, 0) : str8_trim(ui_text(&app.wago_entry));
    if (!remove && !key.size) return;
    if (key.size && (!key_looks_valid(key) || key.size >= sizeof(app.wago_key))) {
        settings_note("That doesn't look like a Wago API key", 1);
    } else if (!key_save(WAGO_KEY_FILE, key)) {
        settings_note(remove ? "Could not delete the saved key" : "Could not save the key", 1);
    } else {
        os_secure_zero(app.wago_key, sizeof(app.wago_key));
        memcpy(app.wago_key, key.str, key.size);
        settings_note(key.size ? "Saved. Checking which installed addons are on Wago..." : "Removed the saved key", 0);
        if (key.size) push_task(TASK_SYNC, str8(0, 0));
        // (browse_order sees the key change: which source two-source rows install from)
    }
    ui_text_clear(&app.wago_entry);
}

//- The CurseForge key: tried before it's saved, so a mistyped one never replaces one that works

typedef struct CfKeyCheck { U32 id; U32 status; char key[API_KEY_MAX]; } CfKeyCheck;

static U32 cf_check_main(void *p)
{
    CfKeyCheck *c = (CfKeyCheck *)p;
    Arena *a = arena_alloc(MB(64));
    c->status = cf_check_key(a, str8_cstring(c->key)).status;
    arena_release(a);
    if (!wm_post(on_cf_key_checked, 0, c)) { os_secure_zero(c->key, sizeof(c->key)); os_free(c); }
    return 0;
}

// The key wam uses from now on (tasks already queued keep the one they were queued with).
static void settings_use_cf_key(String8 key, CfKeySource source)
{
    B32 had = app.cf_key[0] != 0;
    os_secure_zero(app.cf_key, sizeof(app.cf_key));
    copy_cstr(app.cf_key, sizeof(app.cf_key), key);
    app.cf_source = source;
    InterlockedExchange64(&cf_pause.until_ms, 0); // it was for the key before
    app.more_done = !key.size;
    app.sugg_last_query[0] = 0;                   // the same search may find more
    if (!key.size) return;
    push_task(TASK_SYNC, str8(0, 0));
    if (!had || app.catalog_failed) catalog_request(1);
}

// Tries the typed CurseForge key; settings_cf_checked saves it.
static void settings_save_cf(void)
{
    String8 key = str8_trim(ui_text(&app.cf_entry));
    if (!key.size) return;
    if (!key_looks_valid(key)) {
        settings_cf_note("That doesn't look like a CurseForge API key", 1);
    } else {
        CfKeyCheck *c = (CfKeyCheck *)os_alloc(sizeof(CfKeyCheck));
        if (!c) { settings_cf_note("Could not check the key", 1); return; }
        c->id = ++app.cf_check;
        copy_cstr(c->key, sizeof(c->key), key);
        app.cf_checking = 1;
        app.cf_note[0] = 0;
        os_thread_start(cf_check_main, c);
    }
    ui_text_clear(&app.cf_entry);
}

static void settings_cf_checked(CfKeyCheck *c)
{
    if (c->id == app.cf_check) {
        app.cf_checking = 0;
        B32 rejected = c->status == 401 || c->status == 403, accepted = c->status >= 200 && c->status < 300;
        if (rejected) {
            settings_cf_note("CurseForge rejected that key, so it wasn't saved", 1);
        } else if (!key_save(CF_KEY_FILE, str8_cstring(c->key))) {
            settings_cf_note("Could not save the key", 1);
        } else {
            settings_use_cf_key(str8_cstring(c->key), CF_KEY_SAVED);
            settings_cf_note(accepted ? "Saved. CurseForge accepts this key." : "Saved, but CurseForge couldn't be asked whether it works", 0);
        }
        // The popup was closed in the meantime: say it in the status line.
        if (!app.settings_open) worker_set_status(str8_cstring(app.cf_note), app.cf_note_error);
    }
    os_secure_zero(c->key, sizeof(c->key));
    os_free(c);
}

// Removes the saved key: back to the one built into this wam, if any.
static void settings_remove_cf(void)
{
    app.cf_check += 1; // an answer on its way doesn't bring one back
    app.cf_checking = 0;
    ui_text_clear(&app.cf_entry);
    if (!key_save(CF_KEY_FILE, str8(0, 0))) { settings_cf_note("Could not delete the saved key", 1); return; }
    Temp t = scratch_begin(0);
    CfKeySource source;
    String8 key = cf_api_key(t.arena, &source);
    settings_use_cf_key(key, source);
    settings_cf_note(key.size ? "Removed your key. Using the one built into wam." : "Removed the saved key", 0);
    scratch_end(t);
}

// Keys and typing while the popup is open (it takes them all): the key fields.
// Returns 1 if it was open.
static B32 settings_event(WM_Event *e)
{
    if (!app.settings_open) return 0;
    if (e->kind == WM_EVENT_KEY) {
        F32 page = 300 * ui.scale;
        if (e->key == WM_KEY_ESCAPE) { settings_close(); return 1; }
        if (e->key == WM_KEY_PAGE_DOWN) app.settings_scroll.target += page;
        if (e->key == WM_KEY_PAGE_UP) app.settings_scroll.target -= page;
    }
    B32 cf = app.settings_in_cf;
    if (cf ? app.cf_source == CF_KEY_ENV : app.wago_from_env) return 1; // (the field is off: the key comes from the environment)
    if (e->kind == WM_EVENT_KEY && e->key == WM_KEY_RETURN) { if (cf) settings_save_cf(); else settings_save_wago(0); }
    else if (ui_text_edit(cf ? &app.cf_entry : &app.wago_entry, e)) (cf ? app.cf_note : app.settings_note)[0] = 0;
    return 1;
}

// A key's field with Save, and Remove when one is saved. Sets *save / *remove when they're clicked;
// returns the y below it.
static F32 settings_key_row(F32 x, F32 y, F32 w, char *scope, UI_TextBuf *entry, B32 is_cf, B32 saved, String8 hint, B32 *save, B32 *remove)
{
    F32 s = ui.scale;
    ui_scope_push(str8_cstring(scope));
    String8 sl = str8_lit("Save"), rl = str8_lit("Remove");
    F32 sw = ui_button_width(sl), rw = saved ? ui_button_width(rl) + 8 * s : 0;
    Rect field = rect(x, y, x + w - sw - rw - 8 * s, y + 34 * s);
    UI_Id id = ui_id(str8_lit("key field"));
    UI_Signal sg = ui_signal(id, field, UI_FOCUSABLE);
    if (sg.focused || (sg.clicked && ui_mouse_in(ui.clip))) app.settings_in_cf = is_cf; // Tab or a click: typing goes here
    if (sg.clicked && sg.keyboard) *save = 1;                                           // Enter on it
    B32 here = app.settings_in_cf == is_cf;
    ui_text_field(field, entry, hint, 1, here, here ? C_ACCENT : C_BORDER); // the key is never drawn
    ui_focus_ring(id, field, 6 * s);
    Rect sb = rect(field.x1 + 8 * s, y + 3 * s, field.x1 + 8 * s + sw, y + 31 * s);
    if (ui_button(sb, sl, entry->len ? C_TEXT : C_DIM, entry->len ? C_ACCENT_BG : C_BUTTON, entry->len ? C_ACCENT : C_BUTTON_HI)) *save = 1;
    if (saved && ui_button(rect(sb.x1 + 8 * s, sb.y0, sb.x1 + rw, sb.y1), rl, C_TEXT, C_BUTTON, C_BUTTON_HI)) *remove = 1;
    ui_scope_pop();
    return field.y1 + 8 * s;
}

// A section heading and its explanation; returns the y below them.
static F32 settings_heading(F32 x, F32 y, F32 w, char *title, char *about)
{
    F32 s = ui.scale;
    f_draw_text(ui.font.bold, x, y, str8_cstring(title), ui.theme.heading, 0);
    y += ui.font.bold->line_height + 4 * s;
    if (about) y += ui_text_wrapped(ui.font.caption, x, y, w, str8_cstring(about), C_DIM, 0, 1e9f) + 8 * s;
    return y;
}

static B32 settings_folder_button(F32 *x, F32 y, char *label, String8 path)
{
    if (!path.size || !os_dir_exists(path)) return 0;
    String8 l = str8_cstring(label);
    F32 bw = ui_button_width(l);
    Rect b = rect(*x, y, *x + bw, y + 28 * ui.scale);
    *x = b.x1 + 8 * ui.scale;
    if (ui_button(b, l, C_TEXT, C_BUTTON, C_BUTTON_HI)) os_shell_open_async(path, str8(0, 0));
    return 1;
}

// One theme in the picker, drawn in its own colors: its panel and glow, its emblem, its name in its
// heading color and a strip from its accent to its spark. True when clicked.
static B32 settings_theme_card(Rect r, ThemeInfo *info, B32 chosen)
{
    F32 s = ui.scale, px = ui_px(), radius = 7 * s;
    UI_Theme t = info->make();
    UI_Id id = ui_id(str8_cstring(info->key));
    UI_Signal sg = ui_signal(id, r, UI_FOCUSABLE);
    B32 in_clip = ui_mouse_in(ui.clip);
    F32 hot = ui_anim(id, (F32)(sg.hovering && in_clip));

    r_rect_gradient(r, ui_mix(t.panel, t.hover, 0.6f + 0.4f * hot), t.bg, radius, 0, 0.5f, 0);
    if (t.ornate > 0 && t.aura.w > 0) {
        V4 none = t.aura;
        none.w = 0;
        r_rect_gradient(r, ui_fade(t.aura, 2.5f), none, radius, 0, 0.5f, 0);
    }
    Rect strip = rect(r.x0 + 1 * s, r.y1 - 5 * s, r.x1 - 1 * s, r.y1 - 1 * s);
    r_rect_gradient(strip, t.accent, t.ornate > 0 ? t.spark : t.accent_hi, 2 * s, 0, 0.5f, 1);
    V4 rim = chosen ? ui.theme.accent_hi : ui_mix(t.ornate > 0 ? t.trim : t.border_hi, t.trim_hi, hot);
    r_rect(r, rim, radius, chosen ? 2 * s : px, 0.5f);

    F32 cy = r.y0 + (r.y1 - r.y0 - 5 * s) * 0.5f;
    ui_emblem(&t, r.x0 + 16 * s, cy, 20 * s);
    String8 name = str8_cstring(info->name);
    f_draw_text(ui.font.caption, r.x0 + 31 * s, cy - ui.font.caption->line_height * 0.5f, name, chosen ? t.heading : ui_mix(t.dim, t.heading, hot), r.x1 - r.x0 - 34 * s);

    if (sg.hovering && in_clip) ui_tooltip(r, str8_cstring(info->about));
    ui_focus_ring(id, r, radius);
    return sg.clicked && (sg.keyboard || in_clip);
}

// The theme picker: a grid of cards. Picking one applies it at once and saves it. Returns the y below it.
static F32 settings_themes(F32 x, F32 y, F32 w)
{
    F32 s = ui.scale, gap = 8 * s, ch = 44 * s;
    int cols = 5;
    F32 cw = (w - gap * (cols - 1)) / cols;
    for (int i = 0; i < THEME_COUNT; i += 1) {
        F32 cx = x + (i % cols) * (cw + gap), cy = y + (i / cols) * (ch + gap);
        Rect r = rect((F32)(int)cx, (F32)(int)cy, (F32)(int)(cx + cw), (F32)(int)(cy + ch));
        if (settings_theme_card(r, &themes[i], i == theme_current) && i != theme_current) {
            theme_apply(i);
            wam_save_theme(str8_cstring(themes[i].key));
        }
    }
    int rows = (THEME_COUNT + cols - 1) / cols;
    return y + rows * ch + (rows - 1) * gap;
}

static void draw_settings(void)
{
    F32 s = ui.scale;
    Arena *fa = ui.frame_arena;
    Rect panel = ui_modal_begin(580 * s, app.settings_h > 0 ? app.settings_h : 480 * s, &app.settings_fresh);
    F32 pad = 24 * s, x = panel.x0 + pad, w = (panel.x1 - panel.x0) - 2 * pad, y = panel.y0 + 18 * s;

    // Title and close
    f_draw_text(ui.font.title, x, y, str8_lit("Settings"), ui.theme.heading, 0);
    B32 close = ui_modal_close_button(panel);
    y += ui.font.title->line_height + 16 * s;

    // The rest scrolls when the window is too low for it.
    Rect body = rect(panel.x0 + 1 * s, y, panel.x1 - 1 * s, panel.y1 - 1 * s);
    F32 header_h = y - panel.y0;
    F32 content_h = app.settings_h > 0 ? app.settings_h - header_h : 0;
    if (ui_scroll_list(&app.settings_scroll, body, content_h)) wm_request_frame();
    ui.clip = body;
    r_set_clip(body);
    y = (F32)(int)(y - app.settings_scroll.pos);
    F32 top = y;

    //- Wago
    y = settings_heading(x, y, w, "Wago Addons",
                         "Searching and installing from Wago needs your own API key, which comes with a Wago subscription. "
                         "Copy it from your account on addons.wago.io.");
    if (app.wago_from_env) {
        f_draw_text(ui.font.normal, x, y, str8_lit("Set by the WAGO_API_KEY environment variable"), C_GREEN, w);
        y += ui.font.normal->line_height + 4 * s;
    } else {
        B32 save = 0, remove = 0;
        String8 hint = app.wago_key[0] ? str8_lit("Paste a new key to replace the saved one") : str8_lit("Paste your Wago API key (Ctrl+V)");
        y = settings_key_row(x, y, w, "wago", &app.wago_entry, 0, app.wago_key[0] != 0, hint, &save, &remove);
        if (remove) settings_save_wago(1);
        else if (save) settings_save_wago(0);
        String8 state = app.settings_note[0] ? str8_cstring(app.settings_note)
                      : app.wago_key[0] ? str8_lit("\xE2\x9C\x93 A key is saved, encrypted for your Windows user")
                      : str8_lit("No key saved: Wago addons are listed, but can't be installed or updated");
        V4 color = app.settings_note[0] ? (app.settings_note_error ? C_ERROR : C_GREEN) : app.wago_key[0] ? C_GREEN : C_FAINT;
        f_draw_text(ui.font.caption, x, y, state, color, w);
        y += ui.font.caption->line_height;
    }
    y += 22 * s;

    //- CurseForge
    y = settings_heading(x, y, w, "CurseForge",
                         wam_cf_key_len ? "wam comes with a key, so this is optional: your own is used instead when you add one, "
                                               "e.g. if the built-in one stops working. Keys come from console.curseforge.com."
                                             : "This build has no CurseForge key. Add your own, from console.curseforge.com.");
    if (app.cf_source == CF_KEY_ENV) {
        f_draw_text(ui.font.normal, x, y, str8_lit("Set by the CURSEFORGE_API_KEY environment variable"), C_GREEN, w);
        y += ui.font.normal->line_height + 4 * s;
    } else {
        B32 save = 0, remove = 0, saved = app.cf_source == CF_KEY_SAVED;
        String8 hint = saved ? str8_lit("Paste a new key to replace the saved one") : str8_lit("Paste your CurseForge API key (Ctrl+V)");
        y = settings_key_row(x, y, w, "curseforge", &app.cf_entry, 1, saved, hint, &save, &remove);
        if (remove) settings_remove_cf();
        else if (save) settings_save_cf();
        String8 state = app.cf_checking ? str8_lit("Checking the key with CurseForge...")
                      : app.cf_note[0] ? str8_cstring(app.cf_note)
                      : saved ? str8_lit("\xE2\x9C\x93 Using your key, encrypted for your Windows user")
                      : app.cf_key[0] ? str8_lit("Using the key built into wam. Nothing to set up.")
                      : str8_lit("No key saved: CurseForge addons can't be found, installed or updated");
        V4 color = app.cf_checking ? C_DIM : app.cf_note[0] ? (app.cf_note_error ? C_ERROR : C_GREEN) : saved ? C_GREEN : C_FAINT;
        f_draw_text(ui.font.caption, x, y, state, color, w);
        y += ui.font.caption->line_height;
    }
    y += 22 * s;

    //- World of Warcraft
    y = settings_heading(x, y, w, "World of Warcraft", 0);
    {
        String8 change = str8_lit("Change...");
        F32 bw = ui_button_width(change);
        String8 path = app.wow_root.size ? app.wow_root : str8_lit("No folder chosen");
        f_draw_text(ui.font.normal, x, y + (28 * s - ui.font.normal->line_height) * 0.5f, path, app.wow_root.size ? C_TEXT : C_FAINT, w - bw - 16 * s);
        if (ui_button(rect(x + w - bw, y, x + w, y + 28 * s), change, C_TEXT, C_BUTTON, C_BUTTON_HI)) wm_defer(browse_for_root);
        y += 28 * s + 22 * s;
    }

    //- Appearance
    {
        // On the heading's line, at the right: the status line's performance numbers.
        String8 label = str8_lit("Performance numbers in the status line");
        F32 lw = f_text_width(ui.font.caption, label), lx = x + w - lw, cy = y + ui.font.bold->line_height * 0.5f;
        B32 toggle = ui_checkbox(str8_lit("show performance"), lx - 22 * s, cy, app.show_perf);
        f_draw_text(ui.font.caption, lx, cy - ui.font.caption->line_height * 0.5f, label, C_DIM, 0);
        if (ui_clicked(rect(lx, cy - 10 * s, lx + lw, cy + 10 * s))) toggle = 1;
        if (toggle) {
            app.show_perf = !app.show_perf;
            wam_save_show_perf(app.show_perf);
        }
    }
    y = settings_heading(x, y, w, "Appearance", 0);
    y = settings_themes(x, y, w) + 22 * s;

    //- Folders
    y = settings_heading(x, y, w, "Folders", "Where wam keeps its settings, its caches, and the backups of your addon settings.");
    {
        F32 bx = x;
        String8 appdata = os_get_env(fa, "APPDATA"), local = os_get_env(fa, "LOCALAPPDATA");
        settings_folder_button(&bx, y, "Settings", appdata.size ? path_join(fa, appdata, str8_lit("wam")) : appdata);
        settings_folder_button(&bx, y, "Caches", local.size ? path_join(fa, local, str8_lit("wam")) : local);
        String8 backups = app.snap ? path_join(fa, path_dirname(path_dirname(app.snap->addons_dir)), str8_lit("WTF-Backup")) : str8(0, 0);
        if (!settings_folder_button(&bx, y, "Settings backups", backups) && app.snap) {
            f_draw_text(ui.font.caption, bx, y + (28 * s - ui.font.caption->line_height) * 0.5f, str8_lit("No settings backups yet"), C_FAINT, 0);
        }
        y += 28 * s;
    }
    y += 22 * s;

    //- About: the version, and where new ones and problems go
    y = settings_heading(x, y, w, "About", 0);
    {
        String8 releases = str8_lit("Releases"), issues = str8_lit("Report a problem");
        F32 iw = ui_button_width(issues), rw = ui_button_width(releases);
        f_draw_text(ui.font.normal, x, y + (28 * s - ui.font.normal->line_height) * 0.5f, str8_lit("wam " WAM_VERSION), C_TEXT, w - iw - rw - 32 * s);
        Rect ib = rect(x + w - iw, y, x + w, y + 28 * s), rb = rect(ib.x0 - 8 * s - rw, y, ib.x0 - 8 * s, y + 28 * s);
        if (ui_button(rb, releases, C_TEXT, C_BUTTON, C_BUTTON_HI)) os_open_url(str8_lit(WAM_PUBLIC_URL "/releases"));
        if (ui_button(ib, issues, C_TEXT, C_BUTTON, C_BUTTON_HI)) os_open_url(str8_lit(WAM_PUBLIC_URL "/issues"));
        y += 28 * s;
    }
    y += 24 * s;
    if (app.settings_h != header_h + y - top) { app.settings_h = header_h + y - top; wm_request_frame(); } // the next frame sizes the panel to it
    ui_scrollbar(&app.settings_scroll, body, y - top);
    if (ui_modal_end(panel) || close) settings_close();
}
