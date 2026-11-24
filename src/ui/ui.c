// ui.c - state, geometry, theme, fonts and widgets. See ui.h.

//- Geometry, color, hit testing

static Rect rect(F32 x0, F32 y0, F32 x1, F32 y1) { Rect r = { x0, y0, x1, y1 }; return r; }
static B32 point_in(Rect r, F32 x, F32 y) { return x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1; }

static V4 ui_hex(U32 rgb, F32 alpha)
{
    V4 c = { ((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f, (rgb & 255) / 255.0f, alpha };
    return c;
}

static V4 ui_mix(V4 a, V4 b, F32 t)
{
    V4 c = { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t };
    return c;
}

static F32 ui_px(void) { return (F32)Max(1, (int)(ui.scale + 0.5f)); }

static B32 ui_mouse_in(Rect r)
{
    if (!ui.in.inside || !point_in(r, ui.in.mx, ui.in.my)) return 0;
    // An open overlay covers what's below it: only the overlay itself sees the mouse there.
    if (ui.overlay_on && !ui.in_overlay && point_in(ui.overlay, ui.in.mx, ui.in.my)) return 0;
    for (U32 i = 0; i < ui.block_count; i += 1) if (point_in(ui.block[i], ui.in.mx, ui.in.my)) return 0;
    return 1;
}

static void ui_block_mouse(Rect r)
{
    if (ui.block_count < ArrayCount(ui.block)) ui.block[ui.block_count++] = r;
}

static B32 ui_clicked(Rect r)
{
    return ui_mouse_in(r) && ui.in.released && point_in(r, ui.in.press_x, ui.in.press_y);
}

//- Identity, interaction, focus, animation

static U64 ui_hash(U64 seed, String8 s)
{
    U64 h = seed ? seed : 14695981039346656037ull;
    for (U64 i = 0; i < s.size; i += 1) { h ^= s.str[i]; h *= 1099511628211ull; }
    return h ? h : 1;
}

static U64 ui_scope_seed(void) { return ui.scope_count ? ui.scope[Min(ui.scope_count, UI_SCOPE_MAX) - 1] : 0; }

static String8 ui_label_text(String8 label) { return str8_prefix(label, str8_find(label, 0, str8_lit("##"), 0)); }

static UI_Id ui_id(String8 label)
{
    U64 i = str8_find(label, 0, str8_lit("###"), 0);
    if (i < label.size) label = str8_skip(label, i + 3);
    UI_Id id = { ui_hash(ui_scope_seed(), label) };
    return id;
}

static UI_Id ui_id_ptr(void *p)
{
    U64 v = (U64)(uintptr_t)p;
    UI_Id id = { ui_hash(0x9e3779b97f4a7c15ull, str8((U8 *)&v, sizeof(v))) };
    return id;
}

static B32 ui_id_match(UI_Id a, UI_Id b) { return a.v == b.v; }

static void ui_scope_push(String8 key)
{
    U64 seed = ui_hash(ui_scope_seed() ^ 0x5c09e5ull, key);
    if (ui.scope_count < UI_SCOPE_MAX) ui.scope[ui.scope_count] = seed;
    ui.scope_count += 1; // (past the max, pushes and pops still balance)
}

static void ui_scope_push_u64(U64 key) { ui_scope_push(str8((U8 *)&key, sizeof(key))); }
static void ui_scope_pop(void) { if (ui.scope_count) ui.scope_count -= 1; }

static UI_Signal ui_signal(UI_Id id, Rect r, U32 flags)
{
    UI_Signal sg = { 0 };
    sg.hovering = ui_mouse_in(r);
    if (sg.hovering) ui.hot = id;
    // A press inside r makes it active. (A widget that takes a press for itself, like a scrollbar,
    // moves ui.in.press_x/y away so nothing else sees it.)
    if (ui.in.pressed && sg.hovering && point_in(r, ui.in.press_x, ui.in.press_y)) { ui.active = id; sg.pressed = 1; }
    B32 active = id.v && ui.active.v == id.v;
    sg.dragging = active && ui.in.down;
    sg.released = active && ui.in.released;
    // Released where it was pressed. (Checking the press too keeps two widgets that happen to share
    // an id from taking each other's clicks.)
    sg.clicked = sg.released && sg.hovering && point_in(r, ui.in.press_x, ui.in.press_y);
    if (flags & UI_FOCUSABLE) {
        if (ui.focusable_count < UI_FOCUS_MAX) {
            UI_Focusable f = { id, ui.in_overlay };
            ui.focusables[ui.focusable_count++] = f;
        }
        sg.focused = id.v && ui.focus.v == id.v;
        if (sg.focused) {
            ui.focus_seen = 1;
            if (ui.focus_activate) { ui.focus_activate = 0; sg.clicked = sg.keyboard = 1; }
        }
    }
    return sg;
}

static B32 ui_is_active(UI_Id id) { return id.v && ui.active.v == id.v; }

static void ui_focus_ring(UI_Id id, Rect r, F32 radius)
{
    if (!ui.focus_visible || !id.v || ui.focus.v != id.v) return;
    F32 s = ui.scale, g = 3 * s;
    r_rect(rect(r.x0 - g, r.y0 - g, r.x1 + g, r.y1 + g), ui.theme.accent_hi, radius + g, 1.5f * s, 0.5f);
}

// Tab / Shift+Tab: the next or previous focusable widget of the last frame (the overlay's only,
// while one is up), wrapping around.
static void ui_focus_step(int dir)
{
    UI_Focusable *f = ui.focusables_prev;
    U32 n = ui.focusable_prev_count;
    B32 overlay = 0;
    for (U32 i = 0; i < n; i += 1) overlay |= f[i].in_overlay;
    UI_Id order[UI_FOCUS_MAX];
    S32 count = 0, cur = -1;
    for (U32 i = 0; i < n; i += 1) {
        if (overlay && !f[i].in_overlay) continue;
        if (f[i].id.v == ui.focus.v) cur = count;
        order[count++] = f[i].id;
    }
    if (!count) return;
    S32 next = cur < 0 ? (dir > 0 ? 0 : count - 1) : (cur + dir + count) % count;
    ui.focus = order[next];
    ui.focus_visible = 1;
}

// The keys focus takes before the app sees them: Tab and Shift+Tab move it; with the focus shown,
// Enter or Space clicks the focused widget and Esc lets go of it. Typing or a mouse press hides it.
static void ui_focus_keys(void)
{
    if (ui.in.pressed) { ui.focus.v = 0; ui.focus_visible = 0; }
    U32 kept = 0;
    for (U32 i = 0; i < ui.event_count; i += 1) {
        WM_Event e = ui.events[i];
        B32 eat = 0;
        if (e.kind == WM_EVENT_KEY) {
            ui.eat_space_text = 0;
            B32 shown = ui.focus.v && ui.focus_visible;
            if (e.key == WM_KEY_TAB && !(e.mods & (WM_MOD_CTRL | WM_MOD_ALT))) {
                ui_focus_step(e.mods & WM_MOD_SHIFT ? -1 : 1);
                eat = 1;
            } else if (shown && !e.mods && (e.key == WM_KEY_RETURN || e.key == WM_KEY_SPACE)) {
                ui.focus_activate = 1;
                ui.eat_space_text = e.key == WM_KEY_SPACE; // its ' ' comes next
                eat = 1;
            } else if (shown && e.key == WM_KEY_ESCAPE) {
                ui.focus.v = 0;
                eat = 1;
            }
        } else if (e.kind == WM_EVENT_TEXT) {
            if (ui.eat_space_text && e.codepoint == ' ') { ui.eat_space_text = 0; eat = 1; }
            else if (ui.focus_visible) { ui.focus.v = 0; ui.focus_visible = 0; }
        }
        if (!eat) ui.events[kept++] = e;
    }
    ui.event_count = kept;
}

static F32 ui_anim(UI_Id id, F32 target)
{
    if (!id.v) return target;
    U64 h = id.v % UI_ANIM_SLOTS;
    UI_AnimSlot *slot = 0, *spare = 0;
    for (U32 k = 0; k < 8 && !slot; k += 1) {
        UI_AnimSlot *c = &ui.anim[(h + k) % UI_ANIM_SLOTS];
        if (c->id.v == id.v) slot = c;
        else if (!spare && (!c->id.v || c->frame + 1 < ui.frame_index)) spare = c; // free, or not drawn since the last frame
    }
    if (slot && slot->frame + 1 < ui.frame_index) slot->value = target; // back after a while: start over
    if (!slot) {
        if (!spare) return target;
        slot = spare;
        slot->id = id;
        slot->value = target;
    }
    slot->frame = ui.frame_index;
    F32 d = target - slot->value;
    if (fabsf(d) < 0.01f) slot->value = target;
    else { slot->value += d * (1.0f - expf(-ui.dt * 25.0f)); wm_request_frame(); }
    return slot->value;
}

// Around each frame (ui_frame).
static void ui_ids_begin_frame(void)
{
    ui.frame_index += 1;
    ui.scope_count = 0;
    ui.hot.v = 0;
    ui.focusable_count = 0;
    ui.focus_seen = 0;
    ui_focus_keys();
}

static void ui_ids_end_frame(void)
{
    if (!ui.in.down) ui.active.v = 0;
    if (ui.focus.v && !ui.focus_seen) ui.focus.v = 0; // not drawn any more (a popup closed, a row scrolled away)
    ui.focus_activate = 0;
    memcpy(ui.focusables_prev, ui.focusables, ui.focusable_count * sizeof(UI_Focusable));
    ui.focusable_prev_count = ui.focusable_count;
}

static UI_Theme ui_theme_dark(void)
{
    UI_Theme t;
    t.bg        = ui_hex(0x0e1014, 1);
    t.panel     = ui_hex(0x15181e, 1);
    t.hover     = ui_hex(0x1c2029, 1);
    t.button    = ui_hex(0x22262f, 1);
    t.button_hi = ui_hex(0x2b303b, 1);
    t.border    = ui_hex(0x252a34, 1);
    t.border_hi = ui_hex(0x3a4150, 1);
    t.selected  = ui_hex(0x1b2433, 1);
    t.line      = ui_hex(0x1d2129, 1);
    t.text      = ui_hex(0xe7e9ee, 1);
    t.dim       = ui_hex(0x9098a8, 1);
    t.faint     = ui_hex(0x5c6372, 1);
    t.heading   = t.text;
    t.accent    = ui_hex(0x4c8dff, 1);
    t.accent_hi = ui_hex(0x6aa0ff, 1);
    t.accent_bg = ui_hex(0x24467e, 1);
    t.ok        = ui_hex(0x3fb77a, 1);
    t.danger    = ui_hex(0xd9534f, 1);
    t.danger_hi = ui_hex(0xe86b67, 1);
    t.error     = ui_hex(0xff7b72, 1);
    t.warn      = ui_hex(0xe3b341, 1);
    t.trim_hi   = t.border_hi;
    t.trim      = t.border;
    t.trim_dark = t.bg;
    t.gem       = t.accent;
    t.ornate    = 0;
    t.style     = UI_STYLE_GEARWORK;
    t.spark     = t.accent;
    t.aura      = ui_hex(0x000000, 0);
    return t;
}

static UI_Theme ui_theme_tinker(void)
{
    UI_Theme t;
    t.bg        = ui_hex(0x0a0d14, 1);
    t.panel     = ui_hex(0x111723, 1);
    t.hover     = ui_hex(0x192131, 1);
    t.button    = ui_hex(0x232c3e, 1);
    t.button_hi = ui_hex(0x2e3a52, 1);
    t.border    = ui_hex(0x252e42, 1);
    t.border_hi = ui_hex(0x3b4762, 1);
    t.selected  = ui_hex(0x1c2841, 1);
    t.line      = ui_hex(0x19202e, 1);
    t.text      = ui_hex(0xf0e9da, 1);
    t.dim       = ui_hex(0xa9a69c, 1);
    t.faint     = ui_hex(0x6b6d73, 1);
    t.heading   = ui_hex(0xf3d283, 1);
    t.accent    = ui_hex(0xc08a2c, 1);
    t.accent_hi = ui_hex(0xdcaa48, 1);
    t.accent_bg = ui_hex(0x7c5418, 1);
    t.ok        = ui_hex(0x5cc98a, 1);
    t.danger    = ui_hex(0x9e3028, 1);
    t.danger_hi = ui_hex(0xbd4136, 1);
    t.error     = ui_hex(0xff7b72, 1);
    t.warn      = ui_hex(0xe3b341, 1);
    t.trim_hi   = ui_hex(0xf3d283, 1);
    t.trim      = ui_hex(0xa8772a, 1);
    t.trim_dark = ui_hex(0x3c2810, 1);
    t.gem       = ui_hex(0x4fb3ff, 1);
    t.ornate    = 1;
    t.style     = UI_STYLE_GEARWORK;
    t.spark     = ui_hex(0xf3d283, 1);
    t.aura      = ui_hex(0x2b4a8c, 0.10f);
    return t;
}

static void ui_fonts_load(void)
{
    if (ui.font_arena) {
        f_font_close(ui.font.normal); f_font_close(ui.font.bold); f_font_close(ui.font.caption);
        f_font_close(ui.font.title); f_font_close(ui.font.icon); f_font_close(ui.font.glyphs); f_font_close(ui.font.glyphs_big);
        f_font_close(ui.font.italic); f_font_close(ui.font.mono); f_font_close(ui.font.caption_bold);
        f_font_close(ui.font.caption_italic); f_font_close(ui.font.caption_mono);
        arena_clear(ui.font_arena);
    } else {
        ui.font_arena = arena_alloc(MB(64));
    }
    f_atlas_reset();
    F32 s = ui.scale;
    ui.font.normal = f_font_open(ui.font_arena, L"Segoe UI", 14 * s, 400, 0);
    ui.font.bold   = f_font_open(ui.font_arena, L"Segoe UI", 14 * s, 600, 0);
    ui.font.caption  = f_font_open(ui.font_arena, L"Segoe UI", 12 * s, 400, 0);
    ui.font.title  = f_font_open(ui.font_arena, L"Segoe UI", 17 * s, 600, 0);
    ui.font.icon   = f_font_open(ui.font_arena, L"Segoe UI Symbol", 12 * s, 700, 0);
    // Windows 11's caption glyphs; Windows 10 has the older MDL2 ones (same code points).
    wchar_t *glyph_family = f_family_installed(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    ui.font.glyphs = f_font_open(ui.font_arena, glyph_family, 10 * s, 400, 0);
    ui.font.glyphs_big = f_font_open(ui.font_arena, glyph_family, 14 * s, 400, 0);
    // Rich text (descriptions, changelogs).
    ui.font.italic         = f_font_open(ui.font_arena, L"Segoe UI", 14 * s, 400, 1);
    ui.font.mono           = f_font_open(ui.font_arena, L"Consolas", 13 * s, 400, 0);
    ui.font.caption_bold   = f_font_open(ui.font_arena, L"Segoe UI", 12 * s, 600, 0);
    ui.font.caption_italic = f_font_open(ui.font_arena, L"Segoe UI", 12 * s, 400, 1);
    ui.font.caption_mono   = f_font_open(ui.font_arena, L"Consolas", 11.5f * s, 400, 0);
}

//- Ornaments

static void ui_plate(Rect r, V4 bg, F32 radius)
{
    F32 o = ui.theme.ornate;
    r_rect(r, bg, radius, 0, 0.5f);
    if (o <= 0) return;
    V4 rim = ui_mix(bg, ui.theme.trim, 0.45f * o); // a thin rim, tinted towards the trim's metal
    rim.w = bg.w;
    r_rect(r, rim, radius, ui_px(), 0.5f);
}

// Color helpers for the ornaments: toward white, toward black, more transparent.
static V4 ui_lit(V4 c, F32 t)  { return ui_mix(c, ui_hex(0xffffff, c.w), t); }
static V4 ui_dark(V4 c, F32 t) { return ui_mix(c, ui_hex(0x000000, c.w), t); }
static V4 ui_fade(V4 c, F32 a) { c.w *= a; return c; }
static Rect ui_square(F32 cx, F32 cy, F32 h) { return rect(cx - h, cy - h, cx + h, cy + h); }
static void ui_disc(F32 cx, F32 cy, F32 r, V4 top, V4 bottom) { r_rect_gradient(ui_square(cx, cy, r), top, bottom, r, 0, 0.5f, 0); }

static void ui_panel(Rect r, F32 radius)
{
    UI_Theme *t = &ui.theme;
    F32 o = t->ornate, s = ui.scale;
    if (o <= 0) { r_rect(r, t->panel, radius, 0, 0.5f); return; }
    r_rect(rect(r.x0, r.y0 + 3 * s, r.x1, r.y1 + 3 * s), ui_hex(0x000000, 0.45f * o), radius + 2 * s, 0, 6 * s); // shadow
    r_rect_gradient(r, ui_mix(t->panel, t->hover, 0.5f * o), t->panel, radius, 0, 0.5f, 0);
    ui_aura(r, radius, 0.6f);
}

static void ui_panel_rim(Rect r, F32 radius)
{
    UI_Theme *t = &ui.theme;
    F32 o = t->ornate, s = ui.scale, px = ui_px();
    if (o <= 0) { r_rect(r, t->border, radius, px, 0.5f); return; }
    // Metal: a lit rim, then a dark groove inside it.
    F32 rim = Max(px, 1.5f * s);
    r_rect_gradient(r, ui_mix(t->border, t->trim_hi, 0.75f * o), ui_mix(t->border, t->trim, 0.75f * o), radius, rim, 0.5f, 0);
    Rect in = rect(r.x0 + rim, r.y0 + rim, r.x1 - rim, r.y1 - rim);
    r_rect(in, ui_mix(t->border, t->trim_dark, o), Max(radius - rim, 0), px, 0.5f);
    // The style's piece on each corner.
    F32 k = Max(radius * 0.3f, 2 * s), g = t->style == UI_STYLE_GEARWORK ? 10 * s : 16 * s;
    ui_corner(t, r.x0 + k, r.y0 + k, g, -1, -1);
    ui_corner(t, r.x1 - k, r.y0 + k, g, 1, -1);
    ui_corner(t, r.x0 + k, r.y1 - k, g, -1, 1);
    ui_corner(t, r.x1 - k, r.y1 - k, g, 1, 1);
}

static void ui_aura(Rect r, F32 radius, F32 strength)
{
    UI_Theme *t = &ui.theme;
    if (t->ornate <= 0 || t->aura.w <= 0) return;
    F32 h = Min((r.y1 - r.y0) * 0.45f, 320 * ui.scale);
    V4 none = t->aura;
    none.w = 0;
    r_rect_gradient(rect(r.x0, r.y0, r.x1, r.y0 + h), ui_fade(t->aura, strength), none, radius, 0, 0.5f, 0);
}

// A flame standing on (cx, cy + h / 2), h about half its height. flick stretches it (0: at rest),
// sway bends its tip sideways (-1..1).
static void ui_flame(UI_Theme *t, F32 cx, F32 cy, F32 h, F32 flick, F32 sway)
{
    F32 tip = cy - (1.0f + flick) * h, base = cy + 0.42f * h;
    r_glow(cx, cy + 0.1f * h, 0.45f * h, 0.6f * h, ui_fade(t->gem, 0.4f));
    V4 outer_top = ui_dark(t->gem, 0.35f), outer_bottom = ui_dark(t->gem, 0.1f);
    r_taper(cx - 0.26f * h, base, cx - (0.62f - sway * 0.2f) * h, cy - (0.32f + flick * 0.6f) * h, 0.26f * h, 0, outer_top, outer_bottom);
    r_taper(cx + 0.26f * h, base, cx + (0.62f + sway * 0.2f) * h, cy - (0.28f - flick * 0.3f) * h, 0.26f * h, 0, outer_top, outer_bottom);
    r_taper(cx, base, cx + sway * 0.25f * h, tip, 0.5f * h, 0, ui_dark(t->gem, 0.2f), ui_lit(t->gem, 0.15f));
    r_taper(cx, base + 0.05f * h, cx + sway * 0.15f * h, cy - (0.3f + flick * 0.5f) * h, 0.3f * h, 0, t->spark, ui_lit(t->spark, 0.65f));
}

// A snowflake: six arms with two branches each, over a crystal of the spark's color, turned by angle.
static void ui_snowflake(UI_Theme *t, F32 cx, F32 cy, F32 h, F32 angle)
{
    F32 px = ui_px(), w = Max(px * 0.6f, 0.085f * h);
    r_glow(cx, cy, 0.45f * h, 0.5f * h, ui_fade(t->gem, 0.35f));
    r_star(ui_square(cx, cy, 0.62f * h), ui_lit(t->spark, 0.25f), t->spark, 6, 2.6f, angle + 3.14159f / 6, 0);
    V4 top = ui_lit(t->gem, 0.65f), bottom = ui_lit(t->gem, 0.15f);
    for (int k = 0; k < 6; k += 1) {
        F32 a = angle + k * 1.0471976f, ux = sinf(a), uy = -cosf(a);
        r_taper(cx, cy, cx + ux * h, cy + uy * h, w, w * 0.4f, top, bottom);
        F32 bx = cx + ux * 0.55f * h, by = cy + uy * 0.55f * h;
        for (F32 sd = -1; sd <= 1; sd += 2) {
            F32 b = a + sd * 0.8f;
            r_taper(bx, by, bx + sinf(b) * 0.32f * h, by - cosf(b) * 0.32f * h, w * 0.85f, w * 0.35f, top, bottom);
        }
    }
    r_star(ui_square(cx, cy, 0.24f * h), t->trim_hi, ui_lit(t->gem, 0.4f), 6, 6, angle + 3.14159f / 6, 0);
}

// A cut stone (a disc) with a light on its upper left.
static void ui_orb(F32 cx, F32 cy, F32 r, V4 color)
{
    ui_disc(cx, cy, r, ui_lit(color, 0.3f), ui_dark(color, 0.45f));
    F32 hr = r * 0.32f;
    ui_disc(cx - r * 0.32f, cy - r * 0.36f, hr, ui_hex(0xffffff, 0.7f * color.w), ui_hex(0xffffff, 0.1f * color.w));
}

// A ring of metal in the trim's colors.
static void ui_ring(UI_Theme *t, F32 cx, F32 cy, F32 r, F32 thick)
{
    r_rect_gradient(ui_square(cx, cy, r), t->trim_hi, t->trim, r, thick, 0.5f, 0);
}

static void ui_emblem(UI_Theme *t, F32 cx, F32 cy, F32 size)
{
    F32 h = size * 0.5f, s = ui.scale, px = ui_px();
    if (t->ornate <= 0) { ui_gem(cx, cy, size, t->gem); return; }
    switch (t->style) {
    default:
    case UI_STYLE_GEARWORK:
        ui_gem(cx, cy, size, t->gem);
        break;
    case UI_STYLE_VOID:
        r_glow(cx, cy, 0.6f * h, 0.5f * h, ui_fade(t->gem, 0.5f));
        ui_disc(cx, cy, 0.6f * h, ui_dark(t->gem, 0.5f), ui_dark(t->gem, 0.85f)); // the orb, swallowing light
        r_crescent(ui_square(cx, cy, h), t->trim_hi, t->trim, 0.84f, 0.3f, -0.8f, 0);
        r_star(ui_square(cx - 0.1f * h, cy + 0.05f * h, 0.32f * h), ui_lit(t->spark, 0.4f), t->spark, 4, 2.2f, 0, 0);
        r_star(ui_square(cx + 0.66f * h, cy - 0.66f * h, 0.2f * h), ui_lit(t->spark, 0.4f), t->spark, 4, 2.2f, 0, 0);
        break;
    case UI_STYLE_FROST:
        ui_snowflake(t, cx, cy, h, 0);
        break;
    case UI_STYLE_FLAME:
        ui_flame(t, cx, cy + 0.08f * h, 0.92f * h, 0, 0);
        ui_disc(cx - 0.72f * h, cy - 0.5f * h, Max(px, 0.08f * h), t->spark, t->spark);
        ui_disc(cx + 0.78f * h, cy - 0.72f * h, Max(px, 0.06f * h), t->spark, t->spark);
        break;
    case UI_STYLE_FEL: {
        F32 oy = cy + 0.18f * h;
        r_glow(cx, oy, 0.5f * h, 0.5f * h, ui_fade(t->gem, 0.5f));
        for (F32 sd = -1; sd <= 1; sd += 2) { // horns: out, then up
            r_taper(cx + sd * 0.3f * h, oy - 0.25f * h, cx + sd * 0.7f * h, cy - 0.45f * h, 0.17f * h, 0.11f * h, t->trim_hi, t->trim);
            r_taper(cx + sd * 0.7f * h, cy - 0.45f * h, cx + sd * 0.6f * h, cy - 1.0f * h, 0.11f * h, 0, t->trim_hi, t->trim_hi);
        }
        ui_disc(cx, oy, 0.52f * h, ui_lit(t->gem, 0.25f), ui_dark(t->gem, 0.4f)); // the eye
        r_rect(ui_square(cx, oy, 0.52f * h), t->spark, 0.52f * h, Max(px, 1.1f * s), 0.5f);
        r_leaf(cx, oy, 0.1f * h, 0.34f * h, 0, ui_dark(t->gem, 0.85f), ui_dark(t->gem, 0.9f), 0); // its slit
        break;
    }
    case UI_STYLE_BLOOD:
        for (F32 sd = -1; sd <= 1; sd += 2) { // two swords, crossed
            F32 hx = cx - sd * 0.5f * h, hy = cy + 0.5f * h;
            r_taper(hx, hy, cx + sd * 0.95f * h, cy - 0.95f * h, 0.15f * h, 0.02f * h, t->trim_hi, t->trim);
            r_taper(hx, hy, hx - sd * 0.3f * h, hy + 0.3f * h, 0.06f * h, 0.07f * h, ui_lit(t->trim_dark, 0.25f), t->trim_dark);
            r_taper(hx - 0.25f * h, hy - sd * 0.25f * h, hx + 0.25f * h, hy + sd * 0.25f * h, 0.07f * h, 0.07f * h, t->spark, ui_dark(t->spark, 0.3f));
            ui_disc(hx - sd * 0.36f * h, hy + 0.36f * h, Max(px, 0.09f * h), t->spark, ui_dark(t->spark, 0.3f));
        }
        ui_orb(cx, cy, 0.3f * h, t->gem);
        ui_ring(t, cx, cy, 0.34f * h, Max(px, 0.9f * s));
        break;
    case UI_STYLE_HOLY:
        r_glow(cx, cy, 0.5f * h, 0.6f * h, ui_fade(t->spark, 0.4f));
        r_star(ui_square(cx, cy, h), t->trim_hi, t->trim, 8, 2.1f, 0, 0);
        r_star(ui_square(cx, cy, 0.7f * h), ui_lit(t->trim_hi, 0.3f), t->trim_hi, 8, 2.15f, 3.14159f / 8, 0);
        ui_disc(cx, cy, 0.36f * h, ui_lit(t->gem, 0.5f), ui_mix(t->gem, t->spark, 0.4f));
        r_rect(ui_square(cx, cy, 0.36f * h), t->trim, 0.36f * h, Max(px, 0.8f * s), 0.5f);
        break;
    case UI_STYLE_NATURE:
        r_taper(cx, cy + 0.6f * h, cx, cy - 0.05f * h, 0.06f * h, 0.05f * h, t->trim_hi, t->trim); // the stem
        r_leaf(cx - 0.42f * h, cy - 0.02f * h, 0.24f * h, 0.5f * h, -0.95f, ui_lit(t->gem, 0.2f), ui_dark(t->gem, 0.35f), 0);
        r_leaf(cx + 0.42f * h, cy - 0.02f * h, 0.24f * h, 0.5f * h, 0.95f, ui_lit(t->gem, 0.2f), ui_dark(t->gem, 0.35f), 0);
        r_leaf(cx, cy - 0.4f * h, 0.27f * h, 0.6f * h, 0, ui_lit(t->gem, 0.35f), ui_dark(t->gem, 0.2f), 0);
        r_taper(cx, cy + 0.1f * h, cx, cy - 0.85f * h, Max(px * 0.5f, 0.035f * h), 0, ui_dark(t->gem, 0.5f), ui_dark(t->gem, 0.4f)); // a vein
        ui_orb(cx - 0.16f * h, cy + 0.55f * h, 0.17f * h, t->spark);
        ui_orb(cx + 0.17f * h, cy + 0.62f * h, 0.15f * h, t->spark);
        break;
    case UI_STYLE_STORM: {
        r_glow(cx, cy, 0.5f * h, 0.5f * h, ui_fade(t->gem, 0.5f));
        ui_disc(cx, cy, 0.56f * h, ui_lit(t->gem, 0.3f), ui_dark(t->gem, 0.4f));
        ui_ring(t, cx, cy, 0.8f * h, Max(px, 1.4f * s));
        // A bolt across it, outlined so it reads on the orb.
        F32 bx[4] = { 0.3f, -0.18f, 0.16f, -0.28f }, by[4] = { -1.0f, -0.06f, 0.0f, 1.0f }, br[4] = { 0.04f, 0.15f, 0.13f, 0.0f };
        V4 edge = ui_dark(t->gem, 0.75f);
        for (int pass = 0; pass < 2; pass += 1) {
            F32 grow = pass ? 0 : Max(px, 0.8f * s);
            for (int k = 0; k < 3; k += 1) {
                r_taper(cx + bx[k] * h, cy + by[k] * h, cx + bx[k + 1] * h, cy + by[k + 1] * h, br[k] * h + grow, br[k + 1] * h + grow,
                        pass ? ui_lit(t->spark, 0.5f) : edge, pass ? t->spark : edge);
            }
        }
        break;
    }
    }
}

static void ui_corner(UI_Theme *t, F32 cx, F32 cy, F32 size, F32 dx, F32 dy)
{
    F32 h = size * 0.5f, px = ui_px();
    if (t->ornate <= 0) return;
    switch (t->style) {
    default:
    case UI_STYLE_GEARWORK:
        ui_gem(cx, cy, size, t->trim_hi);
        break;
    case UI_STYLE_VOID:
        ui_disc(cx, cy, 0.42f * h, ui_dark(t->gem, 0.35f), ui_dark(t->gem, 0.8f));
        r_crescent(ui_square(cx, cy, 0.78f * h), t->trim_hi, t->trim, 0.85f, 0.32f, atan2f(-dy, -dx), 0); // bulging outward
        break;
    case UI_STYLE_FROST:
        r_taper(cx - dx * 0.2f * h, cy - dy * 0.2f * h, cx + dx * 1.25f * h, cy + dy * 1.25f * h, 0.32f * h, 0, ui_lit(t->gem, 0.55f), t->gem);
        r_star(ui_square(cx, cy, 0.6f * h), t->trim_hi, ui_lit(t->gem, 0.2f), 4, 2.2f, 0, 0);
        break;
    case UI_STYLE_FLAME:
        ui_disc(cx, cy, 0.5f * h, t->trim_hi, t->trim_dark);
        r_glow(cx, cy, 0.25f * h, 0.45f * h, ui_fade(t->gem, 0.7f));
        ui_disc(cx, cy, 0.26f * h, t->spark, t->gem); // an ember set in iron
        break;
    case UI_STYLE_FEL:
        r_taper(cx, cy, cx + dx * 0.75f * h, cy + dy * 0.3f * h, 0.3f * h, 0.18f * h, t->trim_hi, t->trim);
        r_taper(cx + dx * 0.75f * h, cy + dy * 0.3f * h, cx + dx * 0.95f * h, cy + dy * 1.2f * h, 0.18f * h, 0, t->trim_hi, t->trim);
        ui_orb(cx, cy, 0.34f * h, t->gem);
        break;
    case UI_STYLE_BLOOD:
        r_taper(cx, cy, cx + dx * 1.35f * h, cy + dy * 1.35f * h, 0.3f * h, 0, t->trim_hi, t->trim);
        r_taper(cx, cy, cx + dx * 1.1f * h, cy, 0.24f * h, 0, t->trim_hi, t->trim);
        r_taper(cx, cy, cx, cy + dy * 1.1f * h, 0.24f * h, 0, t->trim_hi, t->trim);
        ui_orb(cx, cy, 0.38f * h, t->gem);
        break;
    case UI_STYLE_HOLY:
        r_star(ui_square(cx, cy, h), t->trim_hi, t->trim, 4, 2.0f, 0, 0);
        r_star(ui_square(cx, cy, 0.6f * h), ui_lit(t->trim_hi, 0.3f), t->trim, 4, 2.0f, 3.14159f / 4, 0);
        ui_disc(cx, cy, Max(px, 0.18f * h), ui_lit(t->gem, 0.5f), t->gem);
        break;
    case UI_STYLE_NATURE:
        r_leaf(cx + dx * 0.4f * h, cy + dy * 0.4f * h, 0.3f * h, 0.72f * h, atan2f(dx, -dy), ui_lit(t->gem, 0.25f), ui_dark(t->gem, 0.3f), 0);
        ui_orb(cx - dx * 0.1f * h, cy - dy * 0.1f * h, 0.28f * h, t->spark);
        break;
    case UI_STYLE_STORM:
        ui_disc(cx, cy, 0.55f * h, t->trim_hi, t->trim_dark);
        r_glow(cx, cy, 0.25f * h, 0.4f * h, ui_fade(t->gem, 0.7f));
        ui_disc(cx, cy, 0.28f * h, ui_lit(t->gem, 0.45f), t->gem);
        break;
    }
}

static void ui_divider(F32 x0, F32 x1, F32 y)
{
    UI_Theme *t = &ui.theme;
    F32 o = t->ornate, s = ui.scale, px = ui_px();
    if (o <= 0 || x1 - x0 < 60 * s) { r_rect(rect(x0, y - px * 0.5f, x1, y + px * 0.5f), t->border, 0, 0, 0); return; }
    B32 gears = t->style == UI_STYLE_GEARWORK;
    F32 cx = (x0 + x1) * 0.5f, gap = gears ? 11 * s : 19 * s, fade = Min(160 * s, (cx - gap - x0) * 0.6f);
    V4 metal = ui_mix(t->border, t->trim, 0.85f * o), none = metal;
    none.w = 0;
    F32 h = Max(px, 1 * s), y0 = y - h * 0.5f;
    r_rect_gradient(rect(x0, y0, x0 + fade, y0 + h), none, metal, 0, 0, 0, 1);
    r_rect(rect(x0 + fade, y0, cx - gap, y0 + h), metal, 0, 0, 0);
    r_rect(rect(cx + gap, y0, x1 - fade, y0 + h), metal, 0, 0, 0);
    r_rect_gradient(rect(x1 - fade, y0, x1, y0 + h), metal, none, 0, 0, 0, 1);
    // The style's motifs either side of the emblem (sd: -1 left, 1 right).
    for (F32 sd = -1; sd <= 1; sd += 2) {
        F32 g = cx + sd * gap;
        switch (t->style) {
        default:
        case UI_STYLE_GEARWORK:
            r_diamond(rect(g - sd * 4 * s - 2 * s, y - 2 * s, g - sd * 4 * s + 2 * s, y + 2 * s), metal, metal, 0);
            break;
        case UI_STYLE_VOID:
            ui_disc(g, y, 1.7f * s, t->spark, ui_dark(t->spark, 0.3f));
            ui_disc(g + sd * 7 * s, y, 1.1f * s, ui_fade(t->spark, 0.7f), ui_fade(t->spark, 0.7f));
            break;
        case UI_STYLE_FROST:
            r_taper(g - sd * 3 * s, y, g + sd * 9 * s, y, 2.0f * s, 0, ui_lit(t->gem, 0.5f), t->gem);
            break;
        case UI_STYLE_FLAME:
            r_glow(g, y, 1.4f * s, 2.5f * s, ui_fade(t->gem, 0.6f));
            ui_disc(g, y, 1.5f * s, t->spark, t->gem);
            ui_disc(g + sd * 7 * s, y - 2 * s, 1.0f * s, ui_fade(t->spark, 0.8f), ui_fade(t->gem, 0.8f));
            break;
        case UI_STYLE_FEL:
            r_star(ui_square(g, y, 3.2f * s), ui_lit(t->spark, 0.3f), t->spark, 4, 2.0f, 0, 0);
            break;
        case UI_STYLE_BLOOD:
            r_taper(g - sd * 3 * s, y, g + sd * 10 * s, y, 2.4f * s, 0, t->trim_hi, t->trim);
            break;
        case UI_STYLE_HOLY:
            r_star(ui_square(g, y, 3.4f * s), t->trim_hi, t->trim, 4, 2.0f, 0, 0);
            break;
        case UI_STYLE_NATURE:
            r_leaf(g + sd * 2 * s, y, 1.8f * s, 4.6f * s, sd * 1.5708f, ui_lit(t->gem, 0.25f), ui_dark(t->gem, 0.3f), 0);
            break;
        case UI_STYLE_STORM:
            ui_disc(g, y, 2.2f * s, t->trim_hi, t->trim_dark);
            ui_disc(g, y, 1.0f * s, ui_lit(t->gem, 0.4f), t->gem);
            break;
        }
    }
    ui_emblem(t, cx, y, gears ? 11 * s : 20 * s);
}

static void ui_gem(F32 cx, F32 cy, F32 size, V4 color)
{
    F32 s = ui.scale, h = size * 0.5f, w = h * 0.82f, e = Max(ui_px(), 1.2f * s);
    V4 white = ui_hex(0xffffff, color.w), black = ui_hex(0x000000, color.w);
    r_diamond(rect(cx - w, cy - h, cx + w, cy + h), ui_mix(color, black, 0.45f), ui_mix(color, black, 0.70f), 0); // setting
    r_diamond(rect(cx - w + e, cy - h + e, cx + w - e, cy + h - e), ui_mix(color, white, 0.40f), ui_mix(color, black, 0.30f), 0);
    // The lit facet, upper left.
    F32 fw = (w - e) * 0.55f, fh = (h - e) * 0.55f, fx = cx - fw * 0.55f, fy = cy - fh * 0.55f;
    r_diamond(rect(fx - fw * 0.5f, fy - fh * 0.5f, fx + fw * 0.5f, fy + fh * 0.5f), ui_hex(0xffffff, 0.75f * color.w), ui_hex(0xffffff, 0.15f * color.w), 0);
}

static void ui_cog(F32 cx, F32 cy, F32 r, F32 angle, V4 color)
{
    V4 white = ui_hex(0xffffff, color.w), black = ui_hex(0x000000, color.w);
    Rect b = rect(cx - r, cy - r, cx + r, cy + r);
    F32 teeth = r < 10 * ui.scale ? 8.0f : 10.0f;
    r_cog(b, ui_mix(color, white, 0.25f), ui_mix(color, black, 0.25f), teeth, 0.76f, angle, r * 0.32f, 0);
    r_cog(b, ui_mix(color, black, 0.55f), ui_mix(color, black, 0.55f), teeth, 0.76f, angle, r * 0.32f, Max(ui_px(), r * 0.09f));
}

static B32 ui_spinner(F32 cx, F32 cy, F32 r)
{
    UI_Theme *t = &ui.theme;
    F64 ms = (F64)os_now_ms();
    F32 a = (F32)fmod(ms / 2400.0, 1.0) * 6.2831853f, T = (F32)fmod(ms / 1000.0, 1000.0), tau = 6.2831853f;
    if (t->ornate <= 0) { ui_cog(cx, cy, r, a, t->accent); return 1; }
    switch (t->style) {
    default:
    case UI_STYLE_GEARWORK:
        ui_cog(cx, cy, r, a, t->trim_hi);
        break;
    case UI_STYLE_VOID:
        ui_disc(cx, cy, 0.55f * r, ui_dark(t->gem, 0.5f), ui_dark(t->gem, 0.85f));
        r_crescent(ui_square(cx, cy, r), t->trim_hi, t->trim, 0.84f, 0.3f, a, 0);
        ui_disc(cx + cosf(-2 * a) * 0.95f * r, cy + sinf(-2 * a) * 0.95f * r, 0.16f * r, t->spark, t->spark);
        break;
    case UI_STYLE_FROST:
        ui_snowflake(t, cx, cy, r, a * 0.5f);
        break;
    case UI_STYLE_FLAME:
        ui_flame(t, cx, cy + 0.12f * r, 0.85f * r, 0.12f * sinf(T * 9) + 0.07f * sinf(T * 23), 0.3f * sinf(T * 5));
        break;
    case UI_STYLE_FEL:
        r_glow(cx, cy, 0.4f * r, 0.5f * r, ui_fade(t->gem, 0.5f));
        r_star(ui_square(cx, cy, 0.88f * r), ui_lit(t->gem, 0.4f), t->gem, 5, 2.3f, a, 0);
        r_rect(ui_square(cx, cy, r), t->spark, r, Max(ui_px(), 0.13f * r), 0.5f);
        break;
    case UI_STYLE_BLOOD:
        for (int k = 0; k < 3; k += 1) { // a whirlwind of blades
            F32 ang = a * 1.6f + k * tau / 3;
            F32 mx = cx + cosf(ang) * 0.55f * r, my = cy + sinf(ang) * 0.55f * r;
            r_taper(cx, cy, mx, my, 0.22f * r, 0.17f * r, t->trim_hi, t->trim);
            r_taper(mx, my, cx + cosf(ang + 0.55f) * r, cy + sinf(ang + 0.55f) * r, 0.17f * r, 0, t->trim_hi, t->trim);
        }
        ui_orb(cx, cy, 0.3f * r, t->gem);
        break;
    case UI_STYLE_HOLY:
        r_glow(cx, cy, 0.45f * r, 0.5f * r, ui_fade(t->spark, 0.4f));
        r_star(ui_square(cx, cy, r), t->trim_hi, t->trim, 12, 3.0f, a * 0.5f, 0);
        ui_disc(cx, cy, 0.38f * r, ui_lit(t->gem, 0.5f), ui_mix(t->gem, t->spark, 0.4f));
        break;
    case UI_STYLE_NATURE:
        for (int k = 0; k < 3; k += 1) { // leaves in a pinwheel
            F32 ang = a + k * tau / 3, ux = cosf(ang), uy = sinf(ang);
            r_leaf(cx + ux * 0.5f * r, cy + uy * 0.5f * r, 0.22f * r, 0.5f * r, atan2f(ux, -uy) + 0.4f, ui_lit(t->gem, 0.25f), ui_dark(t->gem, 0.3f), 0);
        }
        ui_orb(cx, cy, 0.24f * r, t->spark);
        break;
    case UI_STYLE_STORM:
        ui_disc(cx, cy, 0.3f * r, t->trim_hi, t->trim_dark);
        for (int k = 0; k < 3; k += 1) { // orbs circling, crackling
            F32 ang = a * 1.5f + k * tau / 3, ox = cx + cosf(ang) * 0.68f * r, oy = cy + sinf(ang) * 0.68f * r;
            r_glow(ox, oy, 0.15f * r, 0.25f * r, ui_fade(t->gem, 0.6f));
            ui_disc(ox, oy, 0.22f * r, ui_lit(t->gem, 0.5f), t->gem);
        }
        break;
    }
    return 1;
}

static void ui_rivet(F32 cx, F32 cy, F32 r)
{
    UI_Theme *t = &ui.theme;
    Rect b = rect(cx - r, cy - r, cx + r, cy + r);
    r_rect_gradient(b, t->trim_hi, t->trim_dark, r, 0, 0.5f, 0);
    r_rect(b, ui_hex(0x000000, 0.5f), r, Max(ui_px() * 0.75f, r * 0.2f), 0.5f);
    F32 h = r * 0.35f;
    r_rect(rect(cx - r * 0.45f - h, cy - r * 0.45f - h, cx - r * 0.45f + h, cy - r * 0.45f + h), ui_hex(0xffffff, 0.6f), h, 0, 0.5f);
}

//- Widgets

static void ui_text_centered(F_Font *f, Rect r, String8 s, V4 color)
{
    F32 tw = f_text_width(f, s);
    f_draw_text(f, (r.x0 + r.x1 - tw) * 0.5f, (r.y0 + r.y1 - f->line_height) * 0.5f, s, color, 0);
}

static void ui_pill(Rect r, String8 label, F_Font *f, V4 fg, V4 bg)
{
    r_rect(r, bg, (r.y1 - r.y0) * 0.5f, 0, 0.5f);
    ui_text_centered(f, r, label, fg);
}

static B32 ui_button(Rect r, String8 label, V4 fg, V4 bg, V4 bg_hot)
{
    UI_Id id = ui_id(label);
    UI_Signal sg = ui_signal(id, r, UI_FOCUSABLE);
    B32 in_clip = ui_mouse_in(ui.clip);
    ui_plate(r, ui_mix(bg, bg_hot, ui_anim(id, (F32)(sg.hovering && in_clip))), 6 * ui.scale);
    ui_text_centered(ui.font.normal, r, ui_label_text(label), fg);
    ui_focus_ring(id, r, 6 * ui.scale);
    return sg.clicked && (sg.keyboard || in_clip);
}

static F32 ui_button_width(String8 label) { return f_text_width(ui.font.normal, ui_label_text(label)) + 24 * ui.scale; }

static B32 ui_checkbox(String8 name, F32 x, F32 cy, B32 checked)
{
    F32 s = ui.scale, sz = 16 * s;
    Rect b = rect(x, (F32)(int)(cy - sz * 0.5f), x + sz, (F32)(int)(cy - sz * 0.5f) + sz);
    Rect hit = rect(b.x0 - 6 * s, b.y0 - 6 * s, b.x1 + 6 * s, b.y1 + 6 * s); // easier to hit than it looks
    UI_Id id = ui_id(name);
    UI_Signal sg = ui_signal(id, hit, UI_FOCUSABLE);
    B32 in_clip = ui_mouse_in(ui.clip);
    B32 hot = sg.hovering && in_clip;
    if (checked) {
        ui_plate(b, hot ? ui.theme.accent_hi : ui.theme.accent, 4 * s);
        ui_text_centered(ui.font.icon, b, str8_lit("\xE2\x9C\x93"), ui.theme.text); // U+2713 check mark
    } else {
        r_rect(b, hot ? ui.theme.faint : ui.theme.border_hi, 4 * s, 1.5f * s, 0.5f);
    }
    ui_focus_ring(id, b, 4 * s);
    return sg.clicked && (sg.keyboard || in_clip);
}

static F32 ui_text_highlighted(F_Font *f, F32 x, F32 y, String8 text, U8 *marks, U32 mark_count, V4 base, V4 hi, F32 max_w)
{
    U8 mark[256] = { 0 };
    for (U32 i = 0; i < mark_count; i += 1) mark[marks[i]] = 1;
    F32 x0 = x;
    U64 i = 0;
    while (i < text.size) {
        B32 on = i < 256 && mark[i];
        U64 j = i + 1;
        // A run never splits a UTF-8 sequence: continuation bytes stay with their lead byte.
        while (j < text.size && (((text.str[j] & 0xC0) == 0x80) || ((j < 256 && mark[j]) == on))) j += 1;
        F32 left = max_w - (x - x0);
        if (left <= 0) break;
        x = f_draw_text(f, x, y, str8_substr(text, i, j), on ? hi : base, left);
        i = j;
    }
    return x;
}

// Word-wrapped text: '\n' ends a line, a blank line is a paragraph gap. Only lines inside
// [vis_y0, vis_y1) are drawn, so long texts in scrolling panels stay cheap. Returns the height.
static F32 ui_text_wrapped(F_Font *f, F32 x, F32 y, F32 w, String8 text, V4 color, F32 vis_y0, F32 vis_y1)
{
    F32 lh = f->line_height, y0 = y, space_w = f_text_width(f, str8_lit(" "));
    String8 rest = text;
    while (rest.size) {
        String8 para = str8_pop_line(&rest);
        if (!para.size) { y += lh * 0.6f; continue; }
        // A bullet line wraps under its text, not under the bullet.
        String8 bullet = str8_lit("\xE2\x80\xA2 ");
        F32 indent = str8_starts_with(para, bullet, 0) ? f_text_width(f, bullet) : 0;
        F32 lx = 0;
        U64 line_start = 0, last_end = 0;
        B32 visible = y + lh > vis_y0 && y < vis_y1;
        for (U64 i = 0; i <= para.size;) {
            U64 j = i;
            while (j < para.size && para.str[j] != ' ') j += 1;
            F32 ww = f_text_width(f, str8_substr(para, i, j));
            F32 avail = w - (line_start ? indent : 0);
            if (lx > 0 && lx + space_w + ww > avail) {
                // This word starts the next line.
                if (visible) f_draw_text(f, x + (line_start ? indent : 0), y, str8_substr(para, line_start, last_end), color, avail);
                y += lh;
                visible = y + lh > vis_y0 && y < vis_y1;
                line_start = i;
                lx = 0;
            }
            lx += (lx > 0 ? space_w : 0) + ww;
            last_end = j;
            if (j >= para.size) break;
            i = j + 1;
        }
        if (visible) f_draw_text(f, x + (line_start ? indent : 0), y, str8_substr(para, line_start, para.size), color, w - (line_start ? indent : 0));
        y += lh;
    }
    return y - y0;
}

static B32 ui_loading_bar(Rect r)
{
    F32 w = r.x1 - r.x0, seg = w * 0.25f;
    F32 t = (F32)fmod((F64)os_now_ms() / 1100.0, 1.0);
    F32 x0 = r.x0 + (w + seg) * t - seg;
    Rect bar = rect(Max(x0, r.x0), r.y0, Min(x0 + seg, r.x1), r.y0 + 2 * ui.scale);
    if (bar.x1 > bar.x0) r_rect(bar, ui.theme.accent, 0, 0, 0);
    return 1;
}

static void ui_tooltip(Rect anchor, String8 text)
{
    ui.tip = str8_copy(ui.frame_arena, text);
    ui.tip_anchor = anchor;
}

static void ui_tooltip_draw(void)
{
    if (!ui.tip.size || !ui.in.inside) return;
    F32 s = ui.scale, pad = 8 * s;
    F_Font *f = ui.font.caption;
    F32 w = f_text_width(f, ui.tip) + 2 * pad, h = f->line_height + 2 * pad * 0.75f;
    F32 cx = (ui.tip_anchor.x0 + ui.tip_anchor.x1) * 0.5f;
    F32 x = Clamp(cx - w * 0.5f, 4 * s, ui.width - 4 * s - w);
    F32 y = ui.tip_anchor.y1 + 6 * s;
    if (y + h > ui.height - 4 * s) y = ui.tip_anchor.y0 - 6 * s - h;
    Rect r = rect((F32)(int)x, (F32)(int)y, (F32)(int)x + w, (F32)(int)y + h);
    r_set_clip(rect(0, 0, ui.width, ui.height));
    r_rect(rect(r.x0, r.y0 + 2 * s, r.x1, r.y1 + 4 * s), ui_hex(0x000000, 0.45f), 6 * s, 0, 6 * s);
    r_rect(r, ui.theme.panel, 5 * s, 0, 0.5f);
    r_rect(r, ui.theme.border_hi, 5 * s, ui_px(), 0.5f);
    f_draw_text(f, r.x0 + pad, r.y0 + pad * 0.75f, ui.tip, ui.theme.text, 0);
}

static UI_Signal ui_drag_handle(UI_Id id, Rect zone)
{
    UI_Signal sg = ui_signal(id, zone, 0);
    if (sg.dragging || sg.hovering) ui.cursor = UI_CURSOR_SIZE_WE;
    return sg;
}

// The scrollbar: a strip along the list's right edge, and the thumb in it (y0..y1 of the strip).
static Rect ui_scroll_track(Rect list) { return rect(list.x1 - 14 * ui.scale, list.y0, list.x1, list.y1); }

static Rect ui_scroll_thumb(UI_Scroll *sc, Rect list, F32 content_h)
{
    F32 view_h = list.y1 - list.y0;
    F32 thumb_h = Min(Max(view_h * view_h / content_h, 28 * ui.scale), view_h);
    F32 t = content_h > view_h ? sc->pos / (content_h - view_h) : 0;
    F32 ty = list.y0 + t * (view_h - thumb_h);
    Rect track = ui_scroll_track(list);
    return rect(track.x0, ty, track.x1, ty + thumb_h);
}

static B32 ui_scroll_list(UI_Scroll *sc, Rect list, F32 content_h)
{
    UI_Id id = ui_id_ptr(sc); // the scrollbar, while its thumb is dragged
    F32 view_h = list.y1 - list.y0;
    F32 max_scroll = Max(0, content_h - view_h);
    if (ui_mouse_in(list) && ui.in.wheel != 0) sc->target -= ui.in.wheel * 48 * ui.scale * 3;

    // The scrollbar: press on the thumb and drag it; press beside it and the thumb jumps there (and
    // drags from there). The press is the scrollbar's, so the rows under it never see a click, and
    // while the mouse is over the scrollbar the rows don't light up.
    if (max_scroll > 0) {
        Rect track = ui_scroll_track(list);
        Rect thumb = ui_scroll_thumb(sc, list, content_h);
        F32 room = view_h - (thumb.y1 - thumb.y0);
        if (ui.in.pressed && ui_mouse_in(track) && point_in(track, ui.in.press_x, ui.in.press_y)) {
            if (!point_in(thumb, ui.in.press_x, ui.in.press_y)) {
                F32 t = room > 0 ? Clamp(0, (ui.in.my - (thumb.y1 - thumb.y0) * 0.5f - list.y0) / room, 1) : 0;
                sc->pos = sc->target = t * max_scroll;
                thumb = ui_scroll_thumb(sc, list, content_h);
            }
            ui.active = id;
            ui.drag_grab = ui.in.my - thumb.y0;
            ui.in.press_x = ui.in.press_y = -1e9f; // taken: no other widget gets this click
        }
        B32 dragging = ui_is_active(id) && ui.in.down;
        if (dragging && room > 0) {
            F32 t = Clamp(0, (ui.in.my - ui.drag_grab - list.y0) / room, 1);
            sc->pos = sc->target = t * max_scroll;
        }
        if (dragging || ui_mouse_in(track)) ui_block_mouse(dragging ? rect(-1e9f, -1e9f, 1e9f, 1e9f) : track);
    }

    sc->target = Clamp(0, sc->target, max_scroll);
    sc->pos = Clamp(0, sc->pos, max_scroll);
    F32 d = sc->target - sc->pos;
    if (fabsf(d) < 0.5f) { sc->pos = sc->target; return 0; }
    sc->pos += d * 0.3f;
    return 1;
}

static void ui_scrollbar(UI_Scroll *sc, Rect list, F32 content_h)
{
    F32 view_h = list.y1 - list.y0, s = ui.scale;
    if (content_h <= view_h) return;
    Rect thumb = ui_scroll_thumb(sc, list, content_h);
    Rect track = ui_scroll_track(list);
    // Thin until the mouse comes near; wider and brighter while hovered or dragged.
    B32 dragging = ui_is_active(ui_id_ptr(sc)) && ui.in.down;
    B32 hot = dragging || (ui.in.inside && point_in(track, ui.in.mx, ui.in.my));
    F32 w = hot ? 8 * s : 4 * s, x1 = list.x1 - 4 * s;
    if (hot) r_rect(rect(x1 - w, list.y0 + 2 * s, x1, list.y1 - 2 * s), ui_hex(0xffffff, 0.04f), w * 0.5f, 0, 0.5f);
    r_rect(rect(x1 - w, thumb.y0 + 2 * s, x1, thumb.y1 - 2 * s), hot ? ui.theme.dim : ui.theme.faint, w * 0.5f, 0, 0.5f);
}

//- Overlay

static void ui_overlay_set(Rect r, B32 on) { ui.overlay = r; ui.overlay_on = on; }

static void ui_overlay_begin(void)
{
    r_set_clip(rect(0, 0, ui.width, ui.height));
    ui.in_overlay = 1;
    ui.clip = ui.overlay;
}

static void ui_overlay_end(void) { ui.in_overlay = 0; }

//- Text editing

static String8 ui_text(UI_TextBuf *t) { return str8((U8 *)t->s, t->len); }

static void ui_text_insert(UI_TextBuf *t, U32 cp)
{
    U8 buf[4];
    U32 n = utf8_encode(buf, cp);
    if (t->len + n >= sizeof(t->s) - 1) return;
    memcpy(t->s + t->len, buf, n);
    t->len += n;
}

static void ui_text_backspace(UI_TextBuf *t)
{
    while (t->len && ((U8)t->s[t->len - 1] & 0xC0) == 0x80) t->len -= 1;
    if (t->len) t->len -= 1;
}

static void ui_text_clear(UI_TextBuf *t)
{
    os_secure_zero(t->s, sizeof(t->s));
    t->len = 0;
}

static void ui_text_paste(UI_TextBuf *t, String8 utf8)
{
    for (U64 i = 0; i < utf8.size;) {
        UnicodeDecode d = utf8_decode(utf8.str + i, utf8.size - i);
        if (d.codepoint >= 0x20) ui_text_insert(t, d.codepoint);
        i += d.advance ? d.advance : 1;
    }
}

static B32 ui_text_edit(UI_TextBuf *t, WM_Event *e)
{
    if (e->kind == WM_EVENT_TEXT) { ui_text_insert(t, e->codepoint); return 1; }
    B32 ctrl = (e->mods & WM_MOD_CTRL) != 0;
    if (e->key == WM_KEY_BACKSPACE) {
        if (ctrl) t->len = 0;
        else ui_text_backspace(t);
        return 1;
    }
    if (ctrl && e->key == 'V') { ui_text_paste(t, os_clipboard_text(ui.frame_arena)); return 1; }
    return 0;
}

static void ui_text_field(Rect r, UI_TextBuf *t, String8 hint, B32 masked, B32 caret, V4 border)
{
    F32 s = ui.scale;
    r_rect(r, ui.theme.bg, 6 * s, 0, 0.5f);
    r_rect(r, border, 6 * s, ui_px(), 0.5f);
    String8 q = ui_text(t);
    if (masked && q.size) {
        // Never draw a secret: one dot per character.
        U64 n = Min(q.size, 64);
        U8 *dots = push_array_nz(ui.frame_arena, U8, n * 3 + 1);
        for (U64 i = 0; i < n; i += 1) memcpy(dots + i * 3, "\xE2\x80\xA2", 3);
        q = str8(dots, n * 3);
    }
    F_Font *f = ui.font.normal;
    F32 ty = (r.y0 + r.y1 - f->line_height) * 0.5f;
    F32 cx = r.x0 + 12 * s;
    if (q.size) cx = f_draw_text(f, r.x0 + 12 * s, ty, q, ui.theme.text, r.x1 - r.x0 - 24 * s) + 1 * s;
    else f_draw_text(f, r.x0 + 16 * s, ty, hint, ui.theme.faint, r.x1 - r.x0 - 32 * s);
    if (caret) r_rect(rect(cx, ty + 2 * s, cx + ui_px(), ty + f->line_height - 2 * s), ui.theme.accent, 0, 0, 0);
}

//- Frames

#define UI_ATLAS_SIZE 2048 // the glyph atlas (R8)

static B32 ui_init(void)
{
    if (!ui.frame_arena) ui.frame_arena = arena_alloc(GB(1));
    ui.theme = ui_theme_dark();
    if (!r_init(wm_window_handle(), UI_ATLAS_SIZE, UI_ATLAS_SIZE)) return 0;
    f_init(UI_ATLAS_SIZE, UI_ATLAS_SIZE);
    ui.scale = wm_scale();
    ui_fonts_load();
    return 1;
}

static U32 ui_shot_count;

// Testing: F12 (with ui_shot_dir set) saves the frame being drawn, and doesn't reach the app.
static void ui_take_shot_keys(void)
{
    U32 kept = 0;
    for (U32 i = 0; i < ui.event_count; i += 1) {
        WM_Event *e = &ui.events[i];
        if (ui_shot_dir.size && e->kind == WM_EVENT_KEY && e->key == WM_KEY_F12) {
            String16 p16 = os_str16_from_8(ui.frame_arena, str8_pushf(ui.frame_arena, "%.*s\\shot-%u.bmp", S8F(ui_shot_dir), ui_shot_count++));
            if (p16.size < ArrayCount(r_shot_path)) memcpy(r_shot_path, p16.str, (p16.size + 1) * sizeof(p16.str[0]));
            continue;
        }
        ui.events[kept++] = *e;
    }
    ui.event_count = kept;
}

static B32 ui_frame(WM_Frame *f, B32 (*build)(void))
{
    if (!r_state) return 0; // (ui_init hasn't run)
    U64 t0 = os_now_us();
    static U64 last_us;
    ui.dt = last_us ? Min((F32)(t0 - last_us) / 1e6f, 1.0f / 30) : 1.0f / 60;
    last_us = t0;
    arena_clear(ui.frame_arena);
    if (f->scale != ui.scale) { ui.scale = f->scale; ui_fonts_load(); }
    r_resize((U32)f->width, (U32)f->height);
    ui.width = (F32)r_state->width;
    ui.height = (F32)r_state->height;
    ui.in = f->in;
    ui.events = f->events;
    ui.event_count = f->event_count;
    ui_take_shot_keys();
    ui_ids_begin_frame(); // (the focus takes its keys here)
    ui.clip = rect(0, 0, ui.width, ui.height);
    ui.overlay_on = ui.in_overlay = 0;
    ui.block_count = 0;
    if (f_begin_frame()) wm_request_frame(); // the glyph atlas was full: emptied, so draw again
    ui.cursor = UI_CURSOR_ARROW;
    r_begin_frame();
    ui_aura(rect(0, 0, ui.width, ui.height), 0, 1); // the theme's glow over the top of the window
    ui.tip = str8(0, 0);
    B32 again = build();
    ui_ids_end_frame();
    ui_tooltip_draw();
    wm_caption_overlay(ui.overlay, ui.overlay_on);
    wm_set_cursor(ui.cursor);
    ui.frame_ms = (F64)(os_now_us() - t0) / 1000.0;
    {
        // Frames in the last second, from a ring of recent frame times.
        static U64 times[512];
        static U32 head;
        U64 now = os_now_us();
        times[head++ % ArrayCount(times)] = now;
        U32 n = 0;
        for (U32 k = 0; k < ArrayCount(times); k += 1) n += times[k] && now - times[k] < 1000000;
        ui.fps = n;
    }
    r_end_frame(ui.theme.bg);
    if (ui_frame_log.size) {
        String8 line = str8_pushf(ui.frame_arena, "%llu %d %d resize %llu cpu %d present %llu by %c sizing %d\r\n", os_now_us(), (int)ui.width, (int)ui.height,
                                  r_state->us_resize, (int)(ui.frame_ms * 1000), r_state->us_present, f->cause, f->sizing);
        r_state->us_resize = 0;
        os_append_file(ui_frame_log, line);
    }
    ui.events = 0;
    ui.event_count = 0;
    return again;
}

static F32 ui_window_buttons(F32 right, F32 h)
{
    F32 bw = 46 * ui.scale, x = right - 3 * bw;
    // Segoe Fluent Icons / MDL2: ChromeMinimize, ChromeMaximize or ChromeRestore, ChromeClose.
    char *glyph[3] = { "\xEE\xA4\xA1", wm_window_is_maximized() ? "\xEE\xA4\xA3" : "\xEE\xA4\xA2", "\xEE\xA2\xBB" };
    for (int i = 0; i < 3; i += 1) {
        Rect b = rect(x + i * bw, 0, x + (i + 1) * bw, h);
        B32 hot = ui_mouse_in(b), down = hot && ui.in.down && point_in(b, ui.in.press_x, ui.in.press_y);
        if (i == 1) {
            B32 max_hot, max_down; // maximize gets the mouse through the window layer (snap layouts)
            wm_caption_maximize(b, &max_hot, &max_down);
            hot |= max_hot; down |= max_down;
        } else {
            wm_caption_hole(b);
        }
        V4 fg = wm_window_is_active() || hot ? ui.theme.text : ui.theme.faint;
        if (hot && i == 2) { r_rect(b, ui_hex(0xc42b1c, down ? 0.85f : 1), 0, 0, 0); fg = ui_hex(0xffffff, 1); }
        else if (hot) r_rect(b, ui_hex(0xffffff, down ? 0.05f : 0.09f), 0, 0, 0);
        ui_text_centered(ui.font.glyphs, b, str8_cstring(glyph[i]), fg);
        if (ui_clicked(b)) {
            if (i == 0) wm_window_minimize();
            else if (i == 1) wm_window_toggle_maximized(); // (only when the caption is off: else maximize isn't client area)
            else wm_window_close();
        }
    }
    return x;
}

//- Lists

static void ui_scroll_reveal(UI_Scroll *sc, F32 top, F32 bottom, F32 view_h)
{
    if (top < sc->target) sc->target = top;
    else if (bottom > sc->target + view_h) sc->target = bottom - view_h;
}

static UI_List ui_list_begin(UI_Scroll *sc, Rect r, S64 count, F32 row_h, F32 extra_h)
{
    UI_List l = { 0 };
    l.sc = sc;
    l.r = r;
    l.row_h = row_h;
    l.content_h = (F32)count * row_h + extra_h;
    l.animating = ui_scroll_list(sc, r, l.content_h);
    ui.clip = r;
    r_set_clip(r);
    l.first = (S64)(sc->pos / row_h);
    l.last = Min(count, l.first + (S64)((r.y1 - r.y0) / row_h) + 2);
    return l;
}

static Rect ui_list_row(UI_List *l, S64 i, String8 key)
{
    if (l->scoped) ui_scope_pop();
    if (key.size) ui_scope_push(key);
    else ui_scope_push_u64((U64)i);
    l->scoped = 1;
    F32 y = (F32)(int)(l->r.y0 + (F32)i * l->row_h - l->sc->pos);
    return rect(l->r.x0, y, l->r.x1, y + l->row_h);
}

static void ui_list_end(UI_List *l)
{
    if (l->scoped) { ui_scope_pop(); l->scoped = 0; }
    ui_scrollbar(l->sc, l->r, l->content_h);
}

//- More widgets

static B32 ui_small_button(Rect r, String8 label, V4 bg, V4 bg_hot)
{
    UI_Id id = ui_id(label);
    UI_Signal sg = ui_signal(id, r, UI_FOCUSABLE);
    ui_plate(r, ui_mix(bg, bg_hot, ui_anim(id, (F32)sg.hovering)), 5 * ui.scale);
    ui_text_centered(ui.font.caption, r, ui_label_text(label), ui.theme.text);
    ui_focus_ring(id, r, 5 * ui.scale);
    return sg.clicked;
}

static F32 ui_small_button_width(String8 label) { return f_text_width(ui.font.caption, ui_label_text(label)) + 20 * ui.scale; }

static B32 ui_glass_button(Rect r, String8 label, F_Font *f)
{
    UI_Id id = ui_id(label);
    UI_Signal sg = ui_signal(id, r, UI_FOCUSABLE);
    F32 t = ui_anim(id, (F32)sg.hovering), radius = (r.y1 - r.y0) * 0.5f;
    r_rect(r, ui_hex(0xffffff, 0.08f + 0.10f * t), radius, 0, 0.5f);
    ui_text_centered(f, r, ui_label_text(label), ui_hex(0xffffff, 0.8f + 0.2f * t));
    ui_focus_ring(id, r, radius);
    return sg.clicked;
}

// The line under a tab's label (inset from r's sides): the active tab's, or half lit on hover.
static void ui_tab_line(Rect r, F32 inset, B32 active, F32 hover)
{
    F32 s = ui.scale, h = Max(ui_px(), 2 * s);
    V4 c = active ? ui.theme.accent_hi : ui.theme.border_hi;
    if (!active) c.w *= hover;
    if (c.w > 0) r_rect(rect(r.x0 + inset, r.y1 - h, r.x1 - inset, r.y1), c, h * 0.5f, 0, 0.5f);
}

static F32 ui_tab_badge_width(String8 badge) { return badge.size ? f_text_width(ui.font.caption, badge) + 14 * ui.scale : 0; }

static F32 ui_tab_width(String8 label, String8 badge)
{
    return f_text_width(ui.font.normal, ui_label_text(label)) + 28 * ui.scale + (badge.size ? ui_tab_badge_width(badge) + 8 * ui.scale : 0);
}

static B32 ui_tab(Rect r, String8 label, String8 badge, B32 active)
{
    F32 s = ui.scale;
    UI_Id id = ui_id(label);
    UI_Signal sg = ui_signal(id, r, UI_FOCUSABLE);
    F32 t = ui_anim(id, (F32)sg.hovering);
    label = ui_label_text(label);
    ui_tab_line(r, 14 * s, active, t);
    V4 fg = active ? ui.theme.text : ui_mix(ui.theme.dim, ui.theme.text, t);
    if (badge.size) {
        F32 lx = r.x0 + 14 * s;
        f_draw_text(ui.font.normal, lx, (r.y0 + r.y1 - ui.font.normal->line_height) * 0.5f, label, fg, 0);
        F32 bx = lx + f_text_width(ui.font.normal, label) + 8 * s, bh = ui.font.caption->line_height + 4 * s;
        ui_pill(rect(bx, (r.y0 + r.y1 - bh) * 0.5f, bx + ui_tab_badge_width(badge), (r.y0 + r.y1 + bh) * 0.5f), badge, ui.font.caption, ui_hex(0x0e1014, 1), ui.theme.ok);
    } else {
        ui_text_centered(ui.font.normal, r, label, fg);
    }
    ui_focus_ring(id, r, 6 * s);
    return sg.clicked;
}

static F32 ui_title_tab_width(String8 label) { return f_text_width(ui.font.normal, ui_label_text(label)) + 26 * ui.scale; }

static B32 ui_title_tab(Rect r, String8 label, B32 active)
{
    UI_Id id = ui_id(label);
    UI_Signal sg = ui_signal(id, r, UI_FOCUSABLE);
    F32 t = ui_anim(id, (F32)sg.hovering);
    ui_tab_line(r, 13 * ui.scale, active, t);
    ui_text_centered(ui.font.normal, r, ui_label_text(label), active ? ui.theme.text : ui_mix(ui.theme.dim, ui.theme.text, t));
    ui_focus_ring(id, r, 6 * ui.scale);
    return !active && sg.clicked;
}

static F32 ui_link_text(F_Font *f, F32 x, F32 y, String8 text, V4 color, F32 max_w, Rect *hit)
{
    F32 s = ui.scale, lh = f->line_height;
    F32 end = f_draw_text(f, x, y, text, color, max_w);
    Rect r = rect(x - 4 * s, y - 3 * s, end + 4 * s, y + lh + 3 * s);
    if (ui_mouse_in(r) && ui_mouse_in(ui.clip)) {
        r_rect(rect(x, y + lh - 1 * s, end, y + lh - 1 * s + ui_px()), color, 0, 0, 0);
        ui.cursor = UI_CURSOR_HAND;
    }
    if (hit) *hit = r;
    return end;
}

static B32 ui_busy_label(F32 right, F32 cy, String8 text)
{
    F32 s = ui.scale, tw = f_text_width(ui.font.normal, text);
    f_draw_text(ui.font.normal, right - tw, cy - ui.font.normal->line_height * 0.5f, text, ui.theme.dim, 0);
    return ui_spinner(right - tw - 12 * s, cy, 7 * s);
}

//- Popups

static Rect ui_modal_begin(F32 w, F32 h, B32 *fresh)
{
    F32 s = ui.scale, W = ui.width, H = ui.height;
    if (*fresh) { *fresh = 0; ui.in.released = 0; }
    ui_overlay_begin();
    r_rect(rect(0, 0, W, H), ui_hex(0x05070a, 0.72f), 0, 0, 0);
    w = Min(w, W - 48 * s);
    h = Min(h, H - 48 * s);
    Rect panel = rect((F32)(int)((W - w) * 0.5f), (F32)(int)((H - h) * 0.5f), 0, 0);
    panel.x1 = panel.x0 + w;
    panel.y1 = panel.y0 + h;
    r_rect(rect(panel.x0 - 2 * s, panel.y0 + 6 * s, panel.x1 + 2 * s, panel.y1 + 14 * s), ui_hex(0x000000, 0.5f), 12 * s, 0, 14 * s); // shadow
    ui_panel(panel, 8 * s);
    ui.clip = panel;
    r_set_clip(panel);
    return panel;
}

static B32 ui_modal_close_button(Rect panel)
{
    F32 s = ui.scale;
    Rect cb = rect(panel.x1 - 12 * s - 32 * s, panel.y0 + 12 * s, panel.x1 - 12 * s, panel.y0 + 44 * s);
    UI_Id id = ui_id(str8_lit("modal close"));
    UI_Signal sg = ui_signal(id, cb, UI_FOCUSABLE);
    F32 t = ui_anim(id, (F32)sg.hovering);
    if (t > 0) { V4 c = ui.theme.hover; c.w *= t; r_rect(cb, c, 6 * s, 0, 0.5f); }
    ui_text_centered(ui.font.glyphs, cb, str8_lit("\xEE\xA2\xBB"), ui_mix(ui.theme.dim, ui.theme.text, t)); // ChromeClose
    ui_focus_ring(id, cb, 6 * s);
    return sg.clicked;
}

static B32 ui_modal_end(Rect panel)
{
    F32 W = ui.width, H = ui.height;
    r_set_clip(rect(0, 0, W, H));
    ui.clip = rect(0, 0, W, H);
    ui_panel_rim(panel, 8 * ui.scale);
    B32 outside = ui.in.released && !point_in(panel, ui.in.press_x, ui.in.press_y) && !point_in(panel, ui.in.mx, ui.in.my);
    ui_overlay_end();
    return outside;
}
