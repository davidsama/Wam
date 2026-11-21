// font_dwrite.h - the C side of font_dwrite.cpp (DirectWrite, which has no C interface).

#ifndef FONT_DWRITE_H
#define FONT_DWRITE_H

typedef unsigned int U32_FDW;
typedef void *FdwFontHandle;

typedef struct FdwMetrics { float ascent, descent, line_gap; } FdwMetrics; // pixels

typedef struct FdwGlyph {
    float advance;      // pixels, fractional
    int left, top;      // bitmap position relative to the pen on the baseline (top negative = above)
    int w, h;           // bitmap size; 0 for whitespace
    int has_bitmap;
    int missing;        // no font has this character (drawn as the font's box)
} FdwGlyph;

int  fdw_init(void);
// family e.g. L"Segoe UI"; weight like CSS (400 normal, 600 semibold, 700 bold).
FdwFontHandle fdw_font_open(const wchar_t *family, float px, int weight, FdwMetrics *out);
void fdw_font_close(FdwFontHandle f);
// Rasterizes one character at a sub-pixel x offset (0..1) into buf (8-bit coverage, w*h bytes,
// row pitch w). Returns 0 if buf (cap bytes) is too small.
int  fdw_glyph(FdwFontHandle f, unsigned codepoint, float subpixel_x, FdwGlyph *out, unsigned char *buf, int cap);
// Just the advance (pixels), without rasterizing: for measuring text.
float fdw_advance(FdwFontHandle f, unsigned codepoint);

#endif
