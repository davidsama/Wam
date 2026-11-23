// gui/remove.c - the removal popup. Removing always asks here first, once for however many addons:
// it lists them, offers to take dependencies nothing else needs along, and warns when addons that
// stay need what's going. Part of main_gui.c's unity build.

static UI_Scroll remove_scroll; // the list of addons

// Asks about removing these (start_remove in views.c).
static void remove_open(KeySet *keys)
{
    app.removing = *keys;
    app.remove_with_deps = 1;
    app.remove_fresh = 1;
    remove_scroll.pos = remove_scroll.target = 0;
    ui.focus.v = 0; // (Enter is the popup's now)
    wm_request_frame();
}

static void remove_confirm(Arena *a)
{
    RemovePlan plan = remove_plan(a, app.snap, &app.removing, app.remove_with_deps);
    keyset_clear(&app.removing);
    // The rows go at once (feedback.c); the worker moves the folders to the Recycle Bin. As many
    // tasks as it takes: one holds so much text.
    String8List part = { 0 };
    U64 size = 0;
    for (String8Node *n = plan.keys.first; n; n = n->next) {
        str8_list_push(a, &part, n->string);
        size += n->string.size + 1;
        if (n->next && size + n->next->string.size + 1 < sizeof(((Task *)0)->text)) continue;
        U32 task = push_task(TASK_REMOVE, str8_list_join(a, &part, str8_lit("\n")));
        if (!task) { worker_set_status(str8_lit("wam has too much to do already. Try removing again in a moment."), 1); break; }
        feedback_remove(task, &part);
        part = (String8List){ 0 };
        size = 0;
    }
}

// Keys while the popup is open (it takes them all). Returns 1 if it was open.
static B32 remove_event(WM_Event *e)
{
    if (!app.removing.count) return 0;
    if (e->kind == WM_EVENT_KEY && e->key == WM_KEY_ESCAPE) keyset_clear(&app.removing);
    else if (e->kind == WM_EVENT_KEY && e->key == WM_KEY_RETURN) remove_confirm(ui.frame_arena);
    return 1;
}

static B32 draw_remove(void)
{
    F32 s = ui.scale;
    Arena *fa = ui.frame_arena;
    B32 animating = 0;
    RemovePlan plan = remove_plan(fa, app.snap, &app.removing, app.remove_with_deps);
    if (!plan.chosen.count) { keyset_clear(&app.removing); return 0; } // gone meanwhile

    static F32 content_h; // measured by the last frame
    F32 foot_h = 64 * s;
    Rect panel = ui_modal_begin(520 * s, (content_h > 0 ? content_h : 150 * s) + foot_h, &app.remove_fresh);
    F32 pad = 24 * s, x = panel.x0 + pad, w = (panel.x1 - panel.x0) - 2 * pad, y = panel.y0 + 18 * s, vis = panel.y1 - foot_h;

    U64 n = plan.chosen.count;
    String8 title = n == 1 ? str8_pushf(fa, "Remove %.*s?", S8F(plan.chosen.first->string)) : str8_pushf(fa, "Remove %llu addons?", n);
    f_draw_text(ui.font.title, x, y, title, ui.theme.heading, w - 40 * s);
    B32 close = ui_modal_close_button(panel);
    y += ui.font.title->line_height + 12 * s;

    // Several: their names, scrolling when there are many.
    if (n > 1) {
        F32 row_h = 24 * s;
        Rect list = rect(x, y, x + w, y + row_h * (F32)Min(n, 8) + 8 * s);
        r_rect(list, C_BG, 6 * s, 0, 0.5f);
        Rect inner = rect(list.x0, list.y0 + 4 * s, list.x1, list.y1 - 4 * s);
        UI_List l = ui_list_begin(&remove_scroll, inner, (S64)n, row_h, 0);
        animating |= l.animating;
        S64 i = 0;
        for (String8Node *it = plan.chosen.first; it; it = it->next, i += 1) {
            if (i < l.first || i >= l.last) continue;
            Rect rr = ui_list_row(&l, i, str8(0, 0));
            f_draw_text(ui.font.normal, rr.x0 + 12 * s, rr.y0 + (row_h - ui.font.normal->line_height) * 0.5f, it->string, C_TEXT, w - 32 * s);
        }
        ui_list_end(&l);
        ui.clip = panel;
        r_set_clip(panel);
        y = list.y1 + 12 * s;
    }
    if (plan.deps.count) {
        F32 lh = ui.font.normal->line_height;
        String8 t = str8_pushf(fa, "Also remove %llu addon%s nothing else needs: %.*s", plan.deps.count, plan.deps.count == 1 ? "" : "s",
                               S8F(str8_list_join(fa, &plan.deps, str8_lit(", "))));
        B32 toggle = ui_checkbox(str8_lit("with dependencies"), x, y + lh * 0.5f, app.remove_with_deps);
        F32 th = ui_text_wrapped(ui.font.normal, x + 28 * s, y, w - 28 * s, t, C_TEXT, y, vis);
        if (ui_clicked(rect(x + 28 * s, y, x + w, y + th))) toggle = 1;
        if (toggle) app.remove_with_deps = !app.remove_with_deps;
        y += th + 12 * s;
    }
    if (plan.broken.count) {
        String8 t = str8_pushf(fa, "These stay installed but need what you're removing: %.*s", S8F(str8_list_join(fa, &plan.broken, str8_lit(", "))));
        y += ui_text_wrapped(ui.font.normal, x, y, w, t, C_WARN, y, vis) + 12 * s;
    }
    content_h = y - panel.y0 + 4 * s;

    // Foot: where they go, and the buttons.
    r_rect(rect(panel.x0 + ui_px(), vis, panel.x1 - ui_px(), vis + ui_px()), C_LINE, 0, 0, 0);
    F32 bh = 32 * s, by = vis + (foot_h - bh) * 0.5f, bx = panel.x1 - pad;
    String8 go = plan.keys.count > 1 ? str8_pushf(fa, "Remove %llu addons###remove", plan.keys.count) : str8_lit("Remove###remove");
    Rect gb = rect(bx - ui_button_width(go), by, bx, by + bh);
    B32 confirm = ui_button(gb, go, C_TEXT, C_DANGER, C_DANGER_HI);
    String8 cancel = str8_lit("Cancel");
    Rect cb = rect(gb.x0 - 8 * s - ui_button_width(cancel), by, gb.x0 - 8 * s, by + bh);
    if (ui_button(cb, cancel, C_TEXT, C_BUTTON, C_BUTTON_HI)) close = 1;
    f_draw_text(ui.font.caption, x, by + (bh - ui.font.caption->line_height) * 0.5f, str8_lit("To the Recycle Bin"), C_FAINT, cb.x0 - x - 12 * s);

    if (ui_modal_end(panel)) close = 1;
    if (confirm) remove_confirm(fa);
    else if (close) keyset_clear(&app.removing);
    return animating;
}
