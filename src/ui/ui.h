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
    // What the ornaments are made of (UI_STYLE_*): the emblem in a divider, the pieces on a panel's
    // corners, the spinner. `spark` is a second color they set against the gem's (embers in a
    // flame, frost on an ice crystal...), `aura` a glow over the top of the window and of panels
    // (alpha 0: none).
    int style;
    V4 spark, aura;
} UI_Theme;

enum {
    UI_STYLE_GEARWORK, // cogs, rivets and cut diamonds
    UI_STYLE_VOID,     // dark orbs in crescents, little stars
    UI_STYLE_FROST,    // ice crystals and shards
    UI_STYLE_FLAME,    // flames and embers
    UI_STYLE_FEL,      // horned orbs and sigils
    UI_STYLE_BLOOD,    // crossed blades, spikes and studded bosses
    UI_STYLE_HOLY,     // sunbursts and sparkles
    UI_STYLE_NATURE,   // leaves and berries
    UI_STYLE_STORM,    // lightning bolts and orbs in copper rings
    UI_STYLE_COUNT
};

// caption: the 12px font for secondary text ("small" would collide with a macro in rpcndr.h).
// glyphs: Windows' own icon font (Segoe Fluent Icons, else Segoe MDL2 Assets): 10px for the window
// buttons, glyphs_big (14px) for icons in rows.
typedef struct UI_Fonts { F_Font *normal, *bold, *caption, *title, *icon, *glyphs, *glyphs_big; } UI_Fonts;

// A widget's identity from one frame to the next: a hash of its label (or a name, or an address)
// within the current scope (ui_scope_push). 0 = none.
typedef struct UI_Id { U64 v; } UI_Id;

#define UI_SCOPE_MAX 16
#define UI_FOCUS_MAX 256
#define UI_ANIM_SLOTS 256
typedef struct UI_Focusable { UI_Id id; B32 in_overlay; } UI_Focusable;
typedef struct UI_AnimSlot { UI_Id id; F32 value; U64 frame; } UI_AnimSlot;

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
    F32 dt;                  // seconds since the last frame, for animation (at most 1/30: after a pause, animations start)
    U64 frame_index;

    // Identity: see ui_signal.
    U64 scope[UI_SCOPE_MAX]; // seeds of the scopes pushed (the top one hashes new ids)
    U32 scope_count;
    UI_Id hot;               // the last widget under the mouse this frame
    UI_Id active;            // the widget the left button was pressed on, while it's held (and in the frame it's let go)
    F32 drag_grab;           // the active widget's own drag state (e.g. where on a scrollbar's thumb it was grabbed)
    UI_Id focus;             // the widget the keyboard is on (Tab / Shift+Tab), 0 = none
    B32 focus_visible;       // focus came from the keyboard: show the ring (a mouse press or typing hides it)
    B32 focus_activate;      // Enter or Space this frame: the focused widget is clicked
    B32 eat_space_text;      // Space activated the focused widget: its ' ' isn't typing
    B32 focus_seen;          // the focused widget was drawn this frame
    UI_Focusable focusables[UI_FOCUS_MAX], focusables_prev[UI_FOCUS_MAX]; // in drawing order, this frame and the last
    U32 focusable_count, focusable_prev_count;
    UI_AnimSlot anim[UI_ANIM_SLOTS];
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
static B32  ui_clicked(Rect r);                // pressed and released inside r, this frame (no identity: see ui_signal)
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

//- Identity, interaction, focus, animation
//
// A widget that remembers anything from one frame to the next (being pressed, dragged, focused,
// half-way through a hover fade) has a UI_Id. ui_id(label) hashes the label within the current
// scope: push a scope for each repeated part (ui_list_row does it per row), so two rows' "Install"
// buttons are different widgets. In a label, "text##more" shows only text but hashes it all;
// "text###key" hashes only key, so a label whose text changes ("Update  3") keeps its identity.
//
// ui_signal(id, r, flags) is what the user does to the widget this frame. Pressing the left button
// on it makes it active until the button is let go: it gets the drag even when the mouse leaves it.
// UI_FOCUSABLE widgets join the Tab order (only those in the overlay, while a popup is up); Enter or
// Space clicks the focused one, Esc lets go of it, and so do typing and a mouse press.

static UI_Id ui_id(String8 label);              // see "##" / "###" above
static UI_Id ui_id_ptr(void *p);                // for state with an address (a UI_Scroll)
static B32   ui_id_match(UI_Id a, UI_Id b);
static void  ui_scope_push(String8 key);
static void  ui_scope_push_u64(U64 key);
static void  ui_scope_pop(void);
static String8 ui_label_text(String8 label);    // what's shown of a label ("text##more" -> "text")

enum { UI_FOCUSABLE = 1 };
typedef struct UI_Signal {
    B32 hovering;            // the mouse is over it (ui_mouse_in)
    B32 pressed;             // the left button went down on it this frame (it's now active)
    B32 dragging;            // it's active and the button is still held (the mouse may be elsewhere)
    B32 released;            // the button went up after a press on it (wherever the mouse is)
    B32 clicked;             // pressed and released on it, or Enter / Space while it has the focus
    B32 focused;             // it has the keyboard focus
    B32 keyboard;            // the click was Enter / Space (the mouse may be anywhere)
} UI_Signal;
static UI_Signal ui_signal(UI_Id id, Rect r, U32 flags);
static B32  ui_is_active(UI_Id id);
// The keyboard focus ring around r (radius: r's corners), if id has the focus and it came from the keyboard.
static void ui_focus_ring(UI_Id id, Rect r, F32 radius);
// A value per widget easing toward target (0..1) over about a tenth of a second, e.g. a hover fade:
// ui_mix(color, hot_color, ui_anim(id, hovering)). It asks for frames until it gets there; a widget
// seen for the first time starts at target.
static F32  ui_anim(UI_Id id, F32 target);

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
// A horizontal rule from x0 to x1 centered on y: metal that fades out at both ends, with the
// theme's emblem in the middle and its motifs beside it when ornate (a plain line otherwise).
static void ui_divider(F32 x0, F32 x1, F32 y);
// The theme's emblem (its style's centerpiece: a cut diamond, a frost crystal, a flame...) in
// theme t's colors, centered on (cx, cy), about `size` across. For a theme other than ui.theme
// too, e.g. to show it in a picker.
static void ui_emblem(UI_Theme *t, F32 cx, F32 cy, F32 size);
// The theme's piece for a corner of a panel, centered on (cx, cy), about `size` across. (dx, dy)
// (each -1 or 1) points out of the panel through that corner.
static void ui_corner(UI_Theme *t, F32 cx, F32 cy, F32 size, F32 dx, F32 dy);
// A soft glow of the theme's aura over the top of r (radius: r's corners; nothing when it has none).
static void ui_aura(Rect r, F32 radius, F32 strength);
// A cut gem (a diamond with a lit facet) centered on (cx, cy), `size` tall.
static void ui_gem(F32 cx, F32 cy, F32 size, V4 color);
// A cog wheel centered on (cx, cy), r its outer radius, turned by angle (radians).
static void ui_cog(F32 cx, F32 cy, F32 r, F32 angle, V4 color);
// "Working on it", in the theme's style (a turning cog, a flickering flame...), r its radius.
// Returns 1 (keeps frames coming).
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
static B32  ui_checkbox(String8 name, F32 x, F32 center_y, B32 checked);          // 16px; true when clicked
// Draws text with the bytes at `marks` (sorted offsets, e.g. fuzzy-match positions) in `hi`.
static F32  ui_text_highlighted(F_Font *f, F32 x, F32 y, String8 text, U8 *marks, U32 mark_count, V4 base, V4 hi, F32 max_w);
// A thin bar sliding along r's top edge, for "loading". Returns 1 (keeps frames coming).
static B32  ui_loading_bar(Rect r);
// Word-wrapped paragraphs ('\n' ends a line; "• " lines hang-indent). Draws only the lines inside
// [vis_y0, vis_y1); returns the total height.
static F32  ui_text_wrapped(F_Font *f, F32 x, F32 y, F32 w, String8 text, V4 color, F32 vis_y0, F32 vis_y1);
// Pictures from URLs: see ui_image.c (ui_image(), ui_image_fetch). ui_avatar there draws a picture,
// or until (unless) it loads, the name's initial on a colored square.

// A handle to drag (e.g. a divider between panes): .dragging while it's being dragged, with the mouse
// at ui.in.mx / my, .released when it's let go. Shows the left-right resize pointer while hovered or dragged.
static UI_Signal ui_drag_handle(UI_Id id, Rect zone);

// A tooltip for what's under the mouse: call it while `anchor` is hovered. It's drawn after the
// frame, over everything, below the anchor (above it near the window's bottom).
static void ui_tooltip(Rect anchor, String8 text);

// Smooth wheel scrolling for a list of content_h pixels shown in `list`, and its scrollbar along the
// right edge: drag the thumb, or press beside it to jump there. Call ui_scroll_list before drawing
// the rows (it takes scrollbar clicks before they can) and ui_scrollbar after. Returns 1 while animating.
typedef struct UI_Scroll { F32 pos, target; } UI_Scroll; // its scrollbar's drag is the active widget ui_id_ptr(sc)
static B32  ui_scroll_list(UI_Scroll *sc, Rect list, F32 content_h);
static void ui_scrollbar(UI_Scroll *sc, Rect list, F32 content_h);
// Scrolls (smoothly) just enough that [top, bottom) of the content is inside a view view_h tall.
static void ui_scroll_reveal(UI_Scroll *sc, F32 top, F32 bottom, F32 view_h);

// A list of `count` rows, each row_h tall, in r, scrolled by sc, with extra_h more content below
// them (e.g. a "Loading more..." row). ui_list_begin scrolls (ui_scroll_list), clips to r and says
// which rows are visible: draw rows first..last-1, at ui_list_row(&l, i, key). ui_list_end draws the
// scrollbar. l.animating is 1 while it scrolls.
typedef struct UI_List { UI_Scroll *sc; Rect r; F32 row_h, content_h; S64 first, last; B32 animating, scoped; } UI_List;
static UI_List ui_list_begin(UI_Scroll *sc, Rect r, S64 count, F32 row_h, F32 extra_h);
// Where row i is (whole pixels). It also starts the row's id scope, named key (what stays the same
// when rows move, e.g. an addon id; empty: i), until the next row or ui_list_end.
static Rect ui_list_row(UI_List *l, S64 i, String8 key);
static void ui_list_end(UI_List *l);

//- More widgets

// A plate button in the caption font (smaller than ui_button, e.g. in a status bar); true when clicked.
static B32  ui_small_button(Rect r, String8 label, V4 bg, V4 bg_hot);
static F32  ui_small_button_width(String8 label);
// A translucent white pill or circle (fully rounded) with a label or symbol, for use over a dark
// backdrop such as a picture viewer; true when clicked.
static B32  ui_glass_button(Rect r, String8 label, F_Font *f);
// A tab: plate when active, a hover fill otherwise, with an optional badge after the label (a count
// on the theme's ok color). True when clicked.
static B32  ui_tab(Rect r, String8 label, String8 badge, B32 active);
static F32  ui_tab_width(String8 label, String8 badge);
// A fully rounded tab (e.g. in a title bar): accent plate when active. True when an inactive one is clicked.
static B32  ui_pill_tab(Rect r, String8 label, B32 active);
static F32  ui_pill_tab_width(String8 label);
// Text that's a link: underlined, with a hand pointer, while hovered (inside ui.clip). Draws it at
// (x, y), at most max_w wide; returns the x after it. *hit gets the area that takes the click (a
// little bigger than the text): check ui_clicked(*hit).
static F32  ui_link_text(F_Font *f, F32 x, F32 y, String8 text, V4 color, F32 max_w, Rect *hit);
// "Installing..." and the like: a turning cog and text, ending at `right`, centered on cy.
// Returns 1 (keeps frames coming).
static B32  ui_busy_label(F32 right, F32 cy, String8 text);

//- Popups: a panel in the middle of the window over a dimmed backdrop
//
// Between ui_modal_begin and ui_modal_end everything draws in the overlay, clipped to the panel, and
// nothing under it sees the mouse (set the overlay to the whole window before drawing what's under
// it: ui_overlay_set). The panel is w x h at most (smaller in a small window). *fresh: it opened this
// frame, by a click the popup mustn't see (it's cleared). ui_modal_end returns 1 when a click beside
// the panel (pressed and released there) should close it.
static Rect ui_modal_begin(F32 w, F32 h, B32 *fresh);
static B32  ui_modal_close_button(Rect panel);    // the ✕ in its top-right corner; true when clicked
static B32  ui_modal_end(Rect panel);

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
