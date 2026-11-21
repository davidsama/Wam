// ui_win32.c - the window for the UI library: creation, DPI, mouse input, frame scheduling,
// deferred work, and the Windows services that can't run inside a frame. See ui.h.
//
// Rendering is event-driven: a frame is drawn when input or the app asks for one
// (ui_request_frame) and again only while it says it's animating. Idle = no CPU.
//
// Needs <windows.h>, <windowsx.h>, <shellapi.h>, <shobjidl.h> (with COBJMACROS), <dwmapi.h>,
// and links user32, gdi32, shell32, ole32, uuid, dwmapi.

#define UI_ATLAS_SIZE 2048 // the glyph atlas (R8)
#define UI_DEFER_MAX  8

static UI_App *ui_app;
static void (*ui_deferred[UI_DEFER_MAX])(void);
static U32 ui_deferred_count;

// Testing: when ui_frame_log is set, each frame appends "<microseconds> <width> <height>" there.
static String8 ui_frame_log;
static HICON ui_icon_big, ui_icon_small; // the app's icon (UI_App.icon), also for its other windows
// Testing: when ui_shot_dir is set, F12 saves the next frame there as shot-N.bmp (r_save_shot).
static String8 ui_shot_dir;
static U32 ui_shot_count;
static HANDLE ui_frame_log_file;
static char ui_frame_cause = '?'; // what asked for the frame being drawn (profiling): s=WM_SIZE p=WM_PAINT m=mouse
static B32 ui_sizing;   // between WM_ENTERSIZEMOVE and WM_EXITSIZEMOVE (an edge is being dragged)

static B32 ui_frame_wanted = 1;   // someone asked for a frame since the last one (not just Windows repainting)
static void ui_request_frame(void) { ui_frame_wanted = 1; if (ui_hwnd) InvalidateRect(ui_hwnd, 0, FALSE); }

//- The caption: the app's own title bar
//
// The window keeps its WS_OVERLAPPEDWINDOW frame (so Windows still draws the shadow, the border and
// rounded corners, and does snapping and the animations) but its top part is client area:
// WM_NCCALCSIZE takes the title bar away, and WM_NCHITTEST tells Windows which parts of the app's
// own bar drag the window (HTCAPTION), which are its widgets (HTCLIENT), and where maximize is
// (HTMAXBUTTON, so Windows 11 shows its snap layouts there). The left, right and bottom resize
// borders stay Windows' own; the top one is the first few pixels of the client area.

#define UI_CAPTION_HOLES 32
typedef struct UI_Caption {
    F32 h;                          // 0: no caption
    Rect holes[UI_CAPTION_HOLES];   // the app's widgets in it
    U32 hole_count;
    Rect max_btn;
    Rect overlay;                   // a popup over the window takes the mouse, caption or not
    B32 overlay_on;
} UI_Caption;
static UI_Caption ui_cap_next, ui_cap; // being described by this frame / described by the last one (for hit tests)
static B32 ui_max_hot, ui_max_down;    // the mouse is on / pressed maximize (it gets non-client messages)
static B32 ui_active = 1;              // the window is the active one (the buttons dim when not)

static void ui_caption(F32 h) { ui_cap_next.h = h; }
static void ui_caption_hole(Rect r) { if (ui_cap_next.hole_count < UI_CAPTION_HOLES) ui_cap_next.holes[ui_cap_next.hole_count++] = r; }

// The width of the resize border Windows would draw (also how far a maximized window hangs off screen).
static int ui_frame_thickness(HWND hwnd)
{
    UINT dpi = GetDpiForWindow(hwnd);
    return GetSystemMetricsForDpi(SM_CYFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
}

static void ui_toggle_maximized(void) { PostMessageW(ui_hwnd, WM_SYSCOMMAND, IsZoomed(ui_hwnd) ? SC_RESTORE : SC_MAXIMIZE, 0); }

static F32 ui_window_buttons(F32 right, F32 h)
{
    F32 bw = 46 * ui.scale, x = right - 3 * bw;
    // Segoe Fluent Icons / MDL2: ChromeMinimize, ChromeMaximize or ChromeRestore, ChromeClose.
    char *glyph[3] = { "\xEE\xA4\xA1", IsZoomed(ui_hwnd) ? "\xEE\xA4\xA3" : "\xEE\xA4\xA2", "\xEE\xA2\xBB" };
    for (int i = 0; i < 3; i += 1) {
        Rect b = rect(x + i * bw, 0, x + (i + 1) * bw, h);
        B32 hot = ui_mouse_in(b), down = hot && ui.in.down && point_in(b, ui.in.press_x, ui.in.press_y);
        if (i == 1) { ui_cap_next.max_btn = b; hot |= ui_max_hot; down |= ui_max_down; }
        else ui_caption_hole(b);
        V4 fg = ui_active || hot ? ui.theme.text : ui.theme.faint;
        if (hot && i == 2) { r_rect(b, ui_hex(0xc42b1c, down ? 0.85f : 1), 0, 0, 0); fg = ui_hex(0xffffff, 1); }
        else if (hot) r_rect(b, ui_hex(0xffffff, down ? 0.05f : 0.09f), 0, 0, 0);
        ui_text_centered(ui.font.glyphs, b, str8_cstring(glyph[i]), fg);
        if (ui_clicked(b)) {
            if (i == 1) ui_toggle_maximized(); // (only when the caption is off: else maximize isn't client area)
            else PostMessageW(ui_hwnd, WM_SYSCOMMAND, i == 0 ? SC_MINIMIZE : SC_CLOSE, 0);
        }
    }
    return x;
}

// Non-client messages for the caption; returns 1 with *result set when it handled msg.
static B32 ui_caption_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT *result)
{
    switch (msg) {
        case WM_NCCALCSIZE: {
            if (!wp) return 0;
            // Windows' borders on the left, right and bottom; none on top. Maximized, the window
            // reaches past the screen's edges by the border's width, so the top moves down by that.
            NCCALCSIZE_PARAMS *p = (NCCALCSIZE_PARAMS *)lp;
            LONG top = p->rgrc[0].top;
            *result = DefWindowProcW(hwnd, msg, wp, lp);
            p->rgrc[0].top = top + (IsZoomed(hwnd) ? ui_frame_thickness(hwnd) : 0);
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
            int t = ui_frame_thickness(hwnd);
            if (!IsZoomed(hwnd) && pt.y < t) {
                *result = pt.x < 2 * t ? HTTOPLEFT : pt.x >= cr.right - 2 * t ? HTTOPRIGHT : HTTOP;
                return 1;
            }
            F32 x = (F32)pt.x, y = (F32)pt.y;
            if (ui_cap.h <= 0 || y >= ui_cap.h || (ui_cap.overlay_on && point_in(ui_cap.overlay, x, y))) return 1;
            if (point_in(ui_cap.max_btn, x, y)) { *result = HTMAXBUTTON; return 1; }
            for (U32 i = 0; i < ui_cap.hole_count; i += 1) if (point_in(ui_cap.holes[i], x, y)) return 1;
            *result = HTCAPTION;
            return 1;
        }
        // Maximize gets non-client mouse messages. Windows would draw its old-style button for them,
        // so they stop here: hover and press are drawn by ui_window_buttons, the click is a toggle.
        case WM_NCMOUSEMOVE: {
            B32 hot = wp == HTMAXBUTTON;
            if (hot != ui_max_hot) { ui_max_hot = hot; if (!hot) ui_max_down = 0; ui_request_frame(); }
            if (!hot) return 0;
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE | TME_NONCLIENT, hwnd, 0 };
            TrackMouseEvent(&tme);
            *result = 0;
            return 1;
        }
        case WM_NCMOUSELEAVE:
            if (ui_max_hot || ui_max_down) { ui_max_hot = ui_max_down = 0; ui_request_frame(); }
            return 0;
        case WM_NCLBUTTONDOWN: case WM_NCLBUTTONDBLCLK:
            if (wp != HTMAXBUTTON) return 0;
            ui_max_down = 1;
            ui_request_frame();
            *result = 0;
            return 1;
        case WM_NCLBUTTONUP:
            if (wp != HTMAXBUTTON) return 0;
            if (ui_max_down) ui_toggle_maximized();
            ui_max_down = 0;
            ui_request_frame();
            *result = 0;
            return 1;
        case WM_NCACTIVATE:
            ui_active = wp != 0;
            ui_request_frame();
            *result = DefWindowProcW(hwnd, msg, wp, -1); // -1: without repainting the (old) title bar
            return 1;
    }
    return 0;
}

static void ui_defer(void (*fn)(void))
{
    for (U32 i = 0; i < ui_deferred_count; i += 1) if (ui_deferred[i] == fn) return;
    if (ui_deferred_count < UI_DEFER_MAX) ui_deferred[ui_deferred_count++] = fn;
}

static void ui_run_deferred(void)
{
    // Copy first: a deferred call may defer more work (or run a nested message loop).
    void (*todo[UI_DEFER_MAX])(void);
    U32 n = ui_deferred_count;
    memcpy(todo, ui_deferred, n * sizeof(todo[0]));
    ui_deferred_count = 0;
    for (U32 i = 0; i < n; i += 1) todo[i]();
    if (n) ui_request_frame();
}

static void ui_apply_cursor(void)
{
    WCHAR *id = ui.cursor == UI_CURSOR_SIZE_WE ? MAKEINTRESOURCEW(32644) /* IDC_SIZEWE */
              : ui.cursor == UI_CURSOR_HAND ? MAKEINTRESOURCEW(32649) /* IDC_HAND */ : MAKEINTRESOURCEW(32512); /* IDC_ARROW */
    SetCursor(LoadCursorW(0, id));
}

// One frame. Never runs inside another one: anything that pumps messages mid-frame (a menu, a
// dialog, ShellExecute) would deliver WM_PAINT and re-run the frame with the same click still
// pending. A paint that arrives meanwhile is simply redone afterwards.
static B32 ui_frame(void)
{
    static B32 in_frame;
    if (in_frame) return 1;
    in_frame = 1;
    U64 t0 = os_now_us();
    ui_frame_wanted = 0;
    arena_clear(ui.frame_arena);
    ui.width = (F32)r_state->width;
    ui.height = (F32)r_state->height;
    ui.clip = rect(0, 0, ui.width, ui.height);
    ui.overlay_on = ui.in_overlay = 0;
    ui.block_count = 0;
    if (f_begin_frame()) ui_frame_wanted = 1; // the glyph atlas was full: emptied, so draw again
    int cursor_before = ui.cursor;
    ui.cursor = UI_CURSOR_ARROW;
    r_begin_frame();
    memset(&ui_cap_next, 0, sizeof(ui_cap_next));
    ui.tip = str8(0, 0);
    B32 again = ui_app->frame();
    ui_tooltip_draw();
    ui_cap_next.overlay = ui.overlay;
    ui_cap_next.overlay_on = ui.overlay_on;
    ui_cap = ui_cap_next;
    if (ui.cursor != cursor_before && ui.in.inside) ui_apply_cursor();
    ui.frame_ms = (F64)(os_now_us() - t0) / 1000.0;
    {
        // Frames in the last second, from a ring of recent frame times.
        static U64 times[512];
        static U32 head;
        U64 now = os_now_us();
        times[head++ % ArrayCount(times)] = now;
        U32 n = 0;
        for (U32 k = 0; k < ArrayCount(times); k += 1) n += times[k] && now - times[k] < 1000000;
        ui.fps = n;
    }
    r_end_frame(ui.theme.bg);
    if (ui_frame_log.size) {
        if (!ui_frame_log_file) ui_frame_log_file = CreateFileW((WCHAR *)os_str16_from_8(ui.frame_arena, ui_frame_log).str, FILE_APPEND_DATA, FILE_SHARE_READ, 0, OPEN_ALWAYS, 0, 0);
        char line[160];
        int n = sprintf(line, "%llu %d %d resize %llu cpu %d present %llu by %c sizing %d\r\n", os_now_us(), (int)ui.width, (int)ui.height,
                        r_state->us_resize, (int)(ui.frame_ms * 1000), r_state->us_present, ui_frame_cause, ui_sizing);
        r_state->us_resize = 0;
        DWORD wrote;
        if (ui_frame_log_file != INVALID_HANDLE_VALUE) WriteFile(ui_frame_log_file, line, (DWORD)n, &wrote, 0);
    }
    ui.in.pressed = ui.in.released = 0;
    ui.in.wheel = 0;
    in_frame = 0;
    return again;
}

// Remembers the restored rectangle (not while maximized or minimized).
static void ui_place_track(HWND hwnd)
{
    RECT r;
    if (IsZoomed(hwnd) || IsIconic(hwnd) || !GetWindowRect(hwnd, &r)) return;
    ui_window_place.x = r.left;
    ui_window_place.y = r.top;
    ui_window_place.w = r.right - r.left;
    ui_window_place.h = r.bottom - r.top;
}

static LRESULT CALLBACK ui_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    LRESULT caption_result = 0;
    if (ui_caption_message(hwnd, msg, wp, lp, &caption_result)) return caption_result; // also while being created
    if (!r_state || !ui_app) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            // While an edge is dragged, Windows also asks for a repaint after every WM_SIZE, which has
            // already drawn that size: skip it unless something else wants a frame.
            ui_frame_cause = 'p';
            B32 redundant = ui_sizing && !ui_frame_wanted && ui.width == (F32)r_state->width && ui.height == (F32)r_state->height;
            if (!redundant && ui_frame()) ui_request_frame();
            ui_run_deferred();
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_SETCURSOR: if (LOWORD(lp) == HTCLIENT) { ui_apply_cursor(); return TRUE; } break;
        case UI_WM_IMAGE: ui_image_arrived((UI_ImageResult *)lp); return 0;
        case WM_KEYDOWN:
            if (wp == VK_F12 && ui_shot_dir.size) {
                Temp t = temp_begin(ui.frame_arena);
                String16 p16 = os_str16_from_8(t.arena, str8_pushf(t.arena, "%.*s\\shot-%u.bmp", S8F(ui_shot_dir), ui_shot_count++));
                if (p16.size < ArrayCount(r_shot_path)) memcpy(r_shot_path, p16.str, (p16.size + 1) * sizeof(WCHAR));
                temp_end(t);
                ui_request_frame();
                return 0;
            }
            break;
        case WM_SIZE:
            // Draw the new size right away instead of waiting for WM_PAINT, which only comes once the
            // queue is empty: while the edge is being dragged sizes arrive faster than that, and
            // Windows would keep showing the old, smaller frame in the meantime.
            r_resize(LOWORD(lp), HIWORD(lp));
            if (wp == SIZE_MAXIMIZED) ui_window_place.maximized = 1;
            if (wp == SIZE_RESTORED) { ui_window_place.maximized = 0; ui_place_track(hwnd); }
            ui_frame_cause = 's';
            if (LOWORD(lp) && HIWORD(lp) && ui_frame()) ui_request_frame();
            return 0;
        case WM_MOVE: ui_place_track(hwnd); return 0;
        case WM_ENTERSIZEMOVE: ui_sizing = 1; return 0;
        case WM_EXITSIZEMOVE: ui_sizing = 0; ui_request_frame(); return 0;
        case WM_GETMINMAXINFO: {
            MINMAXINFO *mm = (MINMAXINFO *)lp;
            mm->ptMinTrackSize.x = (LONG)(ui_app->min_width * ui.scale);
            mm->ptMinTrackSize.y = (LONG)(ui_app->min_height * ui.scale);
            return 0;
        }
        case WM_DPICHANGED: {
            ui.scale = (F32)HIWORD(wp) / 96.0f;
            RECT *r = (RECT *)lp;
            SetWindowPos(hwnd, 0, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            ui_fonts_load();
            ui_request_frame();
            return 0;
        }
        case WM_MOUSEMOVE: {
            ui.in.mx = (F32)GET_X_LPARAM(lp);
            ui.in.my = (F32)GET_Y_LPARAM(lp);
            ui.in.inside = 1;
            if (!ui.in.tracking) {
                TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
                TrackMouseEvent(&tme);
                ui.in.tracking = 1;
            }
            ui_request_frame();
            return 0;
        }
        case WM_MOUSELEAVE: ui.in.inside = 0; ui.in.tracking = 0; ui_request_frame(); return 0;
        // Clicks get an immediate frame so a later WM_MOUSELEAVE can't swallow them.
        case WM_LBUTTONDOWN:
            ui.in.mx = ui.in.press_x = (F32)GET_X_LPARAM(lp);
            ui.in.my = ui.in.press_y = (F32)GET_Y_LPARAM(lp);
            ui.in.inside = 1;
            ui.in.down = ui.in.pressed = 1;
            SetCapture(hwnd);
            ui_frame_cause = 'm';
            if (ui_frame()) ui_request_frame();
            return 0;
        case WM_LBUTTONUP:
            ui.in.mx = (F32)GET_X_LPARAM(lp);
            ui.in.my = (F32)GET_Y_LPARAM(lp);
            ui.in.inside = 1;
            ui.in.down = 0;
            ui.in.released = 1;
            ReleaseCapture();
            ui_frame_cause = 'm';
            if (ui_frame()) ui_request_frame();
            ui_request_frame(); // redraw once more so hover reflects the post-click state
            ui_run_deferred();
            return 0;
        case WM_MOUSEWHEEL: ui.in.wheel += (F32)GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA; ui_request_frame(); return 0;
        case WM_DROPFILES: {
            HDROP drop = (HDROP)wp;
            UINT n = DragQueryFileW(drop, 0xFFFFFFFF, 0, 0);
            for (UINT i = 0; i < n && ui_app->file_dropped; i += 1) {
                WCHAR path[1024];
                UINT len = DragQueryFileW(drop, i, path, ArrayCount(path));
                if (!len || len >= ArrayCount(path)) continue;
                Temp t = temp_begin(ui.frame_arena);
                ui_app->file_dropped(os_str8_from_16(t.arena, (U16 *)path, len));
                temp_end(t);
            }
            DragFinish(drop);
            ui_request_frame();
            return 0;
        }
        case WM_ACTIVATE:
            if (LOWORD(wp) != WA_INACTIVE && ui_app->activated) ui_app->activated();
            break;
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    if (ui_app->message) {
        B32 handled = 0;
        LRESULT r = ui_app->message(hwnd, msg, wp, lp, &handled);
        if (handled) return r;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static B32 ui_window_open(HINSTANCE inst, UI_App *desc)
{
    ui_app = desc;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(0, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE); // the folder picker is COM
    if (!ui.frame_arena) ui.frame_arena = arena_alloc(GB(1));
    ui.theme = ui_theme_dark();

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = ui_wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(0, MAKEINTRESOURCEW(32512)); // IDC_ARROW
    if (desc->icon) {
        // The resource's sizes for the title bar / taskbar (small) and Alt+Tab (big).
        ui_icon_big = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(desc->icon), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
        ui_icon_small = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(desc->icon), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    }
    wc.hIcon = ui_icon_big ? ui_icon_big : LoadIconW(0, MAKEINTRESOURCEW(32512)); // IDI_APPLICATION
    wc.hIconSm = ui_icon_small;
    wc.lpszClassName = desc->class_name;
    RegisterClassExW(&wc);

    F32 sys_scale = (F32)GetDpiForSystem() / 96.0f;
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT, w = (int)(desc->width * sys_scale), h = (int)(desc->height * sys_scale);
    // A saved place opens there, if its monitor is still around. Created right on it, so the window
    // starts out at that monitor's dpi instead of being rescaled by a WM_DPICHANGED.
    RECT saved = { desc->x, desc->y, desc->x + desc->w, desc->y + desc->h };
    B32 use_saved = desc->w > 0 && desc->h > 0 && MonitorFromRect(&saved, MONITOR_DEFAULTTONULL);
    if (use_saved) { x = desc->x; y = desc->y; w = desc->w; h = desc->h; }
    HWND hwnd = CreateWindowExW(0, /* not WS_EX_NOREDIRECTIONBITMAP: the bitblt swapchain presents into that surface (see r_init) */ desc->class_name, desc->title, WS_OVERLAPPEDWINDOW,
                                x, y, w, h, 0, 0, inst, 0);
    if (!hwnd) return 0;
    // While being created the window measured its frame the old way (WM_NCCALCSIZE without the
    // rects ui_caption_message reshapes): measure it again, now without the title bar.
    SetWindowPos(hwnd, 0, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    ui_hwnd = hwnd;
    ui_place_track(hwnd);
    ui_window_place.maximized = use_saved && desc->maximized;
    ui.scale = (F32)GetDpiForWindow(hwnd) / 96.0f;
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    if (desc->accept_files) {
        DragAcceptFiles(hwnd, TRUE);
        // Let Explorer drop files even if the app runs elevated.
        ChangeWindowMessageFilterEx(hwnd, WM_DROPFILES, MSGFLT_ALLOW, 0);
        ChangeWindowMessageFilterEx(hwnd, WM_COPYDATA, MSGFLT_ALLOW, 0);
        ChangeWindowMessageFilterEx(hwnd, 0x0049 /* WM_COPYGLOBALDATA */, MSGFLT_ALLOW, 0);
    }
    if (!r_init(hwnd, UI_ATLAS_SIZE, UI_ATLAS_SIZE)) {
        MessageBoxW(0, L"Could not initialize Direct3D 11.", desc->title, MB_OK | MB_ICONERROR);
        return 0;
    }
    f_init(UI_ATLAS_SIZE, UI_ATLAS_SIZE);
    ui_fonts_load();
    return 1;
}

static int ui_window_run(int show)
{
    // Reopen maximized if it was closed that way, unless the launch asked for something else (minimized).
    if (ui_window_place.maximized && (show == SW_SHOWNORMAL || show == SW_SHOWDEFAULT || show == SW_SHOW)) show = SW_SHOWMAXIMIZED;
    ShowWindow(ui_hwnd, show);
    MSG msg;
    while (GetMessageW(&msg, 0, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

//- Shell open (off the UI thread)

typedef struct UI_ShellOpen { WCHAR *file, *args; } UI_ShellOpen;

static U32 ui_shell_open_thread(void *p)
{
    UI_ShellOpen *so = (UI_ShellOpen *)p;
    CoInitializeEx(0, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    ShellExecuteW(0, L"open", so->file, so->args, 0, SW_SHOWNORMAL);
    CoUninitialize();
    HeapFree(GetProcessHeap(), 0, so);
    return 0;
}

static void ui_shell_open_async(String8 file, String8 args)
{
    Temp t = temp_begin(ui.frame_arena);
    if (ui_open_log.size) { // testing: record instead of opening
        HANDLE h = CreateFileW((WCHAR *)os_str16_from_8(t.arena, ui_open_log).str, FILE_APPEND_DATA, FILE_SHARE_READ, 0, OPEN_ALWAYS, 0, 0);
        if (h != INVALID_HANDLE_VALUE) {
            String8 line = str8_pushf(t.arena, "%.*s %.*s\r\n", S8F(file), S8F(args));
            DWORD wrote;
            WriteFile(h, line.str, (DWORD)line.size, &wrote, 0);
            CloseHandle(h);
        }
        temp_end(t);
        return;
    }
    String16 f16 = os_str16_from_8(t.arena, file), a16 = os_str16_from_8(t.arena, args);
    U64 fb = (f16.size + 1) * sizeof(WCHAR), ab = (a16.size + 1) * sizeof(WCHAR);
    UI_ShellOpen *so = (UI_ShellOpen *)HeapAlloc(GetProcessHeap(), 0, sizeof(UI_ShellOpen) + fb + ab);
    if (so) {
        so->file = (WCHAR *)(so + 1);
        so->args = (WCHAR *)((U8 *)so->file + fb);
        memcpy(so->file, f16.str, fb);
        memcpy(so->args, a16.str, ab);
        if (!args.size) so->args = 0;
        os_thread_start(ui_shell_open_thread, so);
    }
    temp_end(t);
}

static void ui_open_url(String8 url)
{
    if (!str8_starts_with(url, str8_lit("https://"), 1)) return; // only web pages, never files or other schemes
    ui_shell_open_async(url, str8(0, 0));
}

//- Clipboard, menus, dialogs

static String8 ui_clipboard_text(Arena *a)
{
    String8 r = { 0 };
    if (!OpenClipboard(ui_hwnd)) return r;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    WCHAR *w = h ? (WCHAR *)GlobalLock(h) : 0;
    if (w) {
        r = os_str8_from_16(a, (U16 *)w, wcslen(w));
        GlobalUnlock(h);
    }
    CloseClipboard();
    return r;
}

static S32 ui_popup_menu(Rect anchor, String8 *items, U32 count, S32 checked)
{
    Temp t = temp_begin(ui.frame_arena);
    HMENU menu = CreatePopupMenu();
    for (U32 i = 0; i < count; i += 1) {
        if (str8_match(items[i], str8_lit("-"), 0)) { AppendMenuW(menu, MF_SEPARATOR, 0, 0); continue; }
        UINT flags = MF_STRING | ((S32)i == checked ? MF_CHECKED : 0);
        AppendMenuW(menu, flags, i + 1, (WCHAR *)os_str16_from_8(t.arena, items[i]).str);
    }
    POINT pt = { (LONG)anchor.x0, (LONG)anchor.y1 + 2 };
    ClientToScreen(ui_hwnd, &pt);
    UINT cmd = (UINT)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON, pt.x, pt.y, 0, ui_hwnd, 0);
    DestroyMenu(menu);
    temp_end(t);
    return cmd >= 1 && cmd <= count ? (S32)cmd - 1 : -1;
}

static S32 ui_popup_menu_tree(Rect anchor, UI_MenuItem *items, U32 count)
{
    Temp t = temp_begin(ui.frame_arena);
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
    ClientToScreen(ui_hwnd, &pt);
    UINT cmd = (UINT)TrackPopupMenu(root, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON, pt.x, pt.y, 0, ui_hwnd, 0);
    DestroyMenu(root); // and the submenus in it
    temp_end(t);
    return cmd >= 1 && cmd <= count ? (S32)cmd - 1 : -1;
}

static String8 ui_pick_folder(Arena *a, wchar_t *title)
{
    String8 r = { 0 };
    IFileOpenDialog *dlg = 0;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, 0, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&dlg))) return r;
    FILEOPENDIALOGOPTIONS opts = 0;
    IFileOpenDialog_GetOptions(dlg, &opts);
    IFileOpenDialog_SetOptions(dlg, opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    if (title) IFileOpenDialog_SetTitle(dlg, title);
    if (SUCCEEDED(IFileOpenDialog_Show(dlg, ui_hwnd))) {
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
