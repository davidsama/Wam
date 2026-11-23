// gui/settings.c - the settings popup: a panel in the middle of the window over a dimmed backdrop,
// opened with the cog in the header (or Ctrl+,). It has what can be set (the Wago key, the WoW
// folder), what's in use (the CurseForge key), and the folders wam keeps things in.
// Esc, the close button or a click beside it closes it. Part of main_gui.c's unity build.

static void browse_for_root(void); // frame.c

static void settings_open(void)
{
    app.settings_open = 1;
    app.settings_fresh = 1;
    app.settings_note[0] = 0;
    ui_text_clear(&app.wago_entry);
    wm_request_frame();
}

static void settings_close(void)
{
    app.settings_open = 0;
    ui_text_clear(&app.wago_entry); // it may hold a key
    wm_request_frame();
}

static void settings_note(char *text, B32 is_error)
{
    copy_cstr(app.settings_note, sizeof(app.settings_note), str8_cstring(text));
    app.settings_note_error = is_error;
}

// Saves the typed Wago key (an empty field does nothing; Remove deletes the saved one).
static void settings_save_wago(B32 remove)
{
    String8 key = remove ? str8(0, 0) : str8_trim(ui_text(&app.wago_entry));
    if (!remove && !key.size) return;
    if (key.size && (!wago_key_looks_valid(key) || key.size >= sizeof(app.wago_key))) {
        settings_note("That doesn't look like a Wago API key", 1);
    } else if (!wago_save_key(key)) {
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

// Keys and typing while the popup is open (it takes them all): the Wago key field.
// Returns 1 if it was open.
static B32 settings_event(WM_Event *e)
{
    if (!app.settings_open) return 0;
    if (e->kind == WM_EVENT_KEY && e->key == WM_KEY_ESCAPE) { settings_close(); return 1; }
    if (app.wago_from_env) return 1; // (the field is off: the key comes from the environment)
    if (e->kind == WM_EVENT_KEY && e->key == WM_KEY_RETURN) settings_save_wago(0);
    else if (ui_text_edit(&app.wago_entry, e)) app.settings_note[0] = 0;
    return 1;
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

    //- Wago
    y = settings_heading(x, y, w, "Wago Addons",
                         "Searching and installing from Wago needs your own API key, which comes with a Wago subscription. "
                         "Copy it from your account on addons.wago.io.");
    if (app.wago_from_env) {
        f_draw_text(ui.font.normal, x, y, str8_lit("Set by the WAGO_API_KEY environment variable"), C_GREEN, w);
        y += ui.font.normal->line_height + 4 * s;
    } else {
        String8 save = str8_lit("Save"), remove = str8_lit("Remove");
        F32 sw = ui_button_width(save), rw = app.wago_key[0] ? ui_button_width(remove) + 8 * s : 0;
        Rect field = rect(x, y, x + w - sw - rw - 8 * s, y + 34 * s);
        String8 hint = app.wago_key[0] ? str8_lit("Paste a new key to replace the saved one") : str8_lit("Paste your Wago API key (Ctrl+V)");
        ui_text_field(field, &app.wago_entry, hint, 1, app.wago_entry.len ? C_ACCENT : C_BORDER); // the key is never drawn
        Rect sb = rect(field.x1 + 8 * s, y + 3 * s, field.x1 + 8 * s + sw, y + 31 * s);
        if (ui_button(sb, save, app.wago_entry.len ? C_TEXT : C_DIM, app.wago_entry.len ? C_ACCENT_BG : C_BUTTON, app.wago_entry.len ? C_ACCENT : C_BUTTON_HI)) settings_save_wago(0);
        if (app.wago_key[0]) {
            Rect rb = rect(sb.x1 + 8 * s, sb.y0, sb.x1 + 8 * s + rw - 8 * s, sb.y1);
            if (ui_button(rb, remove, C_TEXT, C_BUTTON, C_BUTTON_HI)) settings_save_wago(1);
        }
        y = field.y1 + 8 * s;
        String8 state = app.settings_note[0] ? str8_cstring(app.settings_note)
                      : app.wago_key[0] ? str8_lit("\xE2\x9C\x93 A key is saved, encrypted for your Windows user")
                      : str8_lit("No key saved: Wago addons are listed, but can't be installed or updated");
        V4 color = app.settings_note[0] ? (app.settings_note_error ? C_ERROR : C_GREEN) : app.wago_key[0] ? C_GREEN : C_FAINT;
        f_draw_text(ui.font.caption, x, y, state, color, w);
        y += ui.font.caption->line_height;
    }
    y += 22 * s;

    //- CurseForge
    y = settings_heading(x, y, w, "CurseForge", 0);
    String8 cf = !app.api_key.size ? str8_lit("This build has no CurseForge key. Your own works when set in the CURSEFORGE_API_KEY environment variable.")
               : cf_key_is_users ? str8_lit("Using your key from the CURSEFORGE_API_KEY environment variable.")
               : str8_lit("Using the key built into wam. Nothing to set up.");
    y += ui_text_wrapped(ui.font.caption, x, y, w, cf, C_DIM, 0, 1e9f) + 22 * s;

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
    app.settings_h = y - panel.y0; // the next frame sizes the panel to it
    if (ui_modal_end(panel) || close) settings_close();
}
