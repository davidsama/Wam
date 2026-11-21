// make_icon.c - draws wam's icon (a gold cog with a hammer across it) and writes res\wam.ico and
// res\logo.png. The shapes are signed distance functions on a 256x256 canvas, sampled 8x8 times per
// pixel, so every size is drawn from the shapes rather than scaled down from a bigger picture.
//
// Build and run from the repository root (in a Visual Studio developer prompt):
//   cl /nologo /O2 tools\make_icon.c /Fe:build\make_icon.exe /Fo:build\ ole32.lib windowscodecs.lib
//   build\make_icon.exe [preview folder]
// With a preview folder it also writes icon-<size>.png there for each size.

#define COBJMACROS
#include <windows.h>
#include <wincodec.h>
#include <math.h>
#include <stdio.h>

typedef struct { float r, g, b, a; } Color;
static Color hex(unsigned rgb, float a) { Color c = { ((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f, (rgb & 255) / 255.0f, a }; return c; }
static Color mix(Color x, Color y, float t) { Color c = { x.r + (y.r - x.r) * t, x.g + (y.g - x.g) * t, x.b + (y.b - x.b) * t, x.a + (y.a - x.a) * t }; return c; }

//- Shapes

enum { SHAPE_POLY, SHAPE_CIRCLE, SHAPE_BOX };

typedef struct {
    int kind;
    float x0, y0, x1, y1, radius;  // box: corners and corner radius; circle: center x0,y0 and radius
    float px[64], py[64]; int n;   // polygon
    float angle;                   // degrees, about the canvas center (the hammer is tilted)
    Color fill_top, fill_bottom;   // a vertical gradient (fill_top.a 0: no fill)
    Color stroke; float stroke_w;
} Shape;

static Shape shapes[32];
static int shape_count;

static float seg_dist(float px, float py, float ax, float ay, float bx, float by)
{
    float vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
    float t = (wx * vx + wy * vy) / (vx * vx + vy * vy);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    float dx = wx - vx * t, dy = wy - vy * t;
    return sqrtf(dx * dx + dy * dy);
}

// Signed distance: negative inside.
static float shape_dist(Shape *s, float x, float y)
{
    if (s->angle != 0) {
        float a = -s->angle * 3.14159265f / 180, c = cosf(a), sn = sinf(a);
        float dx = x - 128, dy = y - 128;
        x = 128 + dx * c - dy * sn;
        y = 128 + dx * sn + dy * c;
    }
    if (s->kind == SHAPE_CIRCLE) return sqrtf((x - s->x0) * (x - s->x0) + (y - s->y0) * (y - s->y0)) - s->radius;
    if (s->kind == SHAPE_BOX) {
        float cx = (s->x0 + s->x1) * 0.5f, cy = (s->y0 + s->y1) * 0.5f;
        float qx = fabsf(x - cx) - (s->x1 - s->x0) * 0.5f + s->radius, qy = fabsf(y - cy) - (s->y1 - s->y0) * 0.5f + s->radius;
        float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
        float in = qx > qy ? qx : qy;
        return sqrtf(ox * ox + oy * oy) + (in < 0 ? in : 0) - s->radius;
    }
    float d = 1e9f; int inside = 0;
    for (int i = 0, j = s->n - 1; i < s->n; j = i++) {
        float e = seg_dist(x, y, s->px[j], s->py[j], s->px[i], s->py[i]);
        if (e < d) d = e;
        if (((s->py[i] > y) != (s->py[j] > y)) && x < (s->px[j] - s->px[i]) * (y - s->py[i]) / (s->py[j] - s->py[i]) + s->px[i]) inside = !inside;
    }
    return inside ? -d : d;
}

static Shape *add(int kind, Color top, Color bottom, Color stroke, float stroke_w)
{
    Shape *s = &shapes[shape_count++];
    memset(s, 0, sizeof(*s));
    s->kind = kind; s->fill_top = top; s->fill_bottom = bottom; s->stroke = stroke; s->stroke_w = stroke_w;
    return s;
}

static Shape *box(float x0, float y0, float x1, float y1, float radius, Color top, Color bottom, Color stroke, float sw, float angle)
{
    Shape *s = add(SHAPE_BOX, top, bottom, stroke, sw);
    s->x0 = x0; s->y0 = y0; s->x1 = x1; s->y1 = y1; s->radius = radius; s->angle = angle;
    return s;
}

static void build_icon(void)
{
    Color gold = hex(0xf2c35a, 1), gold_dark = hex(0xc0862a, 1), bronze = hex(0x5a3810, 1), gold_light = hex(0xffe7a3, 1);
    Color blue_top = hex(0x24407a, 1), blue_bottom = hex(0x0f1a33, 1);
    Color steel_top = hex(0xe4eaf2, 1), steel_bottom = hex(0x8d99a8, 1), wood_top = hex(0x9a6232, 1), wood_bottom = hex(0x5e3818, 1);
    Color ink = hex(0x1e1006, 1), none = { 0 };
    shape_count = 0;

    // The cog: 12 trapezoid teeth around a gold ring.
    Shape *cog = add(SHAPE_POLY, gold, gold_dark, bronze, 7);
    int teeth = 12;
    float ro = 121, ri = 99;
    for (int i = 0; i < teeth; i += 1) {
        float a = i * 6.2831853f / teeth, w = 3.14159265f / teeth;
        float r4[4] = { ri, ro, ro, ri }, a4[4] = { a - w * 0.62f, a - w * 0.36f, a + w * 0.36f, a + w * 0.62f };
        for (int k = 0; k < 4; k += 1) {
            cog->px[cog->n] = 128 + r4[k] * cosf(a4[k] - 1.5707963f);
            cog->py[cog->n] = 128 + r4[k] * sinf(a4[k] - 1.5707963f);
            cog->n += 1;
        }
    }
    // The blue face, with a light inner ring.
    Shape *face = add(SHAPE_CIRCLE, blue_top, blue_bottom, bronze, 6);
    face->x0 = face->y0 = 128; face->radius = 74;
    Shape *ring = add(SHAPE_CIRCLE, none, none, mix(gold_light, blue_top, 0.25f), 3);
    ring->x0 = ring->y0 = 128; ring->radius = 64;

    // The hammer, tilted: handle, its gold grip band, the steel head and its gold collar.
    float tilt = -38;
    box(117, 68, 139, 232, 6, wood_top, wood_bottom, ink, 6, tilt);
    box(117, 194, 139, 210, 0, gold, gold_dark, ink, 5, tilt);
    box(70, 32, 186, 82, 8, steel_top, steel_bottom, ink, 6, tilt);
    box(112, 32, 144, 82, 0, gold, gold_dark, ink, 5, tilt);
}

// The color at canvas point (x, y): the shapes painted in order.
static Color sample(float x, float y, float stroke_scale)
{
    Color c = { 0 };
    for (int i = 0; i < shape_count; i += 1) {
        Shape *s = &shapes[i];
        float d = shape_dist(s, x, y), half = s->stroke_w * stroke_scale * 0.5f;
        Color p;
        if (d < -half && s->fill_top.a > 0) {
            float t = s->kind == SHAPE_CIRCLE ? (y - (s->y0 - s->radius)) / (2 * s->radius) : s->kind == SHAPE_BOX ? (y - s->y0) / (s->y1 - s->y0) : y / 256;
            p = mix(s->fill_top, s->fill_bottom, t < 0 ? 0 : t > 1 ? 1 : t);
        } else if (fabsf(d) <= half) {
            p = s->stroke;
        } else {
            continue;
        }
        // p over c (straight alpha)
        float a = p.a + c.a * (1 - p.a);
        if (a > 0) {
            c.r = (p.r * p.a + c.r * c.a * (1 - p.a)) / a;
            c.g = (p.g * p.a + c.g * c.a * (1 - p.a)) / a;
            c.b = (p.b * p.a + c.b * c.a * (1 - p.a)) / a;
        }
        c.a = a;
    }
    return c;
}

// BGRA, straight alpha. Small sizes get thicker outlines so the shapes stay apart.
static unsigned char *render(int size)
{
    unsigned char *px = malloc((size_t)size * size * 4);
    float k = 256.0f / size, stroke_scale = size <= 24 ? 1.6f : size <= 48 ? 1.25f : 1.0f;
    int ss = 8;
    for (int y = 0; y < size; y += 1) for (int x = 0; x < size; x += 1) {
        float r = 0, g = 0, b = 0, a = 0;
        for (int sy = 0; sy < ss; sy += 1) for (int sx = 0; sx < ss; sx += 1) {
            Color c = sample((x + (sx + 0.5f) / ss) * k, (y + (sy + 0.5f) / ss) * k, stroke_scale);
            r += c.r * c.a; g += c.g * c.a; b += c.b * c.a; a += c.a;
        }
        unsigned char *o = px + ((size_t)y * size + x) * 4;
        if (a > 0) { o[0] = (unsigned char)(b / a * 255 + 0.5f); o[1] = (unsigned char)(g / a * 255 + 0.5f); o[2] = (unsigned char)(r / a * 255 + 0.5f); }
        else o[0] = o[1] = o[2] = 0;
        o[3] = (unsigned char)(a / (ss * ss) * 255 + 0.5f);
    }
    return px;
}

//- PNG and ICO

static IWICImagingFactory *wic;

// Encodes BGRA pixels as PNG; returns the bytes (malloc) and their count.
static unsigned char *png_encode(unsigned char *px, int size, DWORD *bytes)
{
    IStream *stream = 0;
    IWICBitmapEncoder *enc = 0;
    IWICBitmapFrameEncode *frame = 0;
    CreateStreamOnHGlobal(0, TRUE, &stream);
    IWICImagingFactory_CreateEncoder(wic, &GUID_ContainerFormatPng, 0, &enc);
    IWICBitmapEncoder_Initialize(enc, stream, WICBitmapEncoderNoCache);
    IWICBitmapEncoder_CreateNewFrame(enc, &frame, 0);
    IWICBitmapFrameEncode_Initialize(frame, 0);
    IWICBitmapFrameEncode_SetSize(frame, size, size);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    IWICBitmapFrameEncode_SetPixelFormat(frame, &fmt);
    IWICBitmapFrameEncode_WritePixels(frame, size, size * 4, size * size * 4, px);
    IWICBitmapFrameEncode_Commit(frame);
    IWICBitmapEncoder_Commit(enc);
    STATSTG st;
    IStream_Stat(stream, &st, STATFLAG_NONAME);
    *bytes = (DWORD)st.cbSize.QuadPart;
    unsigned char *out = malloc(*bytes);
    LARGE_INTEGER zero = { 0 };
    IStream_Seek(stream, zero, STREAM_SEEK_SET, 0);
    ULONG got = 0;
    IStream_Read(stream, out, *bytes, &got);
    IWICBitmapFrameEncode_Release(frame);
    IWICBitmapEncoder_Release(enc);
    IStream_Release(stream);
    return out;
}

static int write_file(char *path, void *data, DWORD bytes)
{
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "can't write %s\n", path); return 0; }
    fwrite(data, 1, bytes, f);
    fclose(f);
    return 1;
}

int main(int argc, char **argv)
{
    CoInitializeEx(0, COINIT_MULTITHREADED);
    if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, 0, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&wic))) return 1;
    build_icon();
    CreateDirectoryA("res", 0);

    // The .ico: every size Windows asks for, each a PNG (supported since Vista).
    int sizes[] = { 16, 20, 24, 32, 40, 48, 64, 128, 256 };
    int n = sizeof(sizes) / sizeof(sizes[0]);
    unsigned char *png[16]; DWORD png_bytes[16];
    for (int i = 0; i < n; i += 1) {
        unsigned char *px = render(sizes[i]);
        png[i] = png_encode(px, sizes[i], &png_bytes[i]);
        if (argc > 1) {
            char path[MAX_PATH];
            snprintf(path, sizeof(path), "%s\\icon-%d.png", argv[1], sizes[i]);
            write_file(path, png[i], png_bytes[i]);
        }
        if (sizes[i] == 128) write_file("res\\logo.png", png[i], png_bytes[i]); // drawn in the window's header
        free(px);
    }
    DWORD total = 6 + 16 * n;
    for (int i = 0; i < n; i += 1) total += png_bytes[i];
    unsigned char *ico = calloc(1, total), *p = ico;
    p[2] = 1; p[4] = (unsigned char)n; p += 6;
    DWORD offset = 6 + 16 * n;
    for (int i = 0; i < n; i += 1, p += 16) {
        p[0] = (unsigned char)(sizes[i] >= 256 ? 0 : sizes[i]);
        p[1] = p[0];
        p[4] = 1; p[6] = 32;
        memcpy(p + 8, &png_bytes[i], 4);
        memcpy(p + 12, &offset, 4);
        memcpy(ico + offset, png[i], png_bytes[i]);
        offset += png_bytes[i];
    }
    if (!write_file("res\\wam.ico", ico, total)) return 1;
    printf("res\\wam.ico (%d sizes, %lu bytes), res\\logo.png\n", n, total);
    return 0;
}
