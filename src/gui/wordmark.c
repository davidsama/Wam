// gui/wordmark.c - wam's name in the header: rune-like capitals drawn from strokes in the theme's
// metal (no font), with the theme's gem for the A's crossbar. Part of main_gui.c's unity build.

// Its strokes in letter heights (x0, y0, x1, y1): the same thickness throughout, as if carved.
static const F32 wordmark_strokes[][4] = {
    { 0.08f, 0, 0.30f, 1 }, { 0.30f, 1, 0.56f, 0.35f }, { 0.56f, 0.35f, 0.82f, 1 }, { 0.82f, 1, 1.04f, 0 },   // W
    { 1.26f, 1, 1.64f, 0 }, { 1.64f, 0, 2.02f, 1 },                                                        // A
    { 2.26f, 1, 2.26f, 0 }, { 2.26f, 0, 2.68f, 0.62f }, { 2.68f, 0.62f, 3.10f, 0 }, { 3.10f, 0, 3.10f, 1 },   // M
};
#define WORDMARK_STROKE 0.075f // half a stroke's thickness

static F32 wordmark_width(F32 h) { return 3.18f * h; }

// The metal at y (0..1 down the letters): one gradient down the whole word, not one per stroke.
static V4 wordmark_color(UI_Theme *t, F32 y)
{
    if (t->ornate <= 0) return t->text;
    return ui_mix(ui_lit(t->trim_hi, 0.15f), ui_mix(t->trim_hi, t->trim, 0.6f), Clamp(0.0f, y, 1.0f));
}

// The letters, h tall, from (x, y), their top left.
static void draw_wordmark(F32 x, F32 y, F32 h)
{
    UI_Theme *t = &ui.theme;
    for (U32 i = 0; i < ArrayCount(wordmark_strokes); i += 1) {
        const F32 *k = wordmark_strokes[i];
        F32 w = WORDMARK_STROKE;
        r_taper(x + k[0] * h, y + k[1] * h, x + k[2] * h, y + k[3] * h, w * h, w * h,
                wordmark_color(t, Min(k[1], k[3]) - w), wordmark_color(t, Max(k[1], k[3]) + w));
    }
    ui_gem(x + 1.64f * h, y + 0.73f * h, 0.52f * h, t->gem);
}
