// gui/changes.c - the changes popup: what an installed addon's update would bring. Opened from the
// "→ new version" link on an Installed row: the releases after the installed one, newest first,
// each with its changelog (core/catalog.c, mgr_changes), and Update (or Unpin, for a pinned addon).
// Part of main_gui.c's unity build.

static F32 changes_content_h; // the release notes' height, measured by the last frame

static void changes_open(String8 id)
{
    if (app.changes) { arena_release(app.changes->arena); app.changes = 0; }
    copy_cstr(app.changes_id, sizeof(app.changes_id), id);
    app.changes_open = 1;
    app.changes_fresh = 1;
    app.changes_scroll = (UI_Scroll){ 0 };
    changes_content_h = 0;
    push_task(TASK_CHANGES, id);
    ui_request_frame();
}

static void changes_close(void)
{
    app.changes_open = 0;
    if (app.changes) { arena_release(app.changes->arena); app.changes = 0; }
    ui_request_frame();
}

static void changes_arrived(ChangesSnap *cs)
{
    B32 wanted = app.changes_open && !app.changes && cs->gen == app.root_gen && cs->flavor == app.flavor
              && str8_match(cs->c.id, str8_cstring(app.changes_id), 1);
    if (!wanted) { arena_release(cs->arena); return; }
    app.changes = cs;
}

// Keys while the popup is open (it takes them all). Returns 1 if it was open.
static B32 changes_key(WPARAM wp)
{
    if (!app.changes_open) return 0;
    F32 page = ui.height * 0.5f, line = 48 * ui.scale;
    UI_Scroll *sc = &app.changes_scroll;
    if (wp == VK_ESCAPE) changes_close();
    else if (wp == VK_DOWN) sc->target += line;
    else if (wp == VK_UP) sc->target -= line;
    else if (wp == VK_NEXT || wp == VK_SPACE) sc->target += page;
    else if (wp == VK_PRIOR) sc->target -= page;
    else if (wp == VK_HOME) sc->target = 0;
    else if (wp == VK_END) sc->target = 1e9f;
    ui_request_frame();
    return 1;
}

static Row *changes_row(void)
{
    if (!app.snap) return 0;
    String8 id = str8_cstring(app.changes_id);
    for (U64 i = 0; i < app.snap->row_count; i += 1) if (str8_match(app.snap->rows[i].id, id, 1)) return &app.snap->rows[i];
    return 0;
}

static B32 draw_changes(void)
{
    F32 s = ui.scale;
    Arena *fa = ui.frame_arena;
    B32 animating = 0;
    Row *row = changes_row();
    AddonChanges *c = app.changes ? &app.changes->c : 0;
    if (!row) { changes_close(); return 0; } // removed meanwhile, or another flavor

    F32 head_h = 92 * s, foot_h = 64 * s;
    F32 want_h = head_h + foot_h + (c && changes_content_h > 0 ? changes_content_h : 260 * s);
    Rect panel = modal_begin(700 * s, Min(want_h, 720 * s), &app.changes_fresh);
    F32 pad = 24 * s, x = panel.x0 + pad, w = (panel.x1 - panel.x0) - 2 * pad, y = panel.y0 + 18 * s;

    // Head: the addon, and from which version to which.
    f_draw_text(ui.font.title, x, y, row->title, ui.theme.heading, w - 40 * s);
    B32 close = modal_close_button(panel);
    y += ui.font.title->line_height + 8 * s;
    String8 to = c && c->ok && c->to.size ? c->to : row->latest;
    String8 line = str8_pushf(fa, "Installed %.*s  \xE2\x86\x92  %.*s%s", S8F(row->version), S8F(to), row->pinned ? "  \xC2\xB7  pinned" : "");
    f_draw_text(ui.font.caption, x, y, line, C_DIM, w);

    // Body: the release notes, scrolling.
    Rect body = rect(panel.x0 + ui_px(), panel.y0 + head_h, panel.x1 - ui_px(), panel.y1 - foot_h);
    r_rect(rect(body.x0, body.y0 - ui_px(), body.x1, body.y0), C_LINE, 0, 0, 0);
    r_rect(rect(body.x0, body.y1, body.x1, body.y1 + ui_px()), C_LINE, 0, 0, 0);
    if (!c) {
        F32 cy = (body.y0 + body.y1) * 0.5f;
        animating |= ui_spinner((body.x0 + body.x1) * 0.5f, cy - 14 * s, 9 * s);
        ui_text_centered(ui.font.caption, rect(body.x0, cy + 4 * s, body.x1, cy + 24 * s), str8_lit("Getting the release notes..."), C_DIM);
    } else if (!c->ok) {
        F32 cy = (body.y0 + body.y1) * 0.5f;
        ui_text_centered(ui.font.normal, rect(body.x0, cy - 22 * s, body.x1, cy), str8_lit("Couldn't get the release notes."), C_TEXT);
        ui_text_centered(ui.font.caption, rect(body.x0, cy + 2 * s, body.x1, cy + 22 * s), c->error, C_ERROR);
    } else {
        animating |= ui_scroll_list(&app.changes_scroll, body, changes_content_h);
        Rect clip = rect(body.x0, body.y0, body.x1, body.y1);
        ui.clip = clip;
        r_set_clip(clip);
        F32 top = body.y0 + 16 * s - app.changes_scroll.pos, cy = top, tw = w - 8 * s;
        if (!c->count) {
            f_draw_text(ui.font.normal, x, cy, str8_lit("No release notes found for the versions since yours."), C_DIM, tw);
            cy += ui.font.normal->line_height + 8 * s;
        }
        for (U32 i = 0; i < c->count; i += 1) {
            ChangeEntry *ce = &c->v[i];
            F32 vx = f_draw_text(ui.font.bold, x, cy, ce->version.size ? ce->version : str8_lit("Latest"), ui.theme.heading, tw);
            if (ce->date.size) f_draw_text(ui.font.caption, vx + 10 * s, cy + (ui.font.bold->line_height - ui.font.caption->line_height) * 0.5f, ce->date, C_FAINT, 0);
            cy += ui.font.bold->line_height + 6 * s;
            if (ce->text.size) cy += ui_text_wrapped(ui.font.caption, x, cy, tw, ce->text, C_DIM, body.y0, body.y1);
            else { f_draw_text(ui.font.caption, x, cy, str8_lit("No notes for this release."), C_FAINT, tw); cy += ui.font.caption->line_height; }
            cy += 22 * s;
        }
        String8 note = c->more ? str8_lit("Older releases since yours aren't shown here; the addon's page has them all.")
                     : c->single ? str8_lit("This site keeps one changelog per addon: shown down to where it mentions your version.")
                     : str8(0, 0);
        if (note.size) { cy += ui_text_wrapped(ui.font.caption, x, cy, tw, note, C_FAINT, body.y0, body.y1) + 12 * s; }
        changes_content_h = cy - top + 4 * s;
        ui.clip = panel;
        r_set_clip(panel);
        ui_scrollbar(&app.changes_scroll, body, changes_content_h);
    }

    // Foot: what to do about it.
    F32 bh = 32 * s, by = panel.y1 - foot_h + (foot_h - bh) * 0.5f, bx = panel.x1 - pad;
    if (row->pinned) {
        String8 unpin = str8_lit("Unpin");
        Rect b = rect(bx - ui_button_width(unpin), by, bx, by + bh);
        if (ui_button(b, unpin, C_TEXT, C_BUTTON, C_BUTTON_HI)) push_task_ex(TASK_PIN, row->id, 0);
        f_draw_text(ui.font.caption, x, by + (bh - ui.font.caption->line_height) * 0.5f,
                    str8_lit("Pinned: updates leave it at its version. Unpin it to update."), C_FAINT, b.x0 - x - 16 * s);
    } else if (row->update) {
        String8 label = row->edited ? str8_lit("Overwrite edits") : str8_lit("Update");
        Rect b = rect(bx - ui_button_width(label), by, bx, by + bh);
        if (ui_button(b, label, C_TEXT, row->edited ? ui_hex(0x6b4a12, 1) : C_ACCENT_BG, row->edited ? ui_hex(0x86601c, 1) : C_ACCENT)) {
            String8List one = { 0 };
            str8_list_push(fa, &one, row->key);
            feedback_update(push_task_ex(TASK_UPDATE, row->key, 1), &one);
            close = 1;
        }
        if (row->edited) f_draw_text(ui.font.caption, x, by + (bh - ui.font.caption->line_height) * 0.5f,
                                     str8_lit("You changed its files since wam installed it: updating replaces your changes."), C_WARN, b.x0 - x - 16 * s);
    } else {
        f_draw_text(ui.font.caption, x, by + (bh - ui.font.caption->line_height) * 0.5f, str8_lit("Up to date."), C_GREEN, w);
    }

    if (modal_end(panel) || close) changes_close();
    return animating;
}
