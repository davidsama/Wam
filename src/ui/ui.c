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
    return t;
}

static int CALLBACK ui_font_found(const LOGFONTW *lf, const TEXTMETRICW *tm, DWORD type, LPARAM found)
{
    (void)lf; (void)tm; (void)type;
    *(B32 *)found = 1;
    return 0;
}

static B32 ui_font_installed(wchar_t *family)
{
    LOGFONTW lf = { 0 };
    lf.lfCharSet = DEFAULT_CHARSET;
    wcsncpy_s(lf.lfFaceName, ArrayCount(lf.lfFaceName), family, _TRUNCATE);
    B32 found = 0;
    HDC dc = GetDC(0);
    EnumFontFamiliesExW(dc, &lf, ui_font_found, (LPARAM)&found, 0);
    ReleaseDC(0, dc);
    return found;
}

static void ui_fonts_load(void)
{
    if (ui.font_arena) {
        f_font_close(ui.font.normal); f_font_close(ui.font.bold); f_font_close(ui.font.caption);
        f_font_close(ui.font.title); f_font_close(ui.font.icon); f_font_close(ui.font.glyphs); f_font_close(ui.font.glyphs_big);
        arena_clear(ui.font_arena);
    } else {
        ui.font_arena = arena_alloc(MB(64));
    }
    f_atlas_reset();
    F32 s = ui.scale;
    ui.font.normal = f_font_open(ui.font_arena, L"Segoe UI", 14 * s, FW_NORMAL);
    ui.font.bold   = f_font_open(ui.font_arena, L"Segoe UI", 14 * s, FW_SEMIBOLD);
    ui.font.caption  = f_font_open(ui.font_arena, L"Segoe UI", 12 * s, FW_NORMAL);
    ui.font.title  = f_font_open(ui.font_arena, L"Segoe UI", 17 * s, FW_SEMIBOLD);
    ui.font.icon   = f_font_open(ui.font_arena, L"Segoe UI Symbol", 12 * s, FW_BOLD);
    // Windows 11's caption glyphs; Windows 10 has the older MDL2 ones (same code points).
    wchar_t *glyph_family = ui_font_installed(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    ui.font.glyphs = f_font_open(ui.font_arena, glyph_family, 10 * s, FW_NORMAL);
    ui.font.glyphs_big = f_font_open(ui.font_arena, glyph_family, 14 * s, FW_NORMAL);
}

//- Ornaments

static void ui_plate(Rect r, V4 bg, F32 radius)
{
    F32 o = ui.theme.ornate, px = ui_px();
    if (o <= 0) { r_rect(r, bg, radius, 0, 0.5f); return; }
    V4 white = ui_hex(0xffffff, bg.w), black = ui_hex(0x000000, bg.w);
    r_rect_gradient(r, ui_mix(bg, white, 0.10f * o), ui_mix(bg, black, 0.22f * o), radius, 0, 0.5f, 0);
    // A rim tinted towards the trim's metal, and light catching the top edge.
    r_rect(r, ui_mix(ui_mix(bg, black, 0.45f), ui.theme.trim, 0.30f * o), radius, px, 0.5f);
    Rect in = rect(r.x0 + px, r.y0 + px, r.x1 - px, r.y1 - px);
    r_rect_gradient(in, ui_hex(0xffffff, 0.16f * o), ui_hex(0xffffff, 0), Max(radius - px, 0), px, 0.5f, 0);
}

static void ui_panel(Rect r, F32 radius)
{
    UI_Theme *t = &ui.theme;
    F32 o = t->ornate, s = ui.scale;
    if (o <= 0) { r_rect(r, t->panel, radius, 0, 0.5f); return; }
    r_rect(rect(r.x0, r.y0 + 3 * s, r.x1, r.y1 + 3 * s), ui_hex(0x000000, 0.45f * o), radius + 2 * s, 0, 6 * s); // shadow
    r_rect_gradient(r, ui_mix(t->panel, t->hover, 0.5f * o), t->panel, radius, 0, 0.5f, 0);
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
    // A stud on each corner.
    F32 k = Max(radius * 0.3f, 2 * s), g = 10 * s;
    ui_gem(r.x0 + k, r.y0 + k, g, t->trim_hi);
    ui_gem(r.x1 - k, r.y0 + k, g, t->trim_hi);
    ui_gem(r.x0 + k, r.y1 - k, g, t->trim_hi);
    ui_gem(r.x1 - k, r.y1 - k, g, t->trim_hi);
}

static void ui_divider(F32 x0, F32 x1, F32 y)
{
    UI_Theme *t = &ui.theme;
    F32 o = t->ornate, s = ui.scale, px = ui_px();
    if (o <= 0 || x1 - x0 < 60 * s) { r_rect(rect(x0, y - px * 0.5f, x1, y + px * 0.5f), t->border, 0, 0, 0); return; }
    F32 cx = (x0 + x1) * 0.5f, gap = 11 * s, fade = Min(160 * s, (cx - gap - x0) * 0.6f);
    V4 metal = ui_mix(t->border, t->trim, 0.85f * o), none = metal;
    none.w = 0;
    F32 h = Max(px, 1 * s), y0 = y - h * 0.5f;
    r_rect_gradient(rect(x0, y0, x0 + fade, y0 + h), none, metal, 0, 0, 0, 1);
    r_rect(rect(x0 + fade, y0, cx - gap, y0 + h), metal, 0, 0, 0);
    r_rect(rect(cx + gap, y0, x1 - fade, y0 + h), metal, 0, 0, 0);
    r_rect_gradient(rect(x1 - fade, y0, x1, y0 + h), metal, none, 0, 0, 0, 1);
    r_diamond(rect(cx - gap + 2 * s, y - 2 * s, cx - gap + 6 * s, y + 2 * s), metal, metal, 0);
    r_diamond(rect(cx + gap - 6 * s, y - 2 * s, cx + gap - 2 * s, y + 2 * s), metal, metal, 0);
    ui_gem(cx, y, 11 * s, t->gem);
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
    F32 a = (F32)fmod((F64)GetTickCount64() / 2400.0, 1.0) * 6.2831853f;
    ui_cog(cx, cy, r, a, ui.theme.ornate > 0 ? ui.theme.trim_hi : ui.theme.accent);
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
    B32 hot = ui_mouse_in(r) && ui_mouse_in(ui.clip);
    ui_plate(r, hot ? bg_hot : bg, 6 * ui.scale);
    ui_text_centered(ui.font.normal, r, label, fg);
    return hot && ui.in.released && point_in(r, ui.in.press_x, ui.in.press_y);
}

static F32 ui_button_width(String8 label) { return f_text_width(ui.font.normal, label) + 24 * ui.scale; }

static B32 ui_checkbox(F32 x, F32 cy, B32 checked)
{
    F32 s = ui.scale, sz = 16 * s;
    Rect b = rect(x, (F32)(int)(cy - sz * 0.5f), x + sz, (F32)(int)(cy - sz * 0.5f) + sz);
    Rect hit = rect(b.x0 - 6 * s, b.y0 - 6 * s, b.x1 + 6 * s, b.y1 + 6 * s); // easier to hit than it looks
    B32 hot = ui_mouse_in(hit) && ui_mouse_in(ui.clip);
    if (checked) {
        ui_plate(b, hot ? ui.theme.accent_hi : ui.theme.accent, 4 * s);
        ui_text_centered(ui.font.icon, b, str8_lit("\xE2\x9C\x93"), ui.theme.text); // U+2713 check mark
    } else {
        r_rect(b, hot ? ui.theme.faint : ui.theme.border_hi, 4 * s, 1.5f * s, 0.5f);
    }
    return hot && ui.in.released && point_in(hit, ui.in.press_x, ui.in.press_y);
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
    F32 t = (F32)fmod((F64)GetTickCount64() / 1100.0, 1.0);
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

static B32 ui_drag_handle(Rect zone, B32 *dragging)
{
    if (ui.in.pressed && ui_mouse_in(zone) && point_in(zone, ui.in.press_x, ui.in.press_y)) *dragging = 1;
    if (!ui.in.down) *dragging = 0;
    if (*dragging || ui_mouse_in(zone)) ui.cursor = UI_CURSOR_SIZE_WE;
    return *dragging;
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
            sc->dragging = 1;
            sc->grab = ui.in.my - thumb.y0;
            ui.in.press_x = ui.in.press_y = -1e9f; // taken: no other widget gets this click
        }
        if (sc->dragging && !ui.in.down) sc->dragging = 0;
        if (sc->dragging && room > 0) {
            F32 t = Clamp(0, (ui.in.my - sc->grab - list.y0) / room, 1);
            sc->pos = sc->target = t * max_scroll;
        }
        if (sc->dragging || ui_mouse_in(track)) ui_block_mouse(sc->dragging ? rect(-1e9f, -1e9f, 1e9f, 1e9f) : track);
    } else {
        sc->dragging = 0;
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
    B32 hot = sc->dragging || (ui.in.inside && point_in(track, ui.in.mx, ui.in.my));
    F32 w = hot ? 8 * s : 4 * s, x1 = list.x1 - 4 * s;
    if (hot) r_rect(rect(x1 - w, list.y0 + 2 * s, x1, list.y1 - 2 * s), ui_hex(0xffffff, 0.04f), w * 0.5f, 0, 0.5f);
    r_rect(rect(x1 - w, thumb.y0 + 2 * s, x1, thumb.y1 - 2 * s), sc->dragging ? ui.theme.dim : hot ? ui.theme.dim : ui.theme.faint, w * 0.5f, 0, 0.5f);
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
    SecureZeroMemory(t->s, sizeof(t->s));
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

static B32 ui_text_edit(UI_TextBuf *t, UI_Event *e)
{
    if (e->kind == UI_EVENT_TEXT) { ui_text_insert(t, e->codepoint); return 1; }
    B32 ctrl = (e->mods & UI_MOD_CTRL) != 0;
    if (e->key == UI_KEY_BACKSPACE) {
        if (ctrl) t->len = 0;
        else ui_text_backspace(t);
        return 1;
    }
    if (ctrl && e->key == 'V') { ui_text_paste(t, ui_clipboard_text(ui.frame_arena)); return 1; }
    return 0;
}

static void ui_text_field(Rect r, UI_TextBuf *t, String8 hint, B32 masked, V4 border)
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
    F32 caret = r.x0 + 12 * s;
    if (q.size) caret = f_draw_text(f, r.x0 + 12 * s, ty, q, ui.theme.text, r.x1 - r.x0 - 24 * s) + 1 * s;
    else f_draw_text(f, r.x0 + 16 * s, ty, hint, ui.theme.faint, r.x1 - r.x0 - 32 * s);
    r_rect(rect(caret, ty + 2 * s, caret + ui_px(), ty + f->line_height - 2 * s), ui.theme.accent, 0, 0, 0);
}
