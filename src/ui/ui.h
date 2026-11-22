// ui.h - a small immediate-mode UI library on top of render/ (rounded rects, glyphs) and font/.
//
// Nothing here knows about the app using it. Each frame the app calls widgets; a widget draws
// itself with ui.theme / ui.font and returns what the user did with it (clicked, toggled...).
// Mouse state lives in ui.in and keys and typed text in ui.events, both filled by the window layer;
// the frame arena is cleared every frame.
//
//   ui.c        state, geometry, theme, fonts, ornaments, widgets, text editing
//   ui_image.c  pictures from URLs (icons, thumbnails), fetched and decoded in the background
//   ui_win32.c  the window: creation, DPI, input, frame scheduling, deferred work, and Windows
//               services widgets can't do mid-frame (shell open, clipboard, folder picker, menus)
//
// Everything is `static` (unity build): include ui.h after render/ and font/, then ui.c,
// ui_image.c and ui_win32.c.

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

typedef struct UI_Input {
    F32 mx, my;              // mouse, client pixels
    F32 press_x, press_y;    // where the last button press happened
    B32 inside, tracking;    // mouse over the window (tracking: WM_MOUSELEAVE requested)
    B32 down;                // left button held
    B32 pressed, released;   // left button went down / up this frame
    F32 wheel;               // wheel notches this frame (positive = away from the user)
} UI_Input;

// Keys and typed text since the last frame, in the order they came (ui.events). A key press and the
// text it types are two events: Space is a UI_KEY_SPACE press and a ' ' of text. Text events are
// printable characters only; Enter, Backspace, Esc and Ctrl+letter come as key presses.
typedef enum UI_Key {
    UI_KEY_NONE = 0,
    // '0'..'9' and 'A'..'Z' are themselves.
    UI_KEY_BACKSPACE = 0x100, UI_KEY_TAB, UI_KEY_RETURN, UI_KEY_ESCAPE, UI_KEY_SPACE, UI_KEY_DELETE,
    UI_KEY_LEFT, UI_KEY_RIGHT, UI_KEY_UP, UI_KEY_DOWN, UI_KEY_PAGE_UP, UI_KEY_PAGE_DOWN, UI_KEY_HOME, UI_KEY_END,
    UI_KEY_COMMA,
    UI_KEY_F1, UI_KEY_F2, UI_KEY_F3, UI_KEY_F4, UI_KEY_F5, UI_KEY_F6,
    UI_KEY_F7, UI_KEY_F8, UI_KEY_F9, UI_KEY_F10, UI_KEY_F11, UI_KEY_F12,
} UI_Key;

enum { UI_MOD_CTRL = 1, UI_MOD_SHIFT = 2, UI_MOD_ALT = 4 };

typedef enum UI_EventKind { UI_EVENT_KEY, UI_EVENT_TEXT } UI_EventKind;

typedef struct UI_Event {
    UI_EventKind kind;
    UI_Key key;              // UI_EVENT_KEY: what was pressed (also each auto-repeat)
    U32 mods;                // UI_MOD_*: held at the time
    U32 codepoint;           // UI_EVENT_TEXT: what was typed
} UI_Event;

#define UI_EVENT_MAX 64

typedef struct UI_State {
    F32 scale;               // DPI scale: 1.0 at 96 dpi
    UI_Theme theme;
    UI_Fonts font;
    Arena *font_arena;
    UI_Input in;
    UI_Event events[UI_EVENT_MAX]; // this frame's keys and text (more than that between two frames are dropped)
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

enum { UI_CURSOR_ARROW, UI_CURSOR_SIZE_WE, UI_CURSOR_HAND };

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
// Pictures from URLs: see ui_image.c (ui_image(), ui_image_fetch). Include it before ui_win32.c.

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
static B32  ui_text_edit(UI_TextBuf *t, UI_Event *e);
// A one-line field: text (or one dot per character when masked) with a caret, or the hint when empty.
static void ui_text_field(Rect r, UI_TextBuf *t, String8 hint, B32 masked, V4 border);

//- Window (ui_win32.c)

typedef struct UI_App {
    wchar_t *class_name, *title;
    int width, height;              // initial size, at 96 dpi
    int icon;                       // resource id of the window's icon (0: Windows' default)
    int min_width, min_height;      // at 96 dpi
    B32 accept_files;               // drag and drop from Explorer
    // Draws one frame between the renderer's begin/end. Return 1 to get another frame (animation).
    B32 (*frame)(void);
    // Everything the window layer doesn't handle itself: timers, WM_APP messages. (Keys and typed
    // text don't come here: they're in ui.events, for the next frame.)
    // Set *handled to stop the default processing.
    LRESULT (*message)(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, B32 *handled);
    void (*file_dropped)(String8 path);   // each dropped file
    void (*activated)(void);              // the window became the active one
    // Where the window opens (screen pixels; w = 0: Windows picks, at width x height). Ignored if
    // it's not on any monitor now. After the window closes, ui_window_place has where it was.
    S32 x, y, w, h;
    B32 maximized;
} UI_App;

static HWND ui_hwnd;
static B32  ui_window_open(HINSTANCE inst, UI_App *desc);   // creates the window, renderer and fonts
static int  ui_window_run(int show);                          // shows it and runs the message loop
// The window's last restored (not maximized/minimized) rectangle in screen pixels, and whether it
// was maximized. Kept up to date while it's open, still valid once it has closed.
typedef struct UI_WindowPlace { S32 x, y, w, h; B32 maximized; } UI_WindowPlace;
static UI_WindowPlace ui_window_place;
static void ui_request_frame(void);

// The window has no Windows title bar: the app draws its own across the top. Each frame it says how
// tall that bar is (ui_caption) and where in it the mouse belongs to its widgets (ui_caption_hole);
// the rest of the bar drags the window, double-click maximizes, right-click opens the window menu.
// ui_window_buttons draws minimize, maximize and close in the top-right corner, h tall, ending at
// `right`, and returns their left edge. Windows 11's snap layouts appear over maximize as usual.
static void ui_caption(F32 h);                 // 0: no caption this frame (e.g. something covers it)
static void ui_caption_hole(Rect r);
static F32  ui_window_buttons(F32 right, F32 h);
// Runs fn after the current frame. For anything that pumps messages (menus, dialogs): called
// inside a frame it would re-run that frame with the same click still pending.
static void ui_defer(void (*fn)(void));

// Opens a file, URL or program on a short-lived thread (ShellExecute pumps messages too).
static void ui_shell_open_async(String8 file, String8 args);
static void ui_open_url(String8 url);              // https:// only
static String8 ui_open_log;                        // testing: when set, opens are appended here instead

static String8 ui_clipboard_text(Arena *a);
// Native popup menu below `anchor`; an item "-" is a separator. Returns the chosen index or -1.
static S32 ui_popup_menu(Rect anchor, String8 *items, U32 count, S32 checked);
// The same with submenus: an item whose index other items name as parent opens a submenu of them
// (it can't be picked itself). parent -1 = the top menu; "-" = a separator. Returns the picked index.
typedef struct UI_MenuItem { String8 label; S32 parent; B32 checked; } UI_MenuItem;
static S32 ui_popup_menu_tree(Rect anchor, UI_MenuItem *items, U32 count);
// Windows' folder picker. Empty if cancelled.
static String8 ui_pick_folder(Arena *a, wchar_t *title);

#endif
