// font_dwrite.c - text with DirectWrite: the same interface as font_gdi.c (f_font_open,
// f_text_width, f_draw_text, a shared R8 glyph atlas), on top of font_dwrite.cpp.
//
// Compared with GDI: natural, symmetric antialiasing instead of hinting snapped to whole pixels
// (even stroke weights), fractional advances with glyphs cached at F_SUBPIXEL positions per pixel
// (even spacing), and Windows' font fallback for characters the font lacks (emoji, CJK, symbols).
//
// Glyphs are rasterized on first use into the atlas with a shelf packer. When it fills up, it's
// emptied at the start of the next frame and everything visible re-rasterizes (f_begin_frame).

#include "font_dwrite.h"

#define F_GLYPH_SLOTS 4096       // per font: characters x sub-pixel positions in use
#define F_SUBPIXEL    4          // x positions per pixel a glyph is rasterized at
#define F_MAX_FONTS   16
// Coverage is lifted a little (gamma < 1): DirectWrite's linear coverage looks thin as light text
// on a dark background next to GDI's.
#define F_CONTRAST    0.80f

typedef struct F_Glyph {
    U32 key;         // codepoint << 2 | sub-pixel bucket
    B32 used;
    B32 has_bitmap;
    Rect src;        // atlas pixels
    F32 off_x;       // bitmap left relative to the pen (whole pixels, the bucket's offset baked in)
    F32 off_y;       // bitmap top relative to the baseline (negative = above)
    F32 w, h;
    F32 advance;     // fractional
} F_Glyph;

// Advances for measuring, apart from the rasterized glyphs (measuring a long text to wrap it
// shouldn't rasterize it at a sub-pixel position it may never be drawn at).
#define F_ADVANCE_SLOTS 4096
typedef struct F_Advance { U32 cp; F32 advance; } F_Advance; // advance < 0: slot empty

typedef struct F_Font {
    FdwFontHandle dw;
    F32 ascent, descent, line_height;
    F_Glyph glyphs[F_GLYPH_SLOTS];
    U32 glyph_count;
    F_Advance advances[F_ADVANCE_SLOTS];
    U32 advance_count;
} F_Font;

typedef struct F_Atlas { U32 w, h, x, y, row_h; B32 overflow; } F_Atlas;

static F_Atlas f_atlas;
static F_Font *f_fonts[F_MAX_FONTS];
static U8 f_gamma[256];

static void f_init(U32 atlas_w, U32 atlas_h)
{
    fdw_init();
    f_atlas.w = atlas_w;
    f_atlas.h = atlas_h;
    f_atlas.x = f_atlas.y = 1;
    f_atlas.row_h = 0;
    for (U32 i = 0; i < 256; i += 1) f_gamma[i] = (U8)(powf((F32)i / 255.0f, F_CONTRAST) * 255.0f + 0.5f);
}

// Whether a font family is installed (e.g. Segoe Fluent Icons is on Windows 11, not 10).
static int CALLBACK f_family_found(const LOGFONTW *lf, const TEXTMETRICW *tm, DWORD type, LPARAM found)
{
    (void)lf; (void)tm; (void)type;
    *(B32 *)found = 1;
    return 0;
}

static B32 f_family_installed(wchar_t *family)
{
    LOGFONTW lf = { 0 };
    lf.lfCharSet = DEFAULT_CHARSET;
    wcsncpy_s(lf.lfFaceName, ArrayCount(lf.lfFaceName), family, _TRUNCATE);
    B32 found = 0;
    HDC dc = GetDC(0);
    EnumFontFamiliesExW(dc, &lf, f_family_found, (LPARAM)&found, 0);
    ReleaseDC(0, dc);
    return found;
}

static F_Font *f_font_open(Arena *a, wchar_t *face, F32 px, int weight, B32 italic)
{
    F_Font *f = push_struct(a, F_Font);
    FdwMetrics m = { 0 };
    f->dw = fdw_font_open(face, px, weight, italic, &m);
    f->ascent = (F32)(int)(m.ascent + 0.5f);
    f->descent = (F32)(int)(m.descent + 0.5f);
    f->line_height = (F32)(int)(m.ascent + m.descent + m.line_gap + 0.5f);
    for (U32 i = 0; i < F_ADVANCE_SLOTS; i += 1) f->advances[i].advance = -1;
    for (U32 i = 0; i < F_MAX_FONTS; i += 1) if (!f_fonts[i]) { f_fonts[i] = f; break; }
    return f;
}

static void f_font_close(F_Font *f)
{
    if (!f) return;
    for (U32 i = 0; i < F_MAX_FONTS; i += 1) if (f_fonts[i] == f) f_fonts[i] = 0;
    if (f->dw) fdw_font_close(f->dw);
    f->dw = 0;
}

// Forget every cached glyph; fonts re-rasterize lazily.
static void f_atlas_reset(void)
{
    f_atlas.x = f_atlas.y = 1;
    f_atlas.row_h = 0;
    f_atlas.overflow = 0;
    for (U32 i = 0; i < F_MAX_FONTS; i += 1) {
        if (!f_fonts[i]) continue;
        memset(f_fonts[i]->glyphs, 0, sizeof(f_fonts[i]->glyphs));
        f_fonts[i]->glyph_count = 0;
    }
}

// Call before drawing a frame. Returns 1 if the atlas filled up last frame and was emptied (the
// caller should draw another frame, since some glyphs were missing from the last one).
static B32 f_begin_frame(void)
{
    if (!f_atlas.overflow) return 0;
    f_atlas_reset();
    return 1;
}

static B32 f_atlas_alloc(U32 w, U32 h, U32 *out_x, U32 *out_y)
{
    if (f_atlas.x + w + 1 > f_atlas.w) { f_atlas.x = 1; f_atlas.y += f_atlas.row_h + 1; f_atlas.row_h = 0; }
    if (f_atlas.y + h + 1 > f_atlas.h) { f_atlas.overflow = 1; return 0; }
    *out_x = f_atlas.x;
    *out_y = f_atlas.y;
    f_atlas.x += w + 1;
    f_atlas.row_h = Max(f_atlas.row_h, h);
    return 1;
}

static F_Glyph *f_glyph_at(F_Font *f, U32 cp, U32 bucket)
{
    static F_Glyph none;
    U32 key = (cp << 2) | (bucket & 3);
    U32 slot = (key * 2654435761u) & (F_GLYPH_SLOTS - 1);
    F_Glyph *g = 0;
    for (U32 probe = 0; probe < F_GLYPH_SLOTS; probe += 1) {
        F_Glyph *c = &f->glyphs[(slot + probe) & (F_GLYPH_SLOTS - 1)];
        if (c->used && c->key == key) return c;
        if (!c->used) { g = c; break; }
    }
    // Table nearly full: start over next frame (like a full atlas).
    if (!g || f->glyph_count > F_GLYPH_SLOTS * 3 / 4) { f_atlas.overflow = 1; if (!g) return &none; }

    U8 buf[128 * 128];
    FdwGlyph dg;
    if (!f->dw || !fdw_glyph(f->dw, cp, (F32)bucket / F_SUBPIXEL, &dg, buf, sizeof(buf))) {
        none.advance = f->line_height * 0.3f;
        return &none;
    }
    g->used = 1;
    g->key = key;
    g->advance = dg.advance;
    f->glyph_count += 1;
    if (!dg.has_bitmap) return g; // whitespace
    U32 ax, ay;
    if (!f_atlas_alloc((U32)dg.w, (U32)dg.h, &ax, &ay)) { g->used = 0; f->glyph_count -= 1; return &none; }
    for (int i = 0; i < dg.w * dg.h; i += 1) buf[i] = f_gamma[buf[i]];
    r_atlas_update(ax, ay, (U32)dg.w, (U32)dg.h, buf, (U32)dg.w);
    g->has_bitmap = 1;
    g->src = (Rect){ (F32)ax, (F32)ay, (F32)(ax + dg.w), (F32)(ay + dg.h) };
    g->w = (F32)dg.w;
    g->h = (F32)dg.h;
    g->off_x = (F32)dg.left;
    g->off_y = (F32)dg.top;
    return g;
}

static F32 f_advance(F_Font *f, U32 cp)
{
    U32 slot = (cp * 2654435761u) & (F_ADVANCE_SLOTS - 1);
    for (U32 probe = 0; probe < 64; probe += 1) {
        F_Advance *a = &f->advances[(slot + probe) & (F_ADVANCE_SLOTS - 1)];
        if (a->advance >= 0 && a->cp == cp) return a->advance;
        if (a->advance < 0) {
            F32 adv = f->dw ? fdw_advance(f->dw, cp) : 0;
            if (f->advance_count < F_ADVANCE_SLOTS * 3 / 4) { a->cp = cp; a->advance = adv; f->advance_count += 1; }
            return adv;
        }
    }
    return f->dw ? fdw_advance(f->dw, cp) : 0;
}

static F32 f_text_width(F_Font *f, String8 s)
{
    F32 w = 0;
    for (U64 i = 0; i < s.size;) {
        UnicodeDecode d = utf8_decode(s.str + i, s.size - i);
        w += f_advance(f, d.codepoint);
        i += d.advance;
    }
    return w;
}

static void f_draw_glyph(F_Font *f, U32 cp, F32 pen, F32 baseline, V4 color)
{
    F32 whole = floorf(pen);
    U32 bucket = (U32)((pen - whole) * F_SUBPIXEL) & (F_SUBPIXEL - 1);
    F_Glyph *g = f_glyph_at(f, cp, bucket);
    if (!g->has_bitmap) return;
    Rect dst = { whole + g->off_x, baseline + g->off_y, whole + g->off_x + g->w, baseline + g->off_y + g->h };
    r_glyph(dst, g->src, color);
}

// Draws text with its line top at y. If max_w > 0, truncates with "...". Returns the pen x after drawing.
static F32 f_draw_text(F_Font *f, F32 x, F32 y, String8 s, V4 color, F32 max_w)
{
    F32 baseline = (F32)(int)(y + f->ascent + 0.5f);
    F32 pen = x;
    F32 limit = max_w > 0 ? x + max_w : 1e9f;
    F32 dots_w = 0;
    if (max_w > 0 && f_text_width(f, s) > max_w) dots_w = f_text_width(f, str8_lit("..."));
    else limit = 1e9f;

    for (U64 i = 0; i < s.size;) {
        UnicodeDecode d = utf8_decode(s.str + i, s.size - i);
        F32 adv = f_advance(f, d.codepoint);
        if (pen + adv > limit - dots_w) {
            for (int k = 0; k < 3; k += 1) {
                f_draw_glyph(f, '.', pen, baseline, color);
                pen += f_advance(f, '.');
            }
            break;
        }
        f_draw_glyph(f, d.codepoint, pen, baseline, color);
        pen += adv;
        i += d.advance;
    }
    return pen;
}
