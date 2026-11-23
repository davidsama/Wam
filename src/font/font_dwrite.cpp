// font_dwrite.cpp - DirectWrite glyph rasterization behind a small C API (font_dwrite.h).
//
// DirectWrite's headers are C++ only, so this one file is C++ and compiled on its own; everything
// else stays C. It knows nothing about atlases or drawing: it opens fonts, measures them and turns
// one glyph at a time into an 8-bit coverage bitmap (grayscale antialiasing, natural symmetric
// rendering, fractional advances, sub-pixel x offsets). Characters the font lacks come from
// Windows' font fallback (Segoe UI Emoji, the CJK fonts, ...), scaled the way Windows scales them.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwrite_2.h>
#include <string.h>

extern "C" {
#include "font_dwrite.h"
}

static IDWriteFactory2 *fdw_factory;
static IDWriteFontCollection *fdw_system;
static IDWriteFontFallback *fdw_fallback;

struct FdwFallbackFace { IDWriteFontFace *face; FLOAT scale; };

struct FdwFont {
    IDWriteFontFace *face;
    WCHAR family[64];
    DWRITE_FONT_WEIGHT weight;
    FLOAT px;
    UINT16 em;
    // Faces fallback picked for characters this font lacks (kept for the font's lifetime). They're
    // tried first for the next missing character: a CJK text needs one face for hundreds of them.
    FdwFallbackFace fallback[16];
    U32_FDW fallback_count;
};

// The text MapCharacters asks about: one character, left to right, the user's locale.
class FdwOneChar : public IDWriteTextAnalysisSource {
public:
    WCHAR text[3];
    UINT32 len;
    FdwOneChar(UINT32 cp) {
        if (cp >= 0x10000) { cp -= 0x10000; text[0] = (WCHAR)(0xD800 + (cp >> 10)); text[1] = (WCHAR)(0xDC00 + (cp & 0x3FF)); len = 2; }
        else { text[0] = (WCHAR)cp; len = 1; }
        text[len] = 0;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWriteTextAnalysisSource)) { *out = this; return S_OK; }
        *out = 0; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }   // lives on the stack
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetTextAtPosition(UINT32 pos, const WCHAR **t, UINT32 *n) override {
        if (pos >= len) { *t = 0; *n = 0; } else { *t = text + pos; *n = len - pos; }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTextBeforePosition(UINT32 pos, const WCHAR **t, UINT32 *n) override {
        if (pos == 0 || pos > len) { *t = 0; *n = 0; } else { *t = text; *n = pos; }
        return S_OK;
    }
    DWRITE_READING_DIRECTION STDMETHODCALLTYPE GetParagraphReadingDirection() override { return DWRITE_READING_DIRECTION_LEFT_TO_RIGHT; }
    HRESULT STDMETHODCALLTYPE GetLocaleName(UINT32, UINT32 *n, const WCHAR **locale) override { *n = len; *locale = L""; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetNumberSubstitution(UINT32, UINT32 *n, IDWriteNumberSubstitution **s) override { *n = len; *s = 0; return S_OK; }
};

extern "C" int fdw_init(void)
{
    if (fdw_factory) return 1;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory2), (IUnknown **)&fdw_factory))) return 0;
    if (FAILED(fdw_factory->GetSystemFontCollection(&fdw_system, FALSE))) return 0;
    fdw_factory->GetSystemFontFallback(&fdw_fallback); // optional: without it, missing characters show as boxes
    return 1;
}

static IDWriteFontFace *fdw_open_face(const WCHAR *family, DWRITE_FONT_WEIGHT weight, int italic)
{
    UINT32 index = 0;
    BOOL exists = FALSE;
    if (FAILED(fdw_system->FindFamilyName(family, &index, &exists)) || !exists) return 0;
    IDWriteFontFamily *fam = 0;
    IDWriteFont *font = 0;
    IDWriteFontFace *face = 0;
    if (SUCCEEDED(fdw_system->GetFontFamily(index, &fam)) &&
        SUCCEEDED(fam->GetFirstMatchingFont(weight, DWRITE_FONT_STRETCH_NORMAL, italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, &font))) {
        font->CreateFontFace(&face);
    }
    if (font) font->Release();
    if (fam) fam->Release();
    return face;
}

extern "C" FdwFontHandle fdw_font_open(const wchar_t *family, float px, int weight, int italic, FdwMetrics *out)
{
    if (!fdw_init()) return 0;
    DWRITE_FONT_WEIGHT w = (DWRITE_FONT_WEIGHT)(weight ? weight : 400);
    IDWriteFontFace *face = fdw_open_face(family, w, italic);
    if (!face) face = fdw_open_face(L"Segoe UI", w, italic);
    if (!face) return 0;
    FdwFont *f = (FdwFont *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(FdwFont)); // no C++ runtime needed
    if (!f) { face->Release(); return 0; }
    f->face = face;
    wcsncpy_s(f->family, family, _TRUNCATE);
    f->weight = w;
    f->px = px;
    DWRITE_FONT_METRICS m;
    face->GetMetrics(&m);
    f->em = m.designUnitsPerEm;
    FLOAT k = px / m.designUnitsPerEm;
    out->ascent = m.ascent * k;
    out->descent = m.descent * k;
    out->line_gap = m.lineGap * k;
    return f;
}

extern "C" void fdw_font_close(FdwFontHandle h)
{
    FdwFont *f = (FdwFont *)h;
    if (!f) return;
    for (U32_FDW i = 0; i < f->fallback_count; i += 1) f->fallback[i].face->Release();
    f->face->Release();
    HeapFree(GetProcessHeap(), 0, f);
}

// The face that has cp: the font's own, else what Windows' fallback picks (and its size scale).
static IDWriteFontFace *fdw_face_for(FdwFont *f, UINT32 cp, UINT16 *glyph, FLOAT *scale)
{
    *scale = 1;
    UINT16 g = 0;
    f->face->GetGlyphIndices(&cp, 1, &g);
    if (g || !fdw_fallback) { *glyph = g; return f->face; }
    for (U32_FDW i = 0; i < f->fallback_count; i += 1) {
        f->fallback[i].face->GetGlyphIndices(&cp, 1, &g);
        if (g) { *glyph = g; *scale = f->fallback[i].scale; return f->fallback[i].face; }
    }
    FdwOneChar src(cp);
    UINT32 mapped_len = 0;
    IDWriteFont *mapped = 0;
    FLOAT sc = 1;
    if (FAILED(fdw_fallback->MapCharacters(&src, 0, src.len, fdw_system, f->family, f->weight, DWRITE_FONT_STYLE_NORMAL,
                                           DWRITE_FONT_STRETCH_NORMAL, &mapped_len, &mapped, &sc)) || !mapped) {
        *glyph = 0;
        return f->face;
    }
    IDWriteFontFace *face = 0;
    mapped->CreateFontFace(&face);
    mapped->Release();
    if (!face) { *glyph = 0; return f->face; }
    sc = sc > 0 ? sc : 1;
    // Keep one reference per distinct face.
    for (U32_FDW i = 0; i < f->fallback_count; i += 1) {
        if (f->fallback[i].face == face) { face->Release(); face = f->fallback[i].face; goto have; }
    }
    if (f->fallback_count < 16) { f->fallback[f->fallback_count].face = face; f->fallback[f->fallback_count].scale = sc; f->fallback_count += 1; }
    else { face->Release(); *glyph = 0; return f->face; }
have:
    face->GetGlyphIndices(&cp, 1, &g);
    *glyph = g;
    *scale = sc;
    return face;
}

extern "C" float fdw_advance(FdwFontHandle h, unsigned cp)
{
    FdwFont *f = (FdwFont *)h;
    if (!f) return 0;
    UINT16 g = 0;
    FLOAT scale = 1;
    IDWriteFontFace *face = fdw_face_for(f, cp, &g, &scale);
    DWRITE_FONT_METRICS m;
    face->GetMetrics(&m);
    DWRITE_GLYPH_METRICS gm;
    if (FAILED(face->GetDesignGlyphMetrics(&g, 1, &gm, FALSE))) return 0;
    return gm.advanceWidth * f->px * scale / m.designUnitsPerEm;
}

extern "C" int fdw_glyph(FdwFontHandle h, unsigned cp, float subpixel_x, FdwGlyph *out, unsigned char *buf, int cap)
{
    FdwFont *f = (FdwFont *)h;
    memset(out, 0, sizeof(*out));
    if (!f) return 0;
    UINT16 g = 0;
    FLOAT scale = 1;
    IDWriteFontFace *face = fdw_face_for(f, cp, &g, &scale);
    FLOAT size = f->px * scale;

    DWRITE_FONT_METRICS m;
    face->GetMetrics(&m);
    DWRITE_GLYPH_METRICS gm;
    if (SUCCEEDED(face->GetDesignGlyphMetrics(&g, 1, &gm, FALSE))) out->advance = gm.advanceWidth * size / m.designUnitsPerEm;
    out->missing = g == 0;

    DWRITE_GLYPH_RUN run = { 0 };
    FLOAT advance = 0;
    DWRITE_GLYPH_OFFSET offset = { 0, 0 };
    run.fontFace = face;
    run.fontEmSize = size;
    run.glyphCount = 1;
    run.glyphIndices = &g;
    run.glyphAdvances = &advance;
    run.glyphOffsets = &offset;

    IDWriteGlyphRunAnalysis *an = 0;
    HRESULT hr = fdw_factory->CreateGlyphRunAnalysis(&run, 0, DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC, DWRITE_MEASURING_MODE_NATURAL,
                                                     DWRITE_GRID_FIT_MODE_DEFAULT, DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE,
                                                     subpixel_x, 0, &an);
    if (FAILED(hr) || !an) return 1; // measured, nothing to draw
    RECT r;
    int ok = 1;
    if (SUCCEEDED(an->GetAlphaTextureBounds(DWRITE_TEXTURE_ALIASED_1x1, &r)) && r.right > r.left && r.bottom > r.top) {
        int w = r.right - r.left, hgt = r.bottom - r.top;
        if (w * hgt <= cap && SUCCEEDED(an->CreateAlphaTexture(DWRITE_TEXTURE_ALIASED_1x1, &r, buf, (UINT32)cap))) {
            out->w = w;
            out->h = hgt;
            out->left = r.left;
            out->top = r.top;
            out->has_bitmap = 1;
        } else {
            ok = 0;
        }
    }
    an->Release();
    return ok;
}
