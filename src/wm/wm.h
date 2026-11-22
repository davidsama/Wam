// wm.h - the window manager layer: one top-level window that the app draws itself (title bar
// included), its keyboard and mouse input, when frames are drawn, and the native pieces that need
// the window (popup menus, the folder picker, the download window). Implemented in wm_win32.c
// (and wm_webview.c); nothing above this layer sees a window handle or a window message.
//
// Event-driven: a frame is drawn when input arrives or someone asks for one (wm_request_frame),
// and again only while the last one said it's animating. Idle = no CPU.
//
// Other threads talk to the UI thread with wm_post: the function runs on the UI thread, in order
// with everything else the window receives (and wakes an idle window).

#ifndef WM_H
#define WM_H

//- Input

// Keys and typed text since the last frame, in the order they came. A key press and the text it
// types are two events: Space is a WM_KEY_SPACE press and a ' ' of text. Text events are printable
// characters only; Enter, Backspace, Esc and Ctrl+letter come as key presses.
typedef enum WM_Key {
    WM_KEY_NONE = 0,
    // '0'..'9' and 'A'..'Z' are themselves.
    WM_KEY_BACKSPACE = 0x100, WM_KEY_TAB, WM_KEY_RETURN, WM_KEY_ESCAPE, WM_KEY_SPACE, WM_KEY_DELETE,
    WM_KEY_LEFT, WM_KEY_RIGHT, WM_KEY_UP, WM_KEY_DOWN, WM_KEY_PAGE_UP, WM_KEY_PAGE_DOWN, WM_KEY_HOME, WM_KEY_END,
    WM_KEY_COMMA,
    WM_KEY_F1, WM_KEY_F2, WM_KEY_F3, WM_KEY_F4, WM_KEY_F5, WM_KEY_F6,
    WM_KEY_F7, WM_KEY_F8, WM_KEY_F9, WM_KEY_F10, WM_KEY_F11, WM_KEY_F12,
} WM_Key;

enum { WM_MOD_CTRL = 1, WM_MOD_SHIFT = 2, WM_MOD_ALT = 4 };

typedef enum WM_EventKind { WM_EVENT_KEY, WM_EVENT_TEXT } WM_EventKind;

typedef struct WM_Event {
    WM_EventKind kind;
    WM_Key key;              // WM_EVENT_KEY: what was pressed (also each auto-repeat)
    U32 mods;                // WM_MOD_*: held at the time
    U32 codepoint;           // WM_EVENT_TEXT: what was typed
} WM_Event;

#define WM_EVENT_MAX 64      // more than that between two frames are dropped

// The mouse, in client pixels.
typedef struct WM_Input {
    F32 mx, my;              // where it is
    F32 press_x, press_y;    // where the last left-button press happened
    B32 inside;              // over the window
    B32 down;                // left button held
    B32 pressed, released;   // left button went down / up since the last frame
    F32 wheel;               // wheel notches since the last frame (positive = away from the user)
} WM_Input;

//- Frames

// What a frame gets: the client size, the DPI scale (1.0 at 96 dpi), and the input since the last one.
typedef struct WM_Frame {
    F32 width, height, scale;
    WM_Input in;
    WM_Event *events;
    U32 event_count;
    char cause;              // profiling: what asked for it (s = a resize, p = a repaint, m = the mouse)
    B32 sizing;              // profiling: an edge of the window is being dragged
} WM_Frame;

typedef struct WM_App {
    char *class_name, *title;       // UTF-8
    int width, height;              // initial size, at 96 dpi
    int icon;                       // resource id of the window's icon (0: Windows' default)
    int min_width, min_height;      // at 96 dpi
    B32 accept_files;               // drag and drop from Explorer
    // Draws one frame. Return 1 to get another one right after (animation).
    B32 (*frame)(WM_Frame *f);
    void (*file_dropped)(String8 path);   // each dropped file
    void (*activated)(void);              // the window became the active one
    // Where the window opens (screen pixels; w = 0: the system picks, at width x height). Ignored
    // if it's not on any monitor now. After the window closes, wm_window_place has where it was.
    S32 x, y, w, h;
    B32 maximized;
} WM_App;

static B32  wm_window_open(WM_App *desc);       // creates the window (not shown yet)
static int  wm_window_run(int show);            // shows it (show: the entry point's nCmdShow) and runs until it closes
static void *wm_window_handle(void);            // the native window, for the renderer
static F32  wm_scale(void);                     // its DPI scale now
static void wm_request_frame(void);

// The window's last restored (not maximized/minimized) rectangle in screen pixels, and whether it
// was maximized. Kept up to date while it's open, still valid once it has closed.
typedef struct WM_WindowPlace { S32 x, y, w, h; B32 maximized; } WM_WindowPlace;
static WM_WindowPlace wm_window_place;

// The mouse pointer, set by each frame.
enum { WM_CURSOR_ARROW, WM_CURSOR_SIZE_WE, WM_CURSOR_HAND };
static void wm_set_cursor(int cursor);

//- Talking to the UI thread

// Runs fn(param, ptr) on the UI thread, after what it's doing now. Callable from any thread.
// Returns 0 if it couldn't (the window is gone): ptr is then still the caller's.
typedef void WM_PostFunc(U64 param, void *ptr);
static B32 wm_post(WM_PostFunc *fn, U64 param, void *ptr);
// Runs fn on the UI thread in ms milliseconds (again: the timer restarts), until stopped.
// UI thread only. Timers only fire while the window has nothing to draw.
static void wm_timer_start(void (*fn)(void), U32 ms);
static void wm_timer_stop(void (*fn)(void));
// Runs fn after the current frame. For anything that runs its own message loop (menus, dialogs):
// inside a frame it would draw that frame again with the same click still pending.
static void wm_defer(void (*fn)(void));

//- The title bar
//
// The window has no system title bar: the app draws its own across the top. Each frame it says how
// tall that bar is (wm_caption) and where in it the mouse belongs to its widgets (wm_caption_hole);
// the rest of the bar drags the window, double-click maximizes, right-click opens the window menu.
// The app draws minimize, maximize and close itself; wm_caption_maximize says where maximize is
// (so Windows 11's snap layouts appear over it) and whether the mouse is on it or pressing it.

static void wm_caption(F32 h);                 // 0: no caption this frame (e.g. something covers it)
static void wm_caption_hole(Rect r);
static void wm_caption_overlay(Rect r, B32 on); // a popup over the window takes the mouse, caption or not
static void wm_caption_maximize(Rect r, B32 *hot, B32 *down);
static B32  wm_window_is_active(void);
static B32  wm_window_is_maximized(void);
static void wm_window_minimize(void);
static void wm_window_toggle_maximized(void);
static void wm_window_close(void);

//- Native pieces (UI thread; they run their own message loop: call them from wm_defer)

// A popup menu below `anchor` (client pixels); an item "-" is a separator. Returns the chosen
// index or -1.
static S32 wm_popup_menu(Rect anchor, String8 *items, U32 count, S32 checked);
// The same with submenus: an item whose index other items name as parent opens a submenu of them
// (it can't be picked itself). parent -1 = the top menu; "-" = a separator. Returns the picked index.
typedef struct WM_MenuItem { String8 label; S32 parent; B32 checked; } WM_MenuItem;
static S32 wm_popup_menu_tree(Rect anchor, WM_MenuItem *items, U32 count);
// The system's folder picker. Empty if cancelled.
static String8 wm_pick_folder(Arena *a, String8 title);

// wm_webview.c (optional): a window that shows one web page (WebView2) and hands back the first
// file the page downloads, saved into save_dir; then it closes itself. note (optional): a line or
// two shown above the page saying why it opened. done(user, path, ok, error) is called once on the
// UI thread. wm_web_available says whether the WebView2 runtime is installed.
typedef void WM_WebDone(void *user, String8 path, B32 ok, String8 error);
static String8 wm_web_data_dir;   // where WebView2 keeps its profile (cookies etc.); set before use
static B32 wm_web_available(void);
static B32 wm_web_busy(void);
static B32 wm_web_download(String8 url, String8 title, String8 note, String8 save_dir, WM_WebDone *done, void *user);

#endif
