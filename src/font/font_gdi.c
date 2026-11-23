// font_gdi.c - glyph rasterization with GDI (GetGlyphOutlineW, 65-level grayscale),
// cached on demand into a shared R8 atlas with a simple shelf packer.
//
// The fallback backend (build with /DWAM_FONT_GDI): pure C, no dependencies. font_dwrite.c is the
// default: better antialiasing and spacing, and font fallback.

#define F_GLYPH_SLOTS 2048

typedef struct F_Glyph {
    U32 codepoint;
    B32 used;
    B32 has_bitmap;
    Rect src;        // atlas pixels
    F32 off_x;       // bitmap left relative to pen
    F32 off_y;       // bitmap top relative to baseline (negative = above)
    F32 w, h;
    F32 advance;
} F_Glyph;

typedef struct F_Font {
    HFONT hfont;
    F32 ascent, descent, line_height;
    F_Glyph glyphs[F_GLYPH_SLOTS];
} F_Font;

typedef struct F_Atlas { U32 w, h, x, y, row_h; } F_Atlas;

static HDC f_dc;
static F_Atlas f_atlas;

static void f_init(U32 atlas_w, U32 atlas_h)
{
    f_dc = CreateCompatibleDC(0);
    f_atlas.w = atlas_w;
    f_atlas.h = atlas_h;
    f_atlas.x = f_atlas.y = 1;
    f_atlas.row_h = 0;
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
    f->hfont = CreateFontW(-(int)(px + 0.5f), 0, 0, 0, weight, italic, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS,
                           CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, face);
    SelectObject(f_dc, f->hfont);
    TEXTMETRICW tm;
    GetTextMetricsW(f_dc, &tm);
    f->ascent = (F32)tm.tmAscent;
    f->descent = (F32)tm.tmDescent;
    f->line_height = (F32)(tm.tmHeight + tm.tmExternalLeading);
    return f;
}

static void f_font_close(F_Font *f) { if (f && f->hfont) DeleteObject(f->hfont); }

// font_dwrite.c empties a full atlas here; GDI's never does (glyphs past it just aren't drawn).
static B32 f_begin_frame(void) { return 0; }

// Forget every cached glyph (e.g. after a DPI change); fonts re-rasterize lazily.
static void f_atlas_reset(void) { f_atlas.x = f_atlas.y = 1; f_atlas.row_h = 0; }

static B32 f_atlas_alloc(U32 w, U32 h, U32 *out_x, U32 *out_y)
{
    if (f_atlas.x + w + 1 > f_atlas.w) { f_atlas.x = 1; f_atlas.y += f_atlas.row_h + 1; f_atlas.row_h = 0; }
    if (f_atlas.y + h + 1 > f_atlas.h) return 0; // TODO: evict / grow
    *out_x = f_atlas.x;
    *out_y = f_atlas.y;
    f_atlas.x += w + 1;
    f_atlas.row_h = Max(f_atlas.row_h, h);
    return 1;
}

static F_Glyph *f_glyph(F_Font *f, U32 cp)
{
    U32 slot = (cp * 2654435761u) & (F_GLYPH_SLOTS - 1);
    for (U32 probe = 0; probe < F_GLYPH_SLOTS; probe += 1) {
        F_Glyph *g = &f->glyphs[(slot + probe) & (F_GLYPH_SLOTS - 1)];
        if (g->used && g->codepoint == cp) return g;
        if (!g->used) { slot = (slot + probe) & (F_GLYPH_SLOTS - 1); break; }
    }
    F_Glyph *g = &f->glyphs[slot];
    g->used = 1;
    g->codepoint = cp;

    UINT ch = cp <= 0xFFFF ? cp : '?';
    SelectObject(f_dc, f->hfont);
    GLYPHMETRICS gm;
    MAT2 identity = { { 0, 1 }, { 0, 0 }, { 0, 0 }, { 0, 1 } };
    DWORD size = GetGlyphOutlineW(f_dc, ch, GGO_GRAY8_BITMAP, &gm, 0, 0, &identity);
    if (size == GDI_ERROR) { g->advance = f->line_height * 0.3f; return g; }
    g->advance = (F32)gm.gmCellIncX;
    if (size == 0) return g; // whitespace

    Temp scratch = scratch_begin(0);
    U8 *raw = push_array_nz(scratch.arena, U8, size);
    GetGlyphOutlineW(f_dc, ch, GGO_GRAY8_BITMAP, &gm, size, raw, &identity);
    U32 w = gm.gmBlackBoxX, h = gm.gmBlackBoxY, pitch = (w + 3) & ~3u;
    U8 *px = push_array_nz(scratch.arena, U8, w * h);
    for (U32 y = 0; y < h; y += 1) {
        for (U32 x = 0; x < w; x += 1) {
            U32 v = raw[y * pitch + x] * 255u / 64u;
            px[y * w + x] = (U8)Min(v, 255u);
        }
    }
    U32 ax, ay;
    if (f_atlas_alloc(w, h, &ax, &ay)) {
        r_atlas_update(ax, ay, w, h, px, w);
        g->has_bitmap = 1;
        g->src.x0 = (F32)ax; g->src.y0 = (F32)ay;
        g->src.x1 = (F32)(ax + w); g->src.y1 = (F32)(ay + h);
        g->w = (F32)w; g->h = (F32)h;
        g->off_x = (F32)gm.gmptGlyphOrigin.x;
        g->off_y = -(F32)gm.gmptGlyphOrigin.y;
    }
    scratch_end(scratch);
    return g;
}

static F32 f_text_width(F_Font *f, String8 s)
{
    F32 w = 0;
    for (U64 i = 0; i < s.size;) {
        UnicodeDecode d = utf8_decode(s.str + i, s.size - i);
        w += f_glyph(f, d.codepoint)->advance;
        i += d.advance;
    }
    return w;
}

// Draws text with its line top at y. If max_w > 0, truncates with "...". Returns the pen x after drawing.
static F32 f_draw_text(F_Font *f, F32 x, F32 y, String8 s, V4 color, F32 max_w)
{
    F32 baseline = (F32)(int)(y + f->ascent + 0.5f);
    F32 pen = (F32)(int)(x + 0.5f);
    F32 limit = max_w > 0 ? x + max_w : 1e9f;
    F32 dots_w = 0;
    if (max_w > 0 && f_text_width(f, s) > max_w) dots_w = f_text_width(f, str8_lit("..."));
    else limit = 1e9f;

    for (U64 i = 0; i < s.size;) {
        UnicodeDecode d = utf8_decode(s.str + i, s.size - i);
        F_Glyph *g = f_glyph(f, d.codepoint);
        if (pen + g->advance > limit - dots_w) {
            for (int k = 0; k < 3; k += 1) {
                F_Glyph *dot = f_glyph(f, '.');
                if (dot->has_bitmap) {
                    Rect dst = { pen + dot->off_x, baseline + dot->off_y, pen + dot->off_x + dot->w, baseline + dot->off_y + dot->h };
                    r_glyph(dst, dot->src, color);
                }
                pen += dot->advance;
            }
            break;
        }
        if (g->has_bitmap) {
            Rect dst = { pen + g->off_x, baseline + g->off_y, pen + g->off_x + g->w, baseline + g->off_y + g->h };
            r_glyph(dst, g->src, color);
        }
        pen += g->advance;
        i += d.advance;
    }
    return pen;
}
