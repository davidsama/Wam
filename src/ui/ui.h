// ui.h - a small immediate-mode UI library on top of render/ (rounded rects, glyphs), font/ and
// the window layer wm/. No platform code: the window, its input and the native pieces are wm's,
// files, threads and the clipboard os's.
//
// Nothing here knows about the app using it. Each frame the app calls widgets; a widget draws
// itself with ui.theme / ui.font and returns what the user did with it (clicked, toggled...).
// The window's mouse state is in ui.in and its keys and typed text in ui.events, for this frame;
// the frame arena is cleared every frame.
//
//   ui.c        state, geometry, theme, fonts, frames, ornaments, widgets, text editing
//   ui_image.c  pictures from URLs (icons, thumbnails), fetched and decoded in the background
//
// Everything is `static` (unity build): include ui.h after render/, font/ and wm/wm.h, then ui.c
// and ui_image.c.

#ifndef UI_H
#define UI_H

//- State

typedef struct UI_Theme {
    V4 bg, panel, hover, button, button_hi, border, border_hi, selected, line;
    V4 text, dim, faint;
    V4 heading;              // titles and section headings
    V4 accent, accent_hi, accent_bg;
    V4 ok, danger, danger_hi, error, warn;
    // Ornaments (see ui_panel, ui_divider, ui_gem...): the metal of rims and trim, light to dark,
    // and the gem color. `ornate` (0..1) is how much the widgets show of it: 0 draws everything flat
    // (plain fills, plain borders, no rivets or gems), 1 draws bevels, gradients and trim in full.
    V4 trim_hi, trim, trim_dark, gem;
    F32 ornate;
} UI_Theme;

// caption: the 12px font for secondary text ("small" would collide with a macro in rpcndr.h).
// glyphs: Windows' own icon font (Segoe Fluent Icons, else Segoe MDL2 Assets): 10px for the window
// buttons, glyphs_big (14px) for icons in rows.
typedef struct UI_Fonts { F_Font *normal, *bold, *caption, *title, *icon, *glyphs, *glyphs_big; } UI_Fonts;

typedef struct UI_State {
    F32 scale;               // DPI scale: 1.0 at 96 dpi
    UI_Theme theme;
    UI_Fonts font;
    Arena *font_arena;
    WM_Input in;             // the mouse, this frame
    WM_Event *events;        // keys and typed text since the last frame, in order
    U32 event_count;
    Arena *frame_arena;     // scratch for this frame, cleared before each one
    F32 width, height;       // client size in pixels
    Rect clip;               // widgets ignore the mouse outside it (e.g. the visible part of a list)
    Rect overlay;            // a popup layer such as a dropdown: it hides the mouse from what's below
    B32 overlay_on, in_overlay;
    Rect block[8];           // this frame: where widgets don't see the mouse (a scrollbar being used)
    U32 block_count;
    int cursor;              // UI_CURSOR_*: what the mouse pointer should look like (widgets set it each frame)
    F64 frame_ms;            // CPU time of the last frame
    U32 fps;                 // frames drawn in the last second (0 when idle: frames only come on demand)
    String8 tip;             // this frame's tooltip (ui_tooltip), drawn over everything at the end
    Rect tip_anchor;
} UI_State;

static UI_State ui;

enum { UI_CURSOR_ARROW = WM_CURSOR_ARROW, UI_CURSOR_SIZE_WE = WM_CURSOR_SIZE_WE, UI_CURSOR_HAND = WM_CURSOR_HAND };

//- Geometry, color, hit testing

static Rect rect(F32 x0, F32 y0, F32 x1, F32 y1);
static B32  point_in(Rect r, F32 x, F32 y);
static V4   ui_hex(U32 rgb, F32 alpha);       // 0xRRGGBB
static V4   ui_mix(V4 a, V4 b, F32 t);         // a to b; t 0..1
static F32  ui_px(void);                       // one physical pixel, at least 1
static B32  ui_mouse_in(Rect r);               // respects the overlay and blocked areas
static void ui_block_mouse(Rect r);            // widgets drawn after this, this frame, don't see the mouse in r
static B32  ui_clicked(Rect r);                // pressed and released inside r, this frame
static UI_Theme ui_theme_dark(void);         // flat, neutral dark gray-blue (ornate 0)
static UI_Theme ui_theme_tinker(void);       // midnight blue with gold and bronze trim, bevels and gems (ornate 1)
static void ui_fonts_load(void);               // (re)builds ui.font for ui.scale (Segoe UI)

//- Frames
//
// After wm_window_open, ui_init sets up the renderer and fonts for the window. Then each WM_App.frame
// is ui_frame(f, build): it takes the frame's size, scale and input, calls build (which calls the
// widgets; it returns 1 to animate) and presents what it drew.

static B32 ui_init(void);                              // 0 if Direct3D 11 isn't available
static B32 ui_frame(WM_Frame *f, B32 (*build)(void));  // returns 1 for another frame (animation)
// Minimize, maximize and close in the top-right corner, h tall, ending at `right`; returns their
// left edge. For the app's own title bar (wm_caption).
static F32 ui_window_buttons(F32 right, F32 h);
// Testing: when ui_frame_log is set, each frame appends its timings there; when ui_shot_dir is
// set, F12 saves the next frame there as shot-N.bmp (and doesn't reach the app).
static String8 ui_frame_log, ui_shot_dir;

//- Ornaments
//
// Drawing pieces that follow the theme's trim and `ornate` level, so a view gets the theme's look
// by using them instead of plain rects. With ornate 0 they fall back to flat fills and lines.

// A raised plate in bg: what ui_button draws. Gradient, rim and a lit top edge when ornate.
static void ui_plate(Rect r, V4 bg, F32 radius);
// A panel in two passes: ui_panel fills it (before its content), ui_panel_rim draws the metal rim
// and corner studs over the content's edges (after it).
static void ui_panel(Rect r, F32 radius);
static void ui_panel_rim(Rect r, F32 radius);
// A horizontal rule from x0 to x1 centered on y: metal that fades out at both ends, with a gem in
// the middle when ornate (a plain line otherwise).
static void ui_divider(F32 x0, F32 x1, F32 y);
// A cut gem (a diamond with a lit facet) centered on (cx, cy), `size` tall.
static void ui_gem(F32 cx, F32 cy, F32 size, V4 color);
// A cog wheel centered on (cx, cy), r its outer radius, turned by angle (radians).
static void ui_cog(F32 cx, F32 cy, F32 r, F32 angle, V4 color);
// A turning cog, for "working on it". Returns 1 (keeps frames coming).
static B32  ui_spinner(F32 cx, F32 cy, F32 r);
// A rivet: a small domed bolt head.
static void ui_rivet(F32 cx, F32 cy, F32 r);

//- Widgets
//
// Sizes are in pixels already scaled by ui.scale. Buttons and checkboxes react only inside ui.clip.

static void ui_text_centered(F_Font *f, Rect r, String8 s, V4 color);
static void ui_pill(Rect r, String8 label, F_Font *f, V4 fg, V4 bg);              // fully rounded label
static B32  ui_button(Rect r, String8 label, V4 fg, V4 bg, V4 bg_hot);            // true when clicked
static F32  ui_button_width(String8 label);
static B32  ui_checkbox(F32 x, F32 center_y, B32 checked);                        // 16px; true when clicked
// Draws text with the bytes at `marks` (sorted offsets, e.g. fuzzy-match positions) in `hi`.
static F32  ui_text_highlighted(F_Font *f, F32 x, F32 y, String8 text, U8 *marks, U32 mark_count, V4 base, V4 hi, F32 max_w);
// A thin bar sliding along r's top edge, for "loading". Returns 1 (keeps frames coming).
static B32  ui_loading_bar(Rect r);
// Word-wrapped paragraphs ('\n' ends a line; "• " lines hang-indent). Draws only the lines inside
// [vis_y0, vis_y1); returns the total height.
static F32  ui_text_wrapped(F_Font *f, F32 x, F32 y, F32 w, String8 text, V4 color, F32 vis_y0, F32 vis_y1);
// Pictures from URLs: see ui_image.c (ui_image(), ui_image_fetch).

// A handle to drag (e.g. a divider between panes): true while it's being dragged, with the mouse at
// ui.in.mx / my. Shows the left-right resize pointer while hovered or dragged. *dragging is its state.
static B32  ui_drag_handle(Rect zone, B32 *dragging);

// A tooltip for what's under the mouse: call it while `anchor` is hovered. It's drawn after the
// frame, over everything, below the anchor (above it near the window's bottom).
static void ui_tooltip(Rect anchor, String8 text);

// Smooth wheel scrolling for a list of content_h pixels shown in `list`, and its scrollbar along the
// right edge: drag the thumb, or press beside it to jump there. Call ui_scroll_list before drawing
// the rows (it takes scrollbar clicks before they can) and ui_scrollbar after. Returns 1 while animating.
typedef struct UI_Scroll { F32 pos, target; B32 dragging; F32 grab; } UI_Scroll;
static B32  ui_scroll_list(UI_Scroll *sc, Rect list, F32 content_h);
static void ui_scrollbar(UI_Scroll *sc, Rect list, F32 content_h);

//- Overlay (dropdowns, popups drawn on top)
//
// Set the overlay's rect before drawing what's under it (so those widgets ignore the mouse there),
// then draw the overlay between ui_overlay_begin/end at the end of the frame.

static void ui_overlay_set(Rect r, B32 on);
static void ui_overlay_begin(void);
static void ui_overlay_end(void);

//- Text editing

typedef struct UI_TextBuf { char s[256]; U32 len; } UI_TextBuf;
static String8 ui_text(UI_TextBuf *t);
static void ui_text_insert(UI_TextBuf *t, U32 codepoint);
static void ui_text_backspace(UI_TextBuf *t);     // removes one UTF-8 character
static void ui_text_clear(UI_TextBuf *t);         // also wipes the bytes (it may have held a secret)
static void ui_text_paste(UI_TextBuf *t, String8 utf8);
// Edits t with one event: typed text, Backspace, Ctrl+Backspace (clears), Ctrl+V (pastes).
// Returns 1 if it used the event; Enter, Escape and the other keys are left to the caller.
static B32  ui_text_edit(UI_TextBuf *t, WM_Event *e);
// A one-line field: text (or one dot per character when masked) with a caret, or the hint when empty.
static void ui_text_field(Rect r, UI_TextBuf *t, String8 hint, B32 masked, V4 border);

#endif
