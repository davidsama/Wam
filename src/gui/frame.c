// gui/frame.c - the window's composition: header (flavors, install picker), toolbar (view tabs and
// actions), the remove confirmation bar, the content panel with the current view, and the status bar.
// Also switching flavors and installs. Part of main_gui.c's unity build.

static void app_event(WM_Event *e); // main_gui.c: keys and typing

//- Frame

static void switch_flavor(int f)
{
    app.flavor = f;
    app.snap = app.snaps[f]; // the last listing of that flavor, if any, right away; a fresh one follows
    for (int v = 0; v < VIEW_COUNT; v += 1) app.scroll[v].pos = app.scroll[v].target = 0;
    if (app.downloads) { arena_release(app.downloads->arena); app.downloads = 0; } // installed state differs per flavor
    keyset_clear(&app.selected);
    keyset_clear(&app.removing);
    os_mutex_lock(&status_lock); // the last message was about the other flavor
    status_text[0] = status_url[0] = 0;
    status_is_error = 0;
    os_mutex_unlock(&status_lock);
    push_task(TASK_REFRESH, str8(0, 0));
    catalog_reset(); // availability differs per game version; loads this flavor's catalog
}

// Which flavors this install has, and the default: Forever, else Retail, else the first one.
static void detect_flavors(void)
{
    int first_existing = -1;
    for (int f = 0; f < FLAVOR_COUNT; f += 1) {
        Temp t = temp_begin(ui.frame_arena);
        app.flavor_exists[f] = wow_resolve_addons_dir(t.arena, app.wow_root, wow_flavors[f].folder).size > 0;
        temp_end(t);
        if (app.flavor_exists[f] && first_existing < 0) first_existing = f;
    }
    int forever = wow_flavor_index("_forever_"), retail = wow_flavor_index("_retail_");
    app.flavor = app.flavor_exists[forever] ? forever : app.flavor_exists[retail] ? retail : Max(first_existing, 0);
}

static void set_wow_root(String8 root)
{
    if (str8_match(root, app.wow_root, 1)) return;
    app.wow_root = str8_copy(app.arena, root);
    app.root_gen += 1; // anything still running for the old install is dropped when it arrives
    detect_flavors();
    for (int f = 0; f < FLAVOR_COUNT; f += 1) if (app.snaps[f]) { arena_release(app.snaps[f]->arena); app.snaps[f] = 0; }
    app.snap = 0;
    switch_flavor(app.flavor); // clears search, downloads, selection and status; queues a refresh
    push_task(TASK_SYNC, str8(0, 0));
    app.dl_scan_queued = 0;
    request_downloads_scan();
    if (!wam_save_root(root)) worker_set_status(str8_lit("Switched, but could not save the choice for next time"), 1);
}

static void pick_root_path(String8 path)
{
    String8 root = wow_normalize_root(ui.frame_arena, path);
    if (!root.size) {
        worker_set_status(str8_pushf(ui.frame_arena, "%.*s isn't a World of Warcraft folder (no _retail_, _classic_ or other game folders in it)", S8F(path)), 1);
        return;
    }
    set_wow_root(root);
}

// Windows' folder picker (deferred: it runs its own message loop).
static void browse_for_root(void)
{
    String8 test_pick = os_get_env(ui.frame_arena, "WAM_TEST_PICK"); // testing: skip the dialog, "pick" this folder
    if (test_pick.size) { pick_root_path(test_pick); return; }
    String8 path = wm_pick_folder(ui.frame_arena, str8_lit("Select your World of Warcraft folder"));
    if (path.size) pick_root_path(path);
}

// The installs wam found, plus Browse (deferred: a native menu runs its own message loop too).
static void show_root_menu(void)
{
    Arena *a = ui.frame_arena;
    String8List roots = wow_find_roots(a);
    if (app.wow_root.size) {
        B32 listed = 0;
        for (String8Node *n = roots.first; n; n = n->next) listed |= str8_match(n->string, app.wow_root, 1);
        if (!listed) str8_list_push(a, &roots, app.wow_root);
    }
    String8 *items = push_array(a, String8, roots.count + 2);
    U32 n = 0;
    S32 checked = -1;
    for (String8Node *r = roots.first; r; r = r->next) {
        if (str8_match(r->string, app.wow_root, 1)) checked = (S32)n;
        items[n++] = r->string;
    }
    U32 root_count = n;
    if (root_count) items[n++] = str8_lit("-");
    items[n++] = str8_lit("Browse for another folder...");
    S32 pick = wm_popup_menu(app.root_btn, items, n, checked);
    if (pick == (S32)n - 1) browse_for_root();
    else if (pick >= 0 && (U32)pick < root_count) set_wow_root(items[pick]);
}

// One frame of wam's window (the UI library has begun it and ends it).
static B32 app_frame(void)
{
    Arena *fa = ui.frame_arena;
    F32 s = ui.scale;
    F32 W = ui.width, H = ui.height;
    // Keys and typing since the last frame first, so this frame shows what they did.
    for (U32 i = 0; i < ui.event_count; i += 1) app_event(&ui.events[i]);
    Snapshot *snap = app.snap;
    B32 animating = 0;
    // The picture viewer covers the window: nothing under it sees the mouse.
    if (app.viewer_open && app.view != VIEW_SEARCH) app.viewer_open = 0;
    // So does the settings popup (its backdrop covers the window).
    ui_overlay_set(rect(0, 0, W, H), app.viewer_open || app.settings_open || app.changes_open);

    //- Header: title + flavor tabs
    F32 header_h = 56 * s;
    r_rect_gradient(rect(0, 0, W, header_h), ui_mix(C_PANEL, C_HOVER, 0.6f), C_PANEL, 0, 0, 0, 0);
    // The header is the window's title bar: what isn't a widget in it drags the window.
    wm_caption(app.viewer_open || app.settings_open || app.changes_open ? 0 : header_h);
    // What fits: the flavor tabs always do. When the window is narrow, the name goes first (the logo
    // stays), then the game version, and then the install button shrinks to a folder icon.
    F32 tabs_w = 0;
    for (int f = 0; f < FLAVOR_COUNT; f += 1) {
        if (app.flavor_exists[f]) tabs_w += ui_pill_tab_width(str8_cstring(wow_flavors[f].label)) + 6 * s;
    }
    String8 title = str8_lit("WoW Addons");
    String8 gv = snap && snap->client.valid ? str8_pushf(fa, "game %u.%u.%u", snap->client.major, snap->client.minor, snap->client.patch) : str8(0, 0);
    String8 root_label = app.wow_root.size ? str8_pushf(fa, "%.*s  \xE2\x96\xBE", S8F(app.wow_root)) : str8_lit("Choose WoW folder  \xE2\x96\xBE");
    F32 hright = ui_window_buttons(W, header_h) - 16 * s;
    // The settings cog, right of the install button.
    {
        F32 bh = 28 * s;
        Rect b = rect(hright - bh, (header_h - bh) * 0.5f, hright, (header_h + bh) * 0.5f);
        B32 hot = ui_mouse_in(b);
        wm_caption_hole(b);
        ui_plate(b, hot ? C_BUTTON_HI : C_BUTTON, 6 * s);
        ui_cog((b.x0 + b.x1) * 0.5f, (b.y0 + b.y1) * 0.5f, 8 * s, hot ? 0.26f : 0, hot ? C_TEXT : C_DIM); // turns a little on hover
        if (ui_clicked(b)) settings_open();
        hright = b.x0 - 8 * s;
    }
    F32 logo = 32 * s, title_w = f_text_width(ui.font.title, title) + 24 * s;
    F32 gv_w = gv.size ? f_text_width(ui.font.caption, gv) + 16 * s : 0;
    F32 root_min = Min(120 * s, f_text_width(ui.font.caption, root_label) + 24 * s), root_icon = 46 * s;
    F32 room = hright - (16 * s + logo + 10 * s) - tabs_w - 18 * s; // what's left for the name, version and install
    B32 show_title = room >= title_w + gv_w + root_min;
    B32 show_gv = gv.size && room - (show_title ? title_w : 0) >= gv_w + root_min;
    B32 root_as_icon = room - (show_title ? title_w : 0) - (show_gv ? gv_w : 0) < root_min;

    // The logo (built into the .exe), then the name in the trim's gold.
    F32 x = 16 * s;
    if (ui_image(rect(x, (header_h - logo) * 0.5f, x + logo, (header_h + logo) * 0.5f), str8_lit("res:LOGO"), UI_IMG_ICON, 0, ui_hex(0xffffff, 1))) x += logo + 10 * s;
    else x += 4 * s;
    if (show_title) x = f_draw_text(ui.font.title, x, (header_h - ui.font.title->line_height) * 0.5f, title, ui.theme.heading, 0) + 24 * s;
    else x += 4 * s;
    for (int f = 0; f < FLAVOR_COUNT; f += 1) {
        if (!app.flavor_exists[f]) continue;
        String8 label = str8_cstring(wow_flavors[f].label);
        F32 bw = ui_pill_tab_width(label), bh = 28 * s;
        Rect b = rect(x, (header_h - bh) * 0.5f, x + bw, (header_h + bh) * 0.5f);
        wm_caption_hole(b);
        if (ui_pill_tab(b, label, f == app.flavor)) switch_flavor(f);
        x = b.x1 + 6 * s;
    }
    if (show_gv) {
        hright -= gv_w - 16 * s;
        f_draw_text(ui.font.caption, hright, (header_h - ui.font.caption->line_height) * 0.5f, gv, C_FAINT, 0);
        hright -= 16 * s;
    }
    // Which install: click to pick another one or browse for it.
    {
        F32 bh = 28 * s, max_w = Max(root_min, hright - x - 12 * s);
        F32 bw = root_as_icon ? root_icon : Min(f_text_width(ui.font.caption, root_label) + 24 * s, max_w);
        Rect b = rect(hright - bw, (header_h - bh) * 0.5f, hright, (header_h + bh) * 0.5f);
        B32 hot = ui_mouse_in(b);
        wm_caption_hole(b);
        ui_plate(b, hot ? C_BUTTON_HI : C_BUTTON, 6 * s);
        if (root_as_icon) { // a folder (Segoe Fluent Icons / MDL2) and the arrow
            F32 gx = b.x0 + 10 * s, mid = (b.y0 + b.y1) * 0.5f;
            gx = f_draw_text(ui.font.glyphs, gx, mid - ui.font.glyphs->line_height * 0.5f, str8_lit("\xEE\xA2\xB7"), C_DIM, 0) + 5 * s;
            f_draw_text(ui.font.caption, gx, mid - ui.font.caption->line_height * 0.5f, str8_lit("\xE2\x96\xBE"), C_DIM, 0);
        } else {
            f_draw_text(ui.font.caption, b.x0 + 12 * s, (b.y0 + b.y1 - ui.font.caption->line_height) * 0.5f, root_label,
                        app.wow_root.size ? C_DIM : C_TEXT, bw - 24 * s);
        }
        app.root_btn = b;
        if (ui_clicked(b)) wm_defer(show_root_menu);
    }

    ui_divider(0, W, header_h);

    //- Toolbar: view tabs + actions
    F32 ty = header_h + 12 * s, th = 32 * s;
    x = 16 * s;
    for (int v = 0; v < VIEW_COUNT; v += 1) {
        U64 dl_waiting = 0, dl_new = 0;
        if (app.downloads) for (U64 i = 0; i < app.downloads->list.count; i += 1) {
            DownloadItem *it = &app.downloads->list.v[i];
            dl_waiting += it->state != DL_INSTALLED;
            dl_new += it->is_new && it->state != DL_INSTALLED;
        }
        // Installed counts what the list shows: installs on their way, not rows being removed.
        U64 shown = snap ? snap->row_count : 0;
        if (v == VIEW_INSTALLED && snap && app.gone.count) for (U64 i = 0; i < snap->row_count; i += 1) shown -= keyset_has(&app.gone, snap->rows[i].key);
        if (v == VIEW_INSTALLED) { PendingInstall *pend[8]; shown += feedback_installs(pend, ArrayCount(pend)); }
        String8 label = v == VIEW_INSTALLED ? str8_pushf(fa, "Installed  %llu", shown)
                      : v == VIEW_SEARCH ? str8_lit("Get addons")
                      : dl_waiting && !dl_new ? str8_pushf(fa, "Downloads  %llu", dl_waiting) : str8_lit("Downloads");
        // New archives in the download folder: a bright "N new" badge on the tab until they're installed.
        String8 badge = v == VIEW_DOWNLOADS && dl_new ? str8_pushf(fa, "%llu new", dl_new) : str8(0, 0);
        Rect b = rect(x, ty, x + ui_tab_width(label, badge), ty + th);
        if (ui_tab(b, label, badge, app.view == (View)v)) app.view = (View)v;
        x = b.x1 + 4 * s;
    }
    if (app.view == VIEW_INSTALLED && app.selected.count && snap) {
        // Selection actions.
        String8List upd = { 0 };
        for (U64 i = 0; i < snap->row_count; i += 1) {
            if (snap->rows[i].update && keyset_has(&app.selected, snap->rows[i].key)) str8_list_push(fa, &upd, snap->rows[i].key);
        }
        String8 sel = str8_pushf(fa, "%u selected", app.selected.count);
        f_draw_text(ui.font.normal, x + 12 * s, ty + (th - ui.font.normal->line_height) * 0.5f, sel, C_DIM, 0);
        F32 rx = W - 16 * s;
        String8 clear = str8_lit("Clear");
        Rect cb = rect(rx - ui_button_width(clear), ty, rx, ty + th);
        if (ui_button(cb, clear, C_TEXT, C_BUTTON, C_BUTTON_HI)) keyset_clear(&app.selected);
        rx = cb.x0 - 8 * s;
        String8 rem = str8_pushf(fa, "Remove  %u", app.selected.count);
        Rect rb = rect(rx - ui_button_width(rem), ty, rx, ty + th);
        if (ui_button(rb, rem, C_TEXT, C_DANGER, C_DANGER_HI)) start_remove(&app.selected);
        rx = rb.x0 - 8 * s;
        if (upd.count) {
            String8 label = str8_pushf(fa, "Update  %llu", upd.count);
            Rect ub = rect(rx - ui_button_width(label), ty, rx, ty + th);
            if (ui_button(ub, label, C_TEXT, C_ACCENT_BG, C_ACCENT)) feedback_update(push_task(TASK_UPDATE, str8_list_join(fa, &upd, str8_lit("\n"))), &upd);
        }
    } else if (app.view == VIEW_INSTALLED && app.wow_root.size) {
        F32 rx = W - 16 * s;
        String8 check = str8_lit("Check for updates");
        Rect cb = rect(rx - ui_button_width(check), ty, rx, ty + th);
        if (ui_button(cb, check, C_TEXT, C_BUTTON, C_BUTTON_HI)) push_task(TASK_SYNC, str8(0, 0));
        rx = cb.x0 - 8 * s;
        if (snap && snap->updates) {
            String8 all = str8_pushf(fa, "Update all  %llu", snap->updates);
            Rect ab = rect(rx - ui_button_width(all), ty, rx, ty + th);
            if (ui_button(ab, all, C_TEXT, C_ACCENT_BG, C_ACCENT)) {
                // Update all: what it will update shows as updating (not pinned or edited ones: it skips those).
                String8List upd = { 0 };
                for (U64 i = 0; i < snap->row_count; i += 1) {
                    Row *r = &snap->rows[i];
                    if (r->update && !r->pinned && !r->edited) str8_list_push(fa, &upd, r->key);
                }
                feedback_update(push_task(TASK_UPDATE, str8(0, 0)), &upd);
            }
        }
    }

    //- Remove confirmation bar
    F32 content_top = ty + th + 12 * s;
    if (app.view == VIEW_INSTALLED && app.removing.count && snap) {
        RemovePlan plan = remove_plan(fa, snap, &app.removing, app.remove_with_deps);
        if (!plan.chosen.count) {
            keyset_clear(&app.removing);
        } else {
            F32 line = 24 * s, pad = 14 * s;
            int lines = 1 + (plan.deps.count > 0) + (plan.broken.count > 0);
            Rect bar = rect(16 * s, content_top, W - 16 * s, content_top + pad * 2 + line * (F32)lines);
            r_rect(bar, ui_hex(0x2a1c1f, 1), 8 * s, 0, 0.5f);
            r_rect(bar, ui_hex(0x5a2a2c, 1), 8 * s, ui_px(), 0.5f);

            F32 bx = bar.x1 - pad, bh = 30 * s, by = bar.y0 + pad;
            String8 cancel = str8_lit("Cancel");
            Rect cb = rect(bx - ui_button_width(cancel), by, bx, by + bh);
            if (ui_button(cb, cancel, C_TEXT, C_BUTTON, C_BUTTON_HI)) keyset_clear(&app.removing);
            U64 total = plan.keys.count;
            String8 go = total > 1 ? str8_pushf(fa, "Remove %llu addons", total) : str8_lit("Remove");
            Rect gb = rect(cb.x0 - 8 * s - ui_button_width(go), by, cb.x0 - 8 * s, by + bh);
            if (ui_button(gb, go, C_TEXT, C_DANGER, C_DANGER_HI)) {
                // The rows go at once (feedback.c); the worker moves the folders to the Recycle Bin.
                feedback_remove(push_task(TASK_REMOVE, str8_list_join(fa, &plan.keys, str8_lit("\n"))), &plan.keys);
                for (String8Node *n = plan.keys.first; n; n = n->next) keyset_remove(&app.selected, n->string);
                keyset_clear(&app.removing);
            }

            F32 tx = bar.x0 + pad, tw = gb.x0 - 16 * s - tx, ly = bar.y0 + pad + (line - ui.font.bold->line_height) * 0.5f;
            String8 what = str8_pushf(fa, "Remove %.*s?", S8F(str8_list_join(fa, &plan.chosen, str8_lit(", "))));
            f_draw_text(ui.font.bold, tx, ly, what, C_TEXT, tw);
            ly += line;
            if (plan.deps.count) {
                if (ui_checkbox(tx, ly + ui.font.normal->line_height * 0.5f, app.remove_with_deps)) app.remove_with_deps = !app.remove_with_deps;
                String8 d = str8_pushf(fa, "Also remove %llu dependenc%s nothing else needs: %.*s", plan.deps.count,
                                       plan.deps.count == 1 ? "y" : "ies", S8F(str8_list_join(fa, &plan.deps, str8_lit(", "))));
                f_draw_text(ui.font.normal, tx + 26 * s, ly, d, C_DIM, tw - 26 * s);
                ly += line;
            }
            if (plan.broken.count) {
                String8 b = str8_pushf(fa, "These stay installed but need what you're removing: %.*s", S8F(str8_list_join(fa, &plan.broken, str8_lit(", "))));
                f_draw_text(ui.font.normal, tx, ly, b, C_ERROR, tw);
            }
            content_top = bar.y1 + 12 * s;
        }
    }

    //- Content panel
    F32 status_h = 30 * s;
    Rect panel = rect(16 * s, content_top, W - 16 * s, H - status_h);
    if (panel.y1 > panel.y0 + 40 * s) {
        ui_panel(panel, 8 * s);
        Rect inner = rect(panel.x0 + ui_px(), panel.y0 + ui_px(), panel.x1 - ui_px(), panel.y1 - ui_px());
        animating |= app.view == VIEW_INSTALLED ? draw_installed(inner) : app.view == VIEW_SEARCH ? draw_search(inner) : draw_downloads(inner);
        r_set_clip(rect(0, 0, W, H));
        ui.clip = rect(0, 0, W, H);
        ui_panel_rim(panel, 8 * s); // over the content's edges
    }

    //- Status bar
    F32 sy = H - status_h + (status_h - ui.font.caption->line_height) * 0.5f;
    os_mutex_lock(&status_lock);
    String8 status = str8_copy(fa, str8_cstring(status_text));
    String8 url = str8_copy(fa, str8_cstring(status_url));
    B32 is_error = status_is_error;
    os_mutex_unlock(&status_lock);
    if (!status.size && snap) {
        status = str8_pushf(fa, "%llu tracked  \xC2\xB7  %llu folders scanned in %.0f ms  \xC2\xB7  %.*s",
                            snap->tracked, snap->folders, snap->scan_ms, S8F(snap->addons_dir));
    }
    if (!app.api_key.size && !app.wago_key[0] && !status_text[0] && !snap) status = str8_lit("CurseForge unavailable in this build; GitHub, WoWInterface and Wago (with your key) work");
    F32 left = 20 * s;
    if (app.pending) {
        animating |= ui_spinner(left + 6 * s, sy + ui.font.caption->line_height * 0.5f, 7 * s); // working on it
        left += 20 * s;
    }
    // fps: frames actually drawn in the last second (they only come on demand, so it reads low when idle)
    String8 perf = str8_pushf(fa, "%u fps  \xC2\xB7  cpu %.2f ms  \xC2\xB7  startup %.0f ms", ui.fps, ui.frame_ms, app.init_ms);
    F32 pw = f_text_width(ui.font.caption, perf);
    F32 right = W - 20 * s - pw - 24 * s;
    if (app.offer_count) {
        // Several addons only CurseForge's pages may serve (e.g. after Update all): open them on request.
        String8 label = str8_pushf(fa, "Get %u from CurseForge", app.offer_count);
        F32 bw = ui_small_button_width(label), bh = 22 * s;
        F32 by = H - status_h + (status_h - bh) * 0.5f;
        Rect b = rect(right - bw, by, right, by + bh);
        if (ui_small_button(b, label, C_ACCENT_BG, C_ACCENT)) waiting_open_all();
        right = b.x0 - 12 * s;
        url = str8(0, 0); // this replaces the single "Open download page"
    }
    if (url.size) {
        // The failed addon can only be downloaded by hand: offer its page.
        String8 label = str8_lit("Open download page");
        F32 bw = ui_small_button_width(label), bh = 22 * s;
        F32 by = H - status_h + (status_h - bh) * 0.5f;
        Rect b = rect(right - bw, by, right, by + bh);
        if (ui_small_button(b, label, C_ACCENT_BG, C_ACCENT)) os_open_url(url);
        right = b.x0 - 12 * s;
    }
    f_draw_text(ui.font.caption, left, sy, status, is_error ? C_ERROR : C_DIM, right - left);
    f_draw_text(ui.font.caption, W - 20 * s - pw, sy, perf, C_FAINT, 0);

    if (app.viewer_open) animating |= draw_viewer();
    if (app.changes_open) animating |= draw_changes();
    if (app.settings_open) draw_settings();
    if (app.init_ms == 0) app.init_ms = (F64)(os_now_us() - app.t_start) / 1000.0;
    return animating;
}

