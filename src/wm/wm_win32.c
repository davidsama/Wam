// wm_win32.c - the window manager layer on Win32: the window (creation, DPI, the app-drawn title
// bar), mouse and key input, frame scheduling, posts from other threads, timers, deferred work,
// popup menus and the folder picker. See wm.h.
//
// Needs <windows.h>, <windowsx.h>, <shellapi.h>, <shobjidl.h> (with COBJMACROS), <dwmapi.h>,
// and links user32, shell32, ole32, uuid, dwmapi.

#define WM_DEFER_MAX 8
#define WM_MSG_POST  (WM_APP + 0x100) // lParam: WM_Posted (heap)

static WM_App *wm_app;
static HWND wm_hwnd;
static HICON wm_icon_big, wm_icon_small; // the app's icon (WM_App.icon), also for its other windows
static F32 wm_dpi_scale = 1;
static B32 wm_sizing;                    // between WM_ENTERSIZEMOVE and WM_EXITSIZEMOVE (an edge is being dragged)
static char wm_frame_cause = '?';        // what asked for the frame being drawn (profiling)
static F32 wm_last_w, wm_last_h;         // the size the last frame was drawn at

static B32 wm_frame_wanted = 1;          // someone asked for a frame since the last one (not just Windows repainting)
static void wm_request_frame(void) { wm_frame_wanted = 1; if (wm_hwnd) InvalidateRect(wm_hwnd, 0, FALSE); }

static void *wm_window_handle(void) { return wm_hwnd; }
static F32 wm_scale(void) { return wm_dpi_scale; }

//- Input
//
// The mouse is state (WM_Input); keys and text are events, queued here until the next frame
// takes them (one arriving while a frame runs waits for the next instead of being lost).

static WM_Input wm_in;
static B32 wm_mouse_tracking;            // WM_MOUSELEAVE requested
static WM_Event wm_queued[WM_EVENT_MAX];
static U32 wm_queued_count;
static U16 wm_high_surrogate;            // the first half of a character outside the BMP (WM_CHAR sends two)

static void wm_queue_event(WM_Event e)
{
    if (wm_queued_count < WM_EVENT_MAX) wm_queued[wm_queued_count++] = e;
    wm_request_frame();
}

static WM_Key wm_key_from_vk(WPARAM vk)
{
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) return (WM_Key)vk;
    if (vk >= VK_F1 && vk <= VK_F12) return (WM_Key)(WM_KEY_F1 + (vk - VK_F1));
    switch (vk) {
        case VK_BACK: return WM_KEY_BACKSPACE;
        case VK_TAB: return WM_KEY_TAB;
        case VK_RETURN: return WM_KEY_RETURN;
        case VK_ESCAPE: return WM_KEY_ESCAPE;
        case VK_SPACE: return WM_KEY_SPACE;
        case VK_DELETE: return WM_KEY_DELETE;
        case VK_LEFT: return WM_KEY_LEFT;
        case VK_RIGHT: return WM_KEY_RIGHT;
        case VK_UP: return WM_KEY_UP;
        case VK_DOWN: return WM_KEY_DOWN;
        case VK_PRIOR: return WM_KEY_PAGE_UP;
        case VK_NEXT: return WM_KEY_PAGE_DOWN;
        case VK_HOME: return WM_KEY_HOME;
        case VK_END: return WM_KEY_END;
        case VK_OEM_COMMA: return WM_KEY_COMMA;
    }
    return WM_KEY_NONE;
}

static U32 wm_mods_now(void)
{
    return (GetKeyState(VK_CONTROL) < 0 ? WM_MOD_CTRL : 0) | (GetKeyState(VK_SHIFT) < 0 ? WM_MOD_SHIFT : 0) |
           (GetKeyState(VK_MENU) < 0 ? WM_MOD_ALT : 0);
}

// One WM_CHAR code: printable characters become text (surrogate pairs joined). The control codes
// it also sends (Enter, Backspace, Esc, Ctrl+letter) are already key presses.
static void wm_queue_char(U32 c)
{
    if (c >= 0xD800 && c <= 0xDBFF) { wm_high_surrogate = (U16)c; return; }
    if (c >= 0xDC00 && c <= 0xDFFF) {
        if (!wm_high_surrogate) return;
        c = 0x10000 + ((U32)(wm_high_surrogate - 0xD800) << 10) + (c - 0xDC00);
    }
    wm_high_surrogate = 0;
    if (c < 0x20 || c == 0x7F) return;
    WM_Event e = { WM_EVENT_TEXT, WM_KEY_NONE, wm_mods_now(), c };
    wm_queue_event(e);
}

//- The mouse pointer

static int wm_cursor;

static void wm_apply_cursor(void)
{
    WCHAR *id = wm_cursor == WM_CURSOR_SIZE_WE ? MAKEINTRESOURCEW(32644) /* IDC_SIZEWE */
              : wm_cursor == WM_CURSOR_HAND ? MAKEINTRESOURCEW(32649) /* IDC_HAND */ : MAKEINTRESOURCEW(32512); /* IDC_ARROW */
    SetCursor(LoadCursorW(0, id));
}

static void wm_set_cursor(int cursor)
{
    if (cursor == wm_cursor) return;
    wm_cursor = cursor;
    if (wm_in.inside) wm_apply_cursor();
}

//- The caption: the app's own title bar
//
// The window keeps its WS_OVERLAPPEDWINDOW frame (so Windows still draws the shadow, the border and
// rounded corners, and does snapping and the animations) but its top part is client area:
// WM_NCCALCSIZE takes the title bar away, and WM_NCHITTEST tells Windows which parts of the app's
// own bar drag the window (HTCAPTION), which are its widgets (HTCLIENT), and where maximize is
// (HTMAXBUTTON, so Windows 11 shows its snap layouts there). The left, right and bottom resize
// borders stay Windows' own; the top one is the first few pixels of the client area.

#define WM_CAPTION_HOLES 32
typedef struct WM_Caption {
    F32 h;                          // 0: no caption
    Rect holes[WM_CAPTION_HOLES];   // the app's widgets in it
    U32 hole_count;
    Rect max_btn;
    Rect overlay;                   // a popup over the window takes the mouse, caption or not
    B32 overlay_on;
} WM_Caption;
static WM_Caption wm_cap_next, wm_cap; // being described by this frame / described by the last one (for hit tests)
static B32 wm_max_hot, wm_max_down;    // the mouse is on / pressed maximize (it gets non-client messages)
static B32 wm_active = 1;              // the window is the active one

static void wm_caption(F32 h) { wm_cap_next.h = h; }
static void wm_caption_hole(Rect r) { if (wm_cap_next.hole_count < WM_CAPTION_HOLES) wm_cap_next.holes[wm_cap_next.hole_count++] = r; }
static void wm_caption_overlay(Rect r, B32 on) { wm_cap_next.overlay = r; wm_cap_next.overlay_on = on; }
static void wm_caption_maximize(Rect r, B32 *hot, B32 *down) { wm_cap_next.max_btn = r; *hot = wm_max_hot; *down = wm_max_down; }
static B32 wm_window_is_active(void) { return wm_active; }
static B32 wm_window_is_maximized(void) { return wm_hwnd && IsZoomed(wm_hwnd); }
static void wm_window_minimize(void) { PostMessageW(wm_hwnd, WM_SYSCOMMAND, SC_MINIMIZE, 0); }
static void wm_window_toggle_maximized(void) { PostMessageW(wm_hwnd, WM_SYSCOMMAND, IsZoomed(wm_hwnd) ? SC_RESTORE : SC_MAXIMIZE, 0); }
static void wm_window_close(void) { PostMessageW(wm_hwnd, WM_SYSCOMMAND, SC_CLOSE, 0); }

// The width of the resize border Windows would draw (also how far a maximized window hangs off screen).
static int wm_frame_thickness(HWND hwnd)
{
    UINT dpi = GetDpiForWindow(hwnd);
    return GetSystemMetricsForDpi(SM_CYFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
}

// Non-client messages for the caption; returns 1 with *result set when it handled msg.
static B32 wm_caption_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT *result)
{
    switch (msg) {
        case WM_NCCALCSIZE: {
            if (!wp) return 0;
            // Windows' borders on the left, right and bottom; none on top. Maximized, the window
            // reaches past the screen's edges by the border's width, so the top moves down by that.
            NCCALCSIZE_PARAMS *p = (NCCALCSIZE_PARAMS *)lp;
            LONG top = p->rgrc[0].top;
            *result = DefWindowProcW(hwnd, msg, wp, lp);
            p->rgrc[0].top = top + (IsZoomed(hwnd) ? wm_frame_thickness(hwnd) : 0);
            return 1;
        }
        case WM_NCHITTEST: {
            LRESULT hit = DefWindowProcW(hwnd, msg, wp, lp);
            *result = hit;
            if (hit != HTCLIENT) return 1;
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ScreenToClient(hwnd, &pt);
            RECT cr;
            GetClientRect(hwnd, &cr);
            int t = wm_frame_thickness(hwnd);
            if (!IsZoomed(hwnd) && pt.y < t) {
                *result = pt.x < 2 * t ? HTTOPLEFT : pt.x >= cr.right - 2 * t ? HTTOPRIGHT : HTTOP;
                return 1;
            }
            F32 x = (F32)pt.x, y = (F32)pt.y;
            Rect ov = wm_cap.overlay;
            if (wm_cap.h <= 0 || y >= wm_cap.h || (wm_cap.overlay_on && x >= ov.x0 && x < ov.x1 && y >= ov.y0 && y < ov.y1)) return 1;
            Rect mb = wm_cap.max_btn;
            if (x >= mb.x0 && x < mb.x1 && y >= mb.y0 && y < mb.y1) { *result = HTMAXBUTTON; return 1; }
            for (U32 i = 0; i < wm_cap.hole_count; i += 1) {
                Rect r = wm_cap.holes[i];
                if (x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1) return 1;
            }
            *result = HTCAPTION;
            return 1;
        }
        // Maximize gets non-client mouse messages. Windows would draw its old-style button for them,
        // so they stop here: hover and press are drawn by the app (wm_caption_maximize), the click is a toggle.
        case WM_NCMOUSEMOVE: {
            B32 hot = wp == HTMAXBUTTON;
            if (hot != wm_max_hot) { wm_max_hot = hot; if (!hot) wm_max_down = 0; wm_request_frame(); }
            if (!hot) return 0;
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE | TME_NONCLIENT, hwnd, 0 };
            TrackMouseEvent(&tme);
            *result = 0;
            return 1;
        }
        case WM_NCMOUSELEAVE:
            if (wm_max_hot || wm_max_down) { wm_max_hot = wm_max_down = 0; wm_request_frame(); }
            return 0;
        case WM_NCLBUTTONDOWN: case WM_NCLBUTTONDBLCLK:
            if (wp != HTMAXBUTTON) return 0;
            wm_max_down = 1;
            wm_request_frame();
            *result = 0;
            return 1;
        case WM_NCLBUTTONUP:
            if (wp != HTMAXBUTTON) return 0;
            if (wm_max_down) wm_window_toggle_maximized();
            wm_max_down = 0;
            wm_request_frame();
            *result = 0;
            return 1;
        case WM_NCACTIVATE:
            wm_active = wp != 0;
            wm_request_frame();
            *result = DefWindowProcW(hwnd, msg, wp, -1); // -1: without repainting the (old) title bar
            return 1;
    }
    return 0;
}

//- Deferred work, posts, timers

static void (*wm_deferred[WM_DEFER_MAX])(void);
static U32 wm_deferred_count;

static void wm_defer(void (*fn)(void))
{
    for (U32 i = 0; i < wm_deferred_count; i += 1) if (wm_deferred[i] == fn) return;
    if (wm_deferred_count < WM_DEFER_MAX) wm_deferred[wm_deferred_count++] = fn;
}

static void wm_run_deferred(void)
{
    // Copy first: a deferred call may defer more work (or run a nested message loop).
    void (*todo[WM_DEFER_MAX])(void);
    U32 n = wm_deferred_count;
    memcpy(todo, wm_deferred, n * sizeof(todo[0]));
    wm_deferred_count = 0;
    for (U32 i = 0; i < n; i += 1) todo[i]();
    if (n) wm_request_frame();
}

typedef struct WM_Posted { WM_PostFunc *fn; U64 param; void *ptr; } WM_Posted;

static B32 wm_post(WM_PostFunc *fn, U64 param, void *ptr)
{
    WM_Posted *p = (WM_Posted *)os_alloc(sizeof(WM_Posted));
    if (!p) return 0;
    p->fn = fn; p->param = param; p->ptr = ptr;
    if (wm_hwnd && PostMessageW(wm_hwnd, WM_MSG_POST, 0, (LPARAM)p)) return 1;
    os_free(p);
    return 0;
}

// A timer's id is its function.
static void wm_timer_start(void (*fn)(void), U32 ms) { SetTimer(wm_hwnd, (UINT_PTR)fn, ms, 0); }
static void wm_timer_stop(void (*fn)(void)) { KillTimer(wm_hwnd, (UINT_PTR)fn); }

//- Frames

// One frame. Never runs inside another one: anything that pumps messages mid-frame (a menu, a
// dialog, ShellExecute) would deliver WM_PAINT and re-run the frame with the same click still
// pending. A paint that arrives meanwhile is simply redone afterwards.
static B32 wm_frame(void)
{
    static B32 in_frame;
    if (in_frame) return 1;
    in_frame = 1;
    wm_frame_wanted = 0;
    RECT cr;
    GetClientRect(wm_hwnd, &cr);
    WM_Frame f = { 0 };
    f.width = (F32)(cr.right - cr.left);
    f.height = (F32)(cr.bottom - cr.top);
    f.scale = wm_dpi_scale;
    f.in = wm_in;
    WM_Event events[WM_EVENT_MAX];
    memcpy(events, wm_queued, wm_queued_count * sizeof(WM_Event));
    f.events = events;
    f.event_count = wm_queued_count;
    wm_queued_count = 0;
    f.cause = wm_frame_cause;
    f.sizing = wm_sizing;
    memset(&wm_cap_next, 0, sizeof(wm_cap_next));
    B32 again = wm_app->frame(&f);
    wm_cap = wm_cap_next;
    wm_last_w = f.width;
    wm_last_h = f.height;
    wm_in.pressed = wm_in.released = 0;
    wm_in.wheel = 0;
    in_frame = 0;
    return again;
}

// Remembers the restored rectangle (not while maximized or minimized).
static void wm_place_track(HWND hwnd)
{
    RECT r;
    if (IsZoomed(hwnd) || IsIconic(hwnd) || !GetWindowRect(hwnd, &r)) return;
    wm_window_place.x = r.left;
    wm_window_place.y = r.top;
    wm_window_place.w = r.right - r.left;
    wm_window_place.h = r.bottom - r.top;
}

static LRESULT CALLBACK wm_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    LRESULT caption_result = 0;
    if (wm_caption_message(hwnd, msg, wp, lp, &caption_result)) return caption_result; // also while being created
    if (!wm_app || hwnd != wm_hwnd) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            // While an edge is dragged, Windows also asks for a repaint after every WM_SIZE, which has
            // already drawn that size: skip it unless something else wants a frame.
            wm_frame_cause = 'p';
            RECT cr;
            GetClientRect(hwnd, &cr);
            B32 redundant = wm_sizing && !wm_frame_wanted && wm_last_w == (F32)cr.right && wm_last_h == (F32)cr.bottom;
            if (!redundant && wm_frame()) wm_request_frame();
            wm_run_deferred();
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_SETCURSOR: if (LOWORD(lp) == HTCLIENT) { wm_apply_cursor(); return TRUE; } break;
        case WM_MSG_POST: {
            WM_Posted p = *(WM_Posted *)lp;
            os_free((void *)lp);
            p.fn(p.param, p.ptr);
            return 0;
        }
        case WM_TIMER: ((void (*)(void))wp)(); return 0;
        case WM_KEYDOWN: {
            WM_Event e = { WM_EVENT_KEY, wm_key_from_vk(wp), wm_mods_now(), 0 };
            if (e.key != WM_KEY_NONE) wm_queue_event(e);
            return 0;
        }
        case WM_CHAR: wm_queue_char((U32)wp); return 0;
        case WM_SIZE:
            // Draw the new size right away instead of waiting for WM_PAINT, which only comes once the
            // queue is empty: while the edge is being dragged sizes arrive faster than that, and
            // Windows would keep showing the old, smaller frame in the meantime.
            if (wp == SIZE_MAXIMIZED) wm_window_place.maximized = 1;
            if (wp == SIZE_RESTORED) { wm_window_place.maximized = 0; wm_place_track(hwnd); }
            wm_frame_cause = 's';
            if (LOWORD(lp) && HIWORD(lp) && wm_frame()) wm_request_frame();
            return 0;
        case WM_MOVE: wm_place_track(hwnd); return 0;
        case WM_ENTERSIZEMOVE: wm_sizing = 1; return 0;
        case WM_EXITSIZEMOVE: wm_sizing = 0; wm_request_frame(); return 0;
        case WM_GETMINMAXINFO: {
            MINMAXINFO *mm = (MINMAXINFO *)lp;
            mm->ptMinTrackSize.x = (LONG)(wm_app->min_width * wm_dpi_scale);
            mm->ptMinTrackSize.y = (LONG)(wm_app->min_height * wm_dpi_scale);
            return 0;
        }
        case WM_DPICHANGED: {
            wm_dpi_scale = (F32)HIWORD(wp) / 96.0f;
            RECT *r = (RECT *)lp;
            SetWindowPos(hwnd, 0, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            wm_request_frame();
            return 0;
        }
        case WM_MOUSEMOVE: {
            wm_in.mx = (F32)GET_X_LPARAM(lp);
            wm_in.my = (F32)GET_Y_LPARAM(lp);
            wm_in.inside = 1;
            if (!wm_mouse_tracking) {
                TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
                TrackMouseEvent(&tme);
                wm_mouse_tracking = 1;
            }
            wm_request_frame();
            return 0;
        }
        case WM_MOUSELEAVE: wm_in.inside = 0; wm_mouse_tracking = 0; wm_request_frame(); return 0;
        // Clicks get an immediate frame so a later WM_MOUSELEAVE can't swallow them.
        case WM_LBUTTONDOWN:
            wm_in.mx = wm_in.press_x = (F32)GET_X_LPARAM(lp);
            wm_in.my = wm_in.press_y = (F32)GET_Y_LPARAM(lp);
            wm_in.inside = 1;
            wm_in.down = wm_in.pressed = 1;
            SetCapture(hwnd);
            wm_frame_cause = 'm';
            if (wm_frame()) wm_request_frame();
            return 0;
        case WM_LBUTTONUP:
            wm_in.mx = (F32)GET_X_LPARAM(lp);
            wm_in.my = (F32)GET_Y_LPARAM(lp);
            wm_in.inside = 1;
            wm_in.down = 0;
            wm_in.released = 1;
            ReleaseCapture();
            wm_frame_cause = 'm';
            if (wm_frame()) wm_request_frame();
            wm_request_frame(); // redraw once more so hover reflects the post-click state
            wm_run_deferred();
            return 0;
        case WM_MOUSEWHEEL: wm_in.wheel += (F32)GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA; wm_request_frame(); return 0;
        case WM_DROPFILES: {
            HDROP drop = (HDROP)wp;
            UINT n = DragQueryFileW(drop, 0xFFFFFFFF, 0, 0);
            for (UINT i = 0; i < n && wm_app->file_dropped; i += 1) {
                WCHAR path[1024];
                UINT len = DragQueryFileW(drop, i, path, ArrayCount(path));
                if (!len || len >= ArrayCount(path)) continue;
                Temp t = scratch_begin(0);
                wm_app->file_dropped(os_str8_from_16(t.arena, (U16 *)path, len));
                scratch_end(t);
            }
            DragFinish(drop);
            wm_request_frame();
            return 0;
        }
        case WM_ACTIVATE:
            if (LOWORD(wp) != WA_INACTIVE && wm_app->activated) wm_app->activated();
            break;
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static B32 wm_window_open(WM_App *desc)
{
    wm_app = desc;
    HINSTANCE inst = GetModuleHandleW(0);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(0, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE); // the folder picker is COM

    Temp t = scratch_begin(0);
    WCHAR *class_name = (WCHAR *)os_str16_from_8(t.arena, str8_cstring(desc->class_name)).str;
    WCHAR *title = (WCHAR *)os_str16_from_8(t.arena, str8_cstring(desc->title)).str;
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = wm_wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(0, MAKEINTRESOURCEW(32512)); // IDC_ARROW
    if (desc->icon) {
        // The resource's sizes for the title bar / taskbar (small) and Alt+Tab (big).
        wm_icon_big = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(desc->icon), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
        wm_icon_small = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(desc->icon), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    }
    wc.hIcon = wm_icon_big ? wm_icon_big : LoadIconW(0, MAKEINTRESOURCEW(32512)); // IDI_APPLICATION
    wc.hIconSm = wm_icon_small;
    wc.lpszClassName = class_name;
    RegisterClassExW(&wc);

    F32 sys_scale = (F32)GetDpiForSystem() / 96.0f;
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT, w = (int)(desc->width * sys_scale), h = (int)(desc->height * sys_scale);
    // A saved place opens there, if its monitor is still around. Created right on it, so the window
    // starts out at that monitor's dpi instead of being rescaled by a WM_DPICHANGED.
    RECT saved = { desc->x, desc->y, desc->x + desc->w, desc->y + desc->h };
    B32 use_saved = desc->w > 0 && desc->h > 0 && MonitorFromRect(&saved, MONITOR_DEFAULTTONULL);
    if (use_saved) { x = desc->x; y = desc->y; w = desc->w; h = desc->h; }
    HWND hwnd = CreateWindowExW(0, /* not WS_EX_NOREDIRECTIONBITMAP: the bitblt swapchain presents into that surface (see r_init) */ class_name, title, WS_OVERLAPPEDWINDOW,
                                x, y, w, h, 0, 0, inst, 0);
    scratch_end(t);
    if (!hwnd) return 0;
    // While being created the window measured its frame the old way (WM_NCCALCSIZE without the
    // rects wm_caption_message reshapes): measure it again, now without the title bar.
    SetWindowPos(hwnd, 0, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    wm_hwnd = hwnd;
    wm_place_track(hwnd);
    wm_window_place.maximized = use_saved && desc->maximized;
    wm_dpi_scale = (F32)GetDpiForWindow(hwnd) / 96.0f;
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    if (desc->accept_files) {
        DragAcceptFiles(hwnd, TRUE);
        // Let Explorer drop files even if the app runs elevated.
        ChangeWindowMessageFilterEx(hwnd, WM_DROPFILES, MSGFLT_ALLOW, 0);
        ChangeWindowMessageFilterEx(hwnd, WM_COPYDATA, MSGFLT_ALLOW, 0);
        ChangeWindowMessageFilterEx(hwnd, 0x0049 /* WM_COPYGLOBALDATA */, MSGFLT_ALLOW, 0);
    }
    return 1;
}

static int wm_window_run(int show)
{
    // Reopen maximized if it was closed that way, unless the launch asked for something else (minimized).
    if (wm_window_place.maximized && (show == SW_SHOWNORMAL || show == SW_SHOWDEFAULT || show == SW_SHOW)) show = SW_SHOWMAXIMIZED;
    ShowWindow(wm_hwnd, show);
    MSG msg;
    while (GetMessageW(&msg, 0, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

//- Menus and dialogs

static S32 wm_popup_menu(Rect anchor, String8 *items, U32 count, S32 checked)
{
    Temp t = scratch_begin(0);
    HMENU menu = CreatePopupMenu();
    for (U32 i = 0; i < count; i += 1) {
        if (str8_match(items[i], str8_lit("-"), 0)) { AppendMenuW(menu, MF_SEPARATOR, 0, 0); continue; }
        UINT flags = MF_STRING | ((S32)i == checked ? MF_CHECKED : 0);
        AppendMenuW(menu, flags, i + 1, (WCHAR *)os_str16_from_8(t.arena, items[i]).str);
    }
    POINT pt = { (LONG)anchor.x0, (LONG)anchor.y1 + 2 };
    ClientToScreen(wm_hwnd, &pt);
    UINT cmd = (UINT)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON, pt.x, pt.y, 0, wm_hwnd, 0);
    DestroyMenu(menu);
    scratch_end(t);
    return cmd >= 1 && cmd <= count ? (S32)cmd - 1 : -1;
}

static S32 wm_popup_menu_tree(Rect anchor, WM_MenuItem *items, U32 count)
{
    Temp t = scratch_begin(0);
    HMENU root = CreatePopupMenu();
    HMENU *sub = push_array(t.arena, HMENU, count + 1);
    for (U32 i = 0; i < count; i += 1) {
        S32 p = items[i].parent;
        if (p >= 0 && (U32)p < count && !sub[p]) sub[p] = CreatePopupMenu();
    }
    for (U32 i = 0; i < count; i += 1) {
        S32 p = items[i].parent;
        HMENU into = p >= 0 && (U32)p < count ? sub[p] : root;
        if (str8_match(items[i].label, str8_lit("-"), 0)) { AppendMenuW(into, MF_SEPARATOR, 0, 0); continue; }
        WCHAR *label = (WCHAR *)os_str16_from_8(t.arena, items[i].label).str;
        UINT check = items[i].checked ? MF_CHECKED : 0;
        if (sub[i]) AppendMenuW(into, MF_POPUP | MF_STRING | check, (UINT_PTR)sub[i], label);
        else AppendMenuW(into, MF_STRING | check, i + 1, label);
    }
    POINT pt = { (LONG)anchor.x0, (LONG)anchor.y1 + 2 };
    ClientToScreen(wm_hwnd, &pt);
    UINT cmd = (UINT)TrackPopupMenu(root, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON, pt.x, pt.y, 0, wm_hwnd, 0);
    DestroyMenu(root); // and the submenus in it
    scratch_end(t);
    return cmd >= 1 && cmd <= count ? (S32)cmd - 1 : -1;
}

static String8 wm_pick_folder(Arena *a, String8 title)
{
    String8 r = { 0 };
    IFileOpenDialog *dlg = 0;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, 0, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&dlg))) return r;
    FILEOPENDIALOGOPTIONS opts = 0;
    IFileOpenDialog_GetOptions(dlg, &opts);
    IFileOpenDialog_SetOptions(dlg, opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    Temp t = scratch_begin(a);
    if (title.size) IFileOpenDialog_SetTitle(dlg, (WCHAR *)os_str16_from_8(t.arena, title).str);
    scratch_end(t);
    if (SUCCEEDED(IFileOpenDialog_Show(dlg, wm_hwnd))) {
        IShellItem *item = 0;
        if (SUCCEEDED(IFileOpenDialog_GetResult(dlg, &item))) {
            PWSTR p = 0;
            if (SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &p)) && p) {
                r = os_str8_from_16(a, (U16 *)p, wcslen(p));
                CoTaskMemFree(p);
            }
            IShellItem_Release(item);
        }
    }
    IFileOpenDialog_Release(dlg);
    return r;
}
