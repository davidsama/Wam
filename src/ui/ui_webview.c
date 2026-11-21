// ui_webview.c - a visible window that shows one web page (WebView2) and hands back the first file
// that page downloads: it's saved into a folder you choose, then the window closes itself.
//
// Optional part of the UI library: needs WebView2.h and WebView2LoaderStatic.lib (the WebView2 SDK)
// and the WebView2 runtime, which Windows 10/11 include with Edge. ui_web_available() says whether
// the runtime is there; callers fall back to the default browser otherwise.
//
// WebView2 is a COM API driven by completion handlers; they run on the UI thread (STA). The
// handlers here are static objects with a vtable each, since only one window exists at a time.
//
//   ui_web_download(url, title, note, save_dir, done, user)
//     note (optional): a line or two shown in a strip above the page, saying why it opened.
//     done(user, path, ok, error) is called once, on the UI thread: with the saved file's path when
//     the download completed, or ok = 0 if it failed or the user closed the window first.

#include "WebView2.h"

typedef void UI_WebDone(void *user, String8 path, B32 ok, String8 error);

static String8 ui_web_data_dir;   // where WebView2 keeps its profile (cookies etc.); set before use

typedef struct UI_Web {
    HWND hwnd;
    ICoreWebView2Environment *env;
    ICoreWebView2Controller *ctl;
    ICoreWebView2 *view;
    ICoreWebView2DownloadOperation *op;
    UI_WebDone *done;
    void *user;
    WCHAR url[2048], save_dir[1024];
    B32 got_download, finished;
    HWND note;          // the strip above the page that says what this window is for (optional)
    HFONT note_font;
    HBRUSH note_brush;
} UI_Web;

static UI_Web ui_web;
#define UI_WM_WEB_FINISH (WM_APP + 0x200) // wParam: ok, lParam: heap path (WCHAR*) or 0

//- Handlers (IUnknown + Invoke)

// Each handler object is { vtable, its IID }; they're static, so AddRef/Release do nothing.
typedef struct UI_WebHandler { void *lpVtbl; const IID *iid; } UI_WebHandler;
static HRESULT STDMETHODCALLTYPE ui_web_qi(UI_WebHandler *This, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, This->iid)) { *out = This; return S_OK; }
    *out = 0;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE ui_web_addref(UI_WebHandler *This) { (void)This; return 1; }
static ULONG STDMETHODCALLTYPE ui_web_release(UI_WebHandler *This) { (void)This; return 1; }
#define UI_WEB_IUNKNOWN(T) (HRESULT (STDMETHODCALLTYPE *)(T *, REFIID, void **))ui_web_qi, (ULONG (STDMETHODCALLTYPE *)(T *))ui_web_addref, (ULONG (STDMETHODCALLTYPE *)(T *))ui_web_release

static void ui_web_finish(B32 ok, WCHAR *path)
{
    if (ui_web.finished) return;
    ui_web.finished = 1;
    WCHAR *copy = 0;
    if (path) {
        size_t n = wcslen(path) + 1;
        copy = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, n * sizeof(WCHAR));
        if (copy) memcpy(copy, path, n * sizeof(WCHAR));
    }
    // Not from inside a WebView2 callback: tear down from our own message.
    PostMessageW(ui_web.hwnd, UI_WM_WEB_FINISH, (WPARAM)ok, (LPARAM)copy);
}

// The download's progress: done or failed.
static HRESULT STDMETHODCALLTYPE ui_web_state_changed(ICoreWebView2StateChangedEventHandler *This, ICoreWebView2DownloadOperation *op, IUnknown *args)
{
    (void)This; (void)args;
    COREWEBVIEW2_DOWNLOAD_STATE state;
    if (FAILED(ICoreWebView2DownloadOperation_get_State(op, &state))) return S_OK;
    if (state == COREWEBVIEW2_DOWNLOAD_STATE_COMPLETED) {
        LPWSTR path = 0;
        ICoreWebView2DownloadOperation_get_ResultFilePath(op, &path);
        ui_web_finish(1, path);
        if (path) CoTaskMemFree(path);
    } else if (state == COREWEBVIEW2_DOWNLOAD_STATE_INTERRUPTED) {
        ui_web_finish(0, 0);
    }
    return S_OK;
}
static ICoreWebView2StateChangedEventHandlerVtbl ui_web_state_vtbl = { UI_WEB_IUNKNOWN(ICoreWebView2StateChangedEventHandler), ui_web_state_changed };
static UI_WebHandler ui_web_state_handler = { &ui_web_state_vtbl, &IID_ICoreWebView2StateChangedEventHandler };

// The page started a download: save it into our folder (keeping its name), without the download UI.
static HRESULT STDMETHODCALLTYPE ui_web_download_starting(ICoreWebView2DownloadStartingEventHandler *This, ICoreWebView2 *sender, ICoreWebView2DownloadStartingEventArgs *args)
{
    (void)This; (void)sender;
    if (ui_web.got_download) { ICoreWebView2DownloadStartingEventArgs_put_Cancel(args, TRUE); return S_OK; } // one file per window
    ui_web.got_download = 1;
    LPWSTR suggested = 0;
    ICoreWebView2DownloadStartingEventArgs_get_ResultFilePath(args, &suggested);
    WCHAR *name = suggested ? wcsrchr(suggested, L'\\') : 0;
    name = name ? name + 1 : suggested ? suggested : L"download";
    WCHAR target[2048];
    _snwprintf(target, ArrayCount(target) - 1, L"%s\\%s", ui_web.save_dir, name);
    target[ArrayCount(target) - 1] = 0;
    ICoreWebView2DownloadStartingEventArgs_put_ResultFilePath(args, target);
    ICoreWebView2DownloadStartingEventArgs_put_Handled(args, TRUE);
    if (suggested) CoTaskMemFree(suggested);
    if (SUCCEEDED(ICoreWebView2DownloadStartingEventArgs_get_DownloadOperation(args, &ui_web.op)) && ui_web.op) {
        EventRegistrationToken token;
        ICoreWebView2DownloadOperation_add_StateChanged(ui_web.op, (ICoreWebView2StateChangedEventHandler *)&ui_web_state_handler, &token);
    }
    return S_OK;
}
static ICoreWebView2DownloadStartingEventHandlerVtbl ui_web_dl_vtbl = { UI_WEB_IUNKNOWN(ICoreWebView2DownloadStartingEventHandler), ui_web_download_starting };
static UI_WebHandler ui_web_dl_handler = { &ui_web_dl_vtbl, &IID_ICoreWebView2DownloadStartingEventHandler };

// The note strip's height for the window's current width: its text, wrapped, plus padding.
static int ui_web_note_height(void)
{
    if (!ui_web.note) return 0;
    RECT rc;
    GetClientRect(ui_web.hwnd, &rc);
    WCHAR text[1024];
    GetWindowTextW(ui_web.note, text, ArrayCount(text));
    RECT tr = { 0, 0, rc.right - (int)(28 * ui.scale), 0 };
    HDC dc = GetDC(ui_web.note);
    HGDIOBJ old = SelectObject(dc, ui_web.note_font);
    DrawTextW(dc, text, -1, &tr, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
    ReleaseDC(ui_web.note, dc);
    return tr.bottom + (int)(20 * ui.scale);
}

static void ui_web_layout(void)
{
    RECT rc;
    GetClientRect(ui_web.hwnd, &rc);
    int nh = ui_web_note_height(), pad = (int)(14 * ui.scale);
    if (ui_web.note) MoveWindow(ui_web.note, pad, (int)(10 * ui.scale), rc.right - 2 * pad, nh - (int)(20 * ui.scale), TRUE);
    if (!ui_web.ctl) return;
    rc.top += nh;
    ICoreWebView2Controller_put_Bounds(ui_web.ctl, rc);
}

// The browser control exists: hook downloads and open the page.
static HRESULT STDMETHODCALLTYPE ui_web_controller_created(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *This, HRESULT err, ICoreWebView2Controller *ctl)
{
    (void)This;
    if (FAILED(err) || !ctl || !ui_web.hwnd) { ui_web_finish(0, 0); return S_OK; }
    ui_web.ctl = ctl;
    ICoreWebView2Controller_AddRef(ctl);
    ICoreWebView2Controller_get_CoreWebView2(ctl, &ui_web.view);
    ui_web_layout();
    ICoreWebView2_4 *v4 = 0;
    if (ui_web.view && SUCCEEDED(ICoreWebView2_QueryInterface(ui_web.view, &IID_ICoreWebView2_4, (void **)&v4)) && v4) {
        EventRegistrationToken token;
        ICoreWebView2_4_add_DownloadStarting(v4, (ICoreWebView2DownloadStartingEventHandler *)&ui_web_dl_handler, &token);
        ICoreWebView2_4_Release(v4);
    } else {
        ui_web_finish(0, 0); // too old a runtime to hand us the file
        return S_OK;
    }
    ICoreWebView2_Navigate(ui_web.view, ui_web.url);
    return S_OK;
}
static ICoreWebView2CreateCoreWebView2ControllerCompletedHandlerVtbl ui_web_ctl_vtbl = { UI_WEB_IUNKNOWN(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler), ui_web_controller_created };
static UI_WebHandler ui_web_ctl_handler = { &ui_web_ctl_vtbl, &IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler };

static HRESULT STDMETHODCALLTYPE ui_web_env_created(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *This, HRESULT err, ICoreWebView2Environment *env)
{
    (void)This;
    if (FAILED(err) || !env || !ui_web.hwnd) { ui_web_finish(0, 0); return S_OK; }
    ui_web.env = env;
    ICoreWebView2Environment_AddRef(env);
    ICoreWebView2Environment_CreateCoreWebView2Controller(env, ui_web.hwnd, (ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *)&ui_web_ctl_handler);
    return S_OK;
}
static ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandlerVtbl ui_web_env_vtbl = { UI_WEB_IUNKNOWN(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler), ui_web_env_created };
static UI_WebHandler ui_web_env_handler = { &ui_web_env_vtbl, &IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler };

//- Window

static void ui_web_teardown(void)
{
    if (ui_web.op) { ICoreWebView2DownloadOperation_Release(ui_web.op); ui_web.op = 0; }
    if (ui_web.view) { ICoreWebView2_Release(ui_web.view); ui_web.view = 0; }
    if (ui_web.ctl) { ICoreWebView2Controller_Close(ui_web.ctl); ICoreWebView2Controller_Release(ui_web.ctl); ui_web.ctl = 0; }
    if (ui_web.env) { ICoreWebView2Environment_Release(ui_web.env); ui_web.env = 0; }
}

static LRESULT CALLBACK ui_web_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
        case WM_SIZE: ui_web_layout(); return 0;
        case WM_CTLCOLORSTATIC: { // the note: light text on the window's dark strip
            HDC dc = (HDC)wp;
            SetTextColor(dc, RGB(0xff, 0xe3, 0xb0));
            SetBkColor(dc, RGB(0x2a, 0x22, 0x14));
            return (LRESULT)ui_web.note_brush;
        }
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            rc.bottom = ui_web_note_height();
            if (ui_web.note_brush) FillRect((HDC)wp, &rc, ui_web.note_brush);
            return 1;
        }
        case WM_CLOSE:
            if (!ui_web.finished) ui_web_finish(0, 0); // closed by the user before the download finished
            return 0;
        case UI_WM_WEB_FINISH: {
            WCHAR *path = (WCHAR *)lp;
            UI_WebDone *done = ui_web.done;
            void *user = ui_web.user;
            B32 ok = (B32)wp && path;
            ui_web_teardown();
            DestroyWindow(hwnd);
            ui_web.hwnd = 0;
            ui_web.note = 0;
            if (ui_web.note_font) { DeleteObject(ui_web.note_font); ui_web.note_font = 0; }
            if (ui_web.note_brush) { DeleteObject(ui_web.note_brush); ui_web.note_brush = 0; }
            ui_web.done = 0;
            Temp t = temp_begin(ui.frame_arena);
            String8 p = path ? os_str8_from_16(t.arena, (U16 *)path, wcslen(path)) : str8(0, 0);
            if (done) done(user, p, ok, ok ? str8(0, 0) : str8_lit("the download didn't finish"));
            temp_end(t);
            if (path) HeapFree(GetProcessHeap(), 0, path);
            ui_request_frame();
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static B32 ui_web_available(void)
{
    LPWSTR version = 0;
    HRESULT hr = GetAvailableCoreWebView2BrowserVersionString(0, &version);
    if (version) CoTaskMemFree(version);
    return SUCCEEDED(hr) && version != 0;
}

static B32 ui_web_busy(void) { return ui_web.hwnd != 0; }

static B32 ui_web_download(String8 url, String8 title, String8 note, String8 save_dir, UI_WebDone *done, void *user)
{
    if (ui_web.hwnd) return 0; // one at a time
    static B32 registered;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = ui_web_proc;
        wc.hInstance = GetModuleHandleW(0);
        wc.hCursor = LoadCursorW(0, MAKEINTRESOURCEW(32512));
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.hIcon = ui_icon_big;
        wc.hIconSm = ui_icon_small;
        wc.lpszClassName = L"ui_web";
        RegisterClassExW(&wc);
        registered = 1;
    }
    Temp t = temp_begin(ui.frame_arena);
    memset(&ui_web, 0, sizeof(ui_web));
    String16 u16 = os_str16_from_8(t.arena, url), d16 = os_str16_from_8(t.arena, save_dir), t16 = os_str16_from_8(t.arena, title);
    if (u16.size >= ArrayCount(ui_web.url) || d16.size >= ArrayCount(ui_web.save_dir)) { temp_end(t); return 0; }
    memcpy(ui_web.url, u16.str, (u16.size + 1) * sizeof(WCHAR));
    memcpy(ui_web.save_dir, d16.str, (d16.size + 1) * sizeof(WCHAR));
    ui_web.done = done;
    ui_web.user = user;

    // Centered over the app's window, owned by it (stays in front, closes with it).
    RECT owner;
    GetWindowRect(ui_hwnd, &owner);
    int w = (int)(760 * ui.scale), h = (int)(600 * ui.scale);
    int x = (owner.left + owner.right - w) / 2, y = (owner.top + owner.bottom - h) / 2;
    ui_web.hwnd = CreateWindowExW(0, L"ui_web", (WCHAR *)t16.str, WS_OVERLAPPEDWINDOW, x, y, w, h, ui_hwnd, 0, GetModuleHandleW(0), 0);
    if (!ui_web.hwnd) { temp_end(t); return 0; }
    BOOL dark = TRUE;
    DwmSetWindowAttribute(ui_web.hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    if (note.size) {
        // A strip above the page saying why this window opened and that it goes away by itself.
        ui_web.note_brush = CreateSolidBrush(RGB(0x2a, 0x22, 0x14));
        ui_web.note_font = CreateFontW(-(int)(14 * ui.scale), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        String16 n16 = os_str16_from_8(t.arena, note);
        ui_web.note = CreateWindowExW(0, L"STATIC", (WCHAR *)n16.str, WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 0, 0, 10, 10, ui_web.hwnd, 0, GetModuleHandleW(0), 0);
        if (ui_web.note) SendMessageW(ui_web.note, WM_SETFONT, (WPARAM)ui_web.note_font, TRUE);
        ui_web_layout();
    }
    ShowWindow(ui_web.hwnd, SW_SHOWNORMAL);

    String16 data16 = os_str16_from_8(t.arena, ui_web_data_dir);
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(0, ui_web_data_dir.size ? (WCHAR *)data16.str : 0, 0, (ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *)&ui_web_env_handler);
    temp_end(t);
    if (FAILED(hr)) { DestroyWindow(ui_web.hwnd); ui_web.hwnd = 0; return 0; }
    return 1;
}
