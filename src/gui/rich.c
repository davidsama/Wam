// gui/rich.c - draws rich text (core/markup.c): descriptions and changelogs.
// Part of main_gui.c's unity build.
//
// A document is laid out once per width, size and DPI scale, and kept in a caption cache; drawing it goes over what's
// laid out and draws what's visible. Colors are looked up when drawing, so a theme change needs no new layout.

typedef enum RichColor { RICH_C_DIM, RICH_C_TEXT, RICH_C_HEADING, RICH_C_LINK, RICH_C_FAINT } RichColor;

typedef struct RichRun { // a piece of one line, in one style
    F32 x, y, w;
    F_Font *font;
    String8 text;
    RichColor color;
    RichSpan *span;      // 0: a list marker
    F32 pic_h;           // a picture drawn in the text: its height (else 0)
} RichRun;

typedef struct RichBox { Rect r; U32 kind; } RichBox;
enum { RICH_BOX_CODE, RICH_BOX_QUOTE, RICH_BOX_LINE };

typedef struct RichLayout {
    Arena *arena;
    U64 doc;             // Rich.id, 0 = free
    F32 w, scale;
    B32 caption;
    U64 used;            // frame last drawn
    RichRun *runs; U64 run_count, run_cap;
    RichBox *boxes; U64 box_count, box_cap;
    F32 h;
    B32 pics;            // it has pictures,
    U32 pic_changes;     // laid out with what was known of them then (ui_picture_changes)
} RichLayout;

#define RICH_CHIP_PAD (6 * ui.scale)
#define RICH_CHIP_GAP (4 * ui.scale)
#define RICH_LAYOUTS 32
static RichLayout rich_layouts[RICH_LAYOUTS];

static RichRun *rich_push_run(RichLayout *l)
{
    if (l->run_count == l->run_cap) {
        U64 cap = Max(l->run_cap * 2, 128);
        RichRun *v = push_array_nz(l->arena, RichRun, cap);
        if (l->run_count) memcpy(v, l->runs, l->run_count * sizeof(RichRun));
        l->runs = v; l->run_cap = cap;
    }
    RichRun *r = &l->runs[l->run_count++];
    memset(r, 0, sizeof(*r));
    return r;
}

static void rich_push_box(RichLayout *l, Rect r, U32 kind)
{
    if (l->box_count == l->box_cap) {
        U64 cap = Max(l->box_cap * 2, 32);
        RichBox *v = push_array_nz(l->arena, RichBox, cap);
        if (l->box_count) memcpy(v, l->boxes, l->box_count * sizeof(RichBox));
        l->boxes = v; l->box_cap = cap;
    }
    l->boxes[l->box_count++] = (RichBox){ r, kind };
}

static F_Font *rich_font(RichBlock *k, RichSpan *sp, B32 caption)
{
    if (k->kind == RICH_HEADING) return k->level <= 2 && !caption ? ui.font.title : k->level <= 4 || !caption ? ui.font.bold : ui.font.caption_bold;
    if (k->kind == RICH_PRE || (sp->flags & RICH_CODE)) return caption ? ui.font.caption_mono : ui.font.mono;
    if (sp->flags & RICH_IMAGE) return ui.font.caption;
    if (sp->flags & RICH_BOLD) return caption ? ui.font.caption_bold : ui.font.bold;
    if (sp->flags & RICH_ITALIC) return caption ? ui.font.caption_italic : ui.font.italic;
    return caption ? ui.font.caption : ui.font.normal;
}

typedef struct RichLine { RichLayout *l; RichBlock *k; F_Font *base; F32 x0, w, x, y, h; B32 caption; } RichLine;

static B32 rich_picture_inline(String8 url) { return rich_addon_site(url) && !ui_picture_failed(url); }

// How big it's drawn: its size at most, inside max_w (a guess until it's loaded).
static void rich_picture_size(String8 url, F32 max_w, F32 *w, F32 *h)
{
    F32 s = ui.scale, pw = 240 * s, ph = 135 * s;
    U32 iw, ih;
    if (ui_picture_size(url, &iw, &ih)) { pw = (F32)iw * s; ph = (F32)ih * s; }
    F32 k = Min(1.0f, Min(max_w / pw, 480 * s / ph));
    *w = Max(pw * k, 1); *h = Max(ph * k, 1);
}

// Part of a word at the pen (continuing the run before it, when it's the same text going on).
static void rich_place(RichLine *c, RichSpan *sp, String8 text, F32 tw)
{
    F_Font *f = rich_font(c->k, sp, c->caption);
    RichRun *prev = c->l->run_count ? &c->l->runs[c->l->run_count - 1] : 0;
    F32 y = c->y + c->base->ascent - f->ascent;
    U8 *end = prev ? prev->text.str + prev->text.size : 0;
    if (prev && !prev->pic_h && prev->span == sp && prev->y == y && (end == text.str || (end + 1 == text.str && *end == ' '))) {
        prev->text.size = (U64)(text.str + text.size - prev->text.str);
        prev->w = c->x + tw - prev->x;
    } else {
        RichRun *r = rich_push_run(c->l);
        r->x = c->x; r->y = y; r->w = tw;
        r->font = f;
        r->text = text;
        r->span = sp;
        r->color = c->k->kind == RICH_HEADING ? RICH_C_HEADING : (sp->flags & RICH_IMAGE) ? RICH_C_DIM : (sp->flags & (RICH_BOLD | RICH_CODE)) ? RICH_C_TEXT : RICH_C_DIM;
        if (sp->link.size || (sp->flags & RICH_IMAGE)) r->color = RICH_C_LINK;
    }
    c->x += tw;
}

static void rich_next_line(RichLine *c) { c->x = c->x0; c->y += c->h; c->h = c->base->line_height; }

// A word too long for a line, character by character.
static void rich_place_split(RichLine *c, RichSpan *sp, String8 text)
{
    F_Font *f = rich_font(c->k, sp, c->caption);
    U64 from = 0;
    F32 w = 0;
    for (U64 i = 0; i < text.size;) {
        UnicodeDecode d = utf8_decode(text.str + i, text.size - i);
        F32 adv = f_advance(f, d.codepoint);
        if (c->x + w + adv > c->x0 + c->w && (c->x > c->x0 || i > from)) {
            if (i > from) rich_place(c, sp, str8_substr(text, from, i), w);
            rich_next_line(c);
            from = i; w = 0;
        }
        w += adv;
        i += Max(d.advance, 1);
    }
    if (from < text.size) rich_place(c, sp, str8_skip(text, from), w);
}

// Lays out a block's spans at (x, y), w wide; returns their height.
static F32 rich_layout_spans(RichLayout *l, RichBlock *k, F32 x, F32 y, F32 w, B32 caption)
{
    RichLine c = { l, k, 0, x, Max(w, 8 * ui.scale), x, y, 0, caption };
    RichSpan plain = { 0 };
    c.base = rich_font(k, &plain, caption);
    c.h = c.base->line_height;
    typedef struct Piece { RichSpan *sp; String8 text; F32 w; } Piece;
    Piece word[32];
    U32 n = 0;
    F32 word_w = 0, space = 0;
    B32 any = 0;
    for (RichSpan *sp = k->first; sp; sp = sp->next) {
        F_Font *f = rich_font(k, sp, caption);
        for (U64 i = 0; i <= sp->text.size;) {
            U64 j = i;
            B32 chip = (sp->flags & RICH_IMAGE) != 0; // a picture's name: a chip of its own, in one piece
            if (chip) j = sp->text.size;
            while (j < sp->text.size && sp->text.str[j] != ' ' && sp->text.str[j] != '\n') j += 1;
            if (chip) l->pics = 1;
            if (chip && rich_picture_inline(sp->image)) {
                F32 pw, ph;
                rich_picture_size(sp->image, c.w, &pw, &ph);
                if (c.x > c.x0 && c.x + space + pw > c.x0 + c.w) rich_next_line(&c);
                else if (c.x > c.x0) c.x += space;
                RichRun *r = rich_push_run(l);
                r->x = c.x; r->y = c.y; r->w = pw; r->pic_h = ph;
                r->font = f; r->span = sp;
                c.x += pw + RICH_CHIP_GAP;
                c.h = Max(c.h, ph + 4 * ui.scale);
                space = 0;
                any = 1;
                break;
            }
            if (j > i) {
                any = 1;
                String8 text = str8_substr(sp->text, i, j);
                word[n].sp = sp; word[n].text = text; word[n].w = chip ? Min(f_text_width(f, text) + RICH_CHIP_PAD * 2 + RICH_CHIP_GAP, c.w) : f_text_width(f, text);
                word_w += word[n].w;
                n += 1;
            }
            B32 at_end = j >= sp->text.size;
            if (n && (!at_end || !sp->next || chip || (sp->next->flags & RICH_IMAGE) || n == ArrayCount(word))) { // the word is over
                if (c.x > c.x0 && c.x + space + word_w > c.x0 + c.w) rich_next_line(&c);
                else if (c.x > c.x0 || k->kind == RICH_PRE) c.x += space;
                for (U32 p = 0; p < n; p += 1) {
                    if (c.x + word[p].w > c.x0 + c.w + 0.5f) rich_place_split(&c, word[p].sp, word[p].text);
                    else rich_place(&c, word[p].sp, word[p].text, word[p].w);
                }
                n = 0; word_w = 0; space = 0;
            }
            if (at_end) break;
            if (sp->text.str[j] == '\n') { rich_next_line(&c); space = 0; }
            else space += f_advance(f, ' ');
            i = j + 1;
        }
    }
    return any ? c.y + c.h - y : 0;
}

static void rich_layout(RichLayout *l, Rich *doc, F32 w, B32 caption)
{
    F32 s = ui.scale, y = 0;
    F_Font *base = caption ? ui.font.caption : ui.font.normal;
    F32 lh = base->line_height, step = 18 * s;
    for (RichBlock *k = doc->first; k;) {
        if (k != doc->first) y += k->kind == RICH_HEADING ? lh * 0.9f : k->gap ? lh * 0.6f : lh * 0.15f;
        F32 x = 0, top = y;
        x += (F32)Min(k->quote, 4) * 14 * s;
        if (k->kind != RICH_HEADING) x += (F32)Min(k->indent, 6) * step;
        x = Min(x, w * 0.5f); // deep in a narrow panel
        RichBlock *next = k->next;
        if (k->kind == RICH_RULE) {
            rich_push_box(l, rect(x, y + lh * 0.4f, w, y + lh * 0.4f + ui_px()), RICH_BOX_LINE);
            y += lh * 0.8f;
        } else if (k->kind == RICH_PRE) {
            F32 pad = 10 * s;
            U64 box = l->box_count;
            rich_push_box(l, rect(x, y, w, y), RICH_BOX_CODE);
            y += pad + rich_layout_spans(l, k, x + pad, y + pad, w - x - 2 * pad, caption) + pad;
            l->boxes[box].r.y1 = y;
        } else if (k->kind == RICH_CELL) {
            F32 pad = 8 * s, cw = (w - x) / (F32)Max(k->cols, 1);
            RichBlock *c = k;
            while (c && c->kind == RICH_CELL && c->table == k->table) {
                U32 row = c->row;
                F32 h = 0;
                for (; c && c->kind == RICH_CELL && c->table == k->table && c->row == row; c = c->next) {
                    h = Max(h, rich_layout_spans(l, c, x + (F32)c->col * cw, y + pad * 0.5f, cw - pad, caption));
                }
                y += h > 0 ? h + pad : 0;
                if (h > 0) rich_push_box(l, rect(x, y, w, y + ui_px()), RICH_BOX_LINE);
            }
            next = c;
        } else {
            if (k->kind == RICH_ITEM) {
                RichRun *r = rich_push_run(l);
                r->font = base;
                r->text = k->number ? str8_pushf(l->arena, "%u.", k->number) : str8_lit("\xE2\x80\xA2");
                r->w = f_text_width(base, r->text);
                r->x = x - r->w - 6 * s;
                r->y = y;
                r->color = RICH_C_FAINT;
            }
            y += rich_layout_spans(l, k, x, y, w - x, caption);
        }
        for (U32 q = 0; q < Min(k->quote, 4); q += 1) {
            F32 qx = (F32)q * 14 * s;
            rich_push_box(l, rect(qx, top, qx + 3 * s, y), RICH_BOX_QUOTE);
        }
        k = next;
    }
    l->h = y;
}

static RichLayout *rich_layout_get(Rich *doc, F32 w, B32 caption)
{
    RichLayout *l = 0, *oldest = &rich_layouts[0];
    for (U32 i = 0; i < RICH_LAYOUTS; i += 1) {
        RichLayout *c = &rich_layouts[i];
        if (c->doc == doc->id) { l = c; break; }
        if (c->used < oldest->used) oldest = c;
    }
    if (!l) l = oldest;
    if (l->doc != doc->id || l->w != w || l->scale != ui.scale || l->caption != caption || (l->pics && l->pic_changes != ui_picture_changes())) {
        if (!l->arena) l->arena = arena_alloc(GB(1));
        Arena *a = l->arena;
        arena_clear(a);
        *l = (RichLayout){ 0 };
        l->arena = a;
        l->doc = doc->id; l->w = w; l->scale = ui.scale; l->caption = caption;
        l->pic_changes = ui_picture_changes();
        rich_layout(l, doc, w, caption);
    }
    l->used = ui.frame_index + 1;
    return l;
}

// "GIF" in a picture's corner, if there's room: it moves when it's shown large.
static void draw_gif_badge(Rect r)
{
    F32 s = ui.scale, w = f_text_width(ui.font.caption, str8_lit("GIF")) + 8 * s, h = ui.font.caption->line_height;
    if (r.x1 - r.x0 < w + 8 * s || r.y1 - r.y0 < h + 8 * s) return;
    Rect b = rect(r.x0 + 4 * s, r.y0 + 4 * s, r.x0 + 4 * s + w, r.y0 + 4 * s + h);
    r_rect(b, ui_hex(0x05070a, 0.78f), 4 * s, 0, 0.5f);
    ui_text_centered(ui.font.caption, b, str8_lit("GIF"), ui_hex(0xffffff, 0.9f));
}

// Draws the document at (x, y), w wide, only what's in [vis_y0, vis_y1); returns its height. caption: in the
// caption font. A click on a link opens it in the browser, one on a picture (or its name) sets *picture to its
// URL (when picture isn't 0, else that opens in the browser too).
static F32 rich_draw(Rich *doc, F32 x, F32 y, F32 w, B32 caption, F32 vis_y0, F32 vis_y1, String8 *picture)
{
    if (rich_empty(doc)) return 0;
    F32 s = ui.scale;
    RichLayout *l = rich_layout_get(doc, w, caption);
    x = (F32)(int)x;
    for (U64 i = 0; i < l->box_count; i += 1) {
        Rect r = l->boxes[i].r;
        r = rect(x + r.x0, y + r.y0, x + r.x1, y + r.y1);
        if (r.y1 < vis_y0 || r.y0 > vis_y1) continue;
        if (l->boxes[i].kind == RICH_BOX_CODE) r_rect(r, ui_hex(0x000000, 0.28f), 6 * s, 0, 0.5f);
        else if (l->boxes[i].kind == RICH_BOX_QUOTE) r_rect(r, C_BORDER_HI, 1.5f * s, 0, 0.5f);
        else r_rect(r, C_LINE, 0, 0, 0);
    }
    // The link under the mouse: all its runs (over several lines, or styles) light up.
    RichRun *hot = 0;
    for (U64 i = 0; i < l->run_count; i += 1) {
        RichRun *r = &l->runs[i];
        F32 ry = y + r->y, rh = r->pic_h ? r->pic_h : r->font->line_height;
        if (!r->span || ry + rh < vis_y0 || ry > vis_y1 || !(r->span->link.size || r->span->image.size)) continue;
        Rect hit = rect(x + r->x, ry, x + r->x + r->w, ry + rh);
        if (!ui_mouse_in(hit) || !ui_mouse_in(ui.clip)) continue;
        hot = r;
        ui.cursor = UI_CURSOR_HAND;
        String8 url = r->span->link.size ? r->span->link : r->span->image;
        if (!r->pic_h || r->span->link.size) ui_tooltip(hit, url.size > 90 ? str8_pushf(ui.frame_arena, "%.*s...", 87, url.str) : url); // where it goes, whatever its text says
        if (ui_clicked(hit)) {
            if (!r->span->link.size && picture) *picture = r->span->image;
            else os_open_url(url);
        }
    }
    for (U64 i = 0; i < l->run_count; i += 1) {
        RichRun *r = &l->runs[i];
        F32 rx = x + r->x, ry = y + r->y, rh = r->pic_h ? r->pic_h : r->font->line_height;
        if (ry + rh < vis_y0 || ry > vis_y1) continue;
        RichSpan *sp = r->span;
        if (r->pic_h) {
            Rect pr = rect(rx, ry, rx + r->w, ry + rh);
            if (!ui_picture(pr, sp->image, 16, 4 * s, 0, 0)) r_rect(pr, C_BUTTON, 4 * s, 0, 0.5f);
            if (ui_image_url_gif(sp->image) && !sp->link.size) draw_gif_badge(pr); // a click plays it, in the viewer
            if (hot == r && sp->link.size) r_rect(pr, C_BORDER_HI, 4 * s, ui_px(), 0.5f);
            continue;
        }
        B32 chip = sp && (sp->flags & RICH_IMAGE);
        F32 rw = chip ? r->w - RICH_CHIP_GAP : r->w, inset = chip ? RICH_CHIP_PAD : 0;
        if (chip) r_rect(rect(rx, ry - s, rx + rw, ry + rh + s), C_BUTTON, 5 * s, 0, 0.5f);
        else if (sp && (sp->flags & RICH_CODE)) r_rect(rect(rx - 3 * s, ry, rx + rw + 3 * s, ry + rh), ui_hex(0x000000, 0.28f), 4 * s, 0, 0.5f);
        V4 color = r->color == RICH_C_TEXT ? C_TEXT : r->color == RICH_C_HEADING ? ui.theme.heading : r->color == RICH_C_LINK ? C_ACCENT_HI : r->color == RICH_C_FAINT ? C_FAINT : C_DIM;
        f_draw_text(r->font, rx + inset, ry, r->text, color, chip ? rw - 2 * inset + 1 : 0);
        if (sp && (sp->flags & RICH_STRIKE)) r_rect(rect(rx, ry + rh * 0.55f, rx + r->w, ry + rh * 0.55f + ui_px()), color, 0, 0, 0);
        B32 is_hot = hot && sp && (sp == hot->span || (sp->link.size && sp->link.str == hot->span->link.str));
        if (is_hot) r_rect(rect(rx + inset, ry + rh - 1 * s, rx + rw - inset, ry + rh - 1 * s + ui_px()), color, 0, 0, 0);
    }
    return l->h;
}
