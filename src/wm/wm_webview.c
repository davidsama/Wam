// wm_webview.c - a visible window that shows one web page (WebView2) and hands back the first file
// that page downloads: it's saved into a folder you choose, then the window closes itself. See wm.h.
//
// Optional part of the window layer: needs WebView2.h and WebView2LoaderStatic.lib (the WebView2
// SDK) and the WebView2 runtime, which Windows 10/11 include with Edge. wm_web_available() says
// whether the runtime is there; callers fall back to the default browser otherwise.
//
// WebView2 is a COM API driven by completion handlers; they run on the UI thread (STA). The
// handlers here are static objects with a vtable each, since only one window exists at a time.

#include "WebView2.h"

typedef struct WM_Web {
    HWND hwnd;
    ICoreWebView2Environment *env;
    ICoreWebView2Controller *ctl;
    ICoreWebView2 *view;
    ICoreWebView2DownloadOperation *op;
    WM_WebDone *done;
    void *user;
    WCHAR url[2048], save_dir[1024];
    B32 got_download, finished;
    HWND note;          // the strip above the page that says what this window is for (optional)
    HFONT note_font;
    HBRUSH note_brush;
} WM_Web;

static WM_Web wm_web;
#define WM_MSG_WEB_FINISH (WM_APP + 0x200) // wParam: ok, lParam: heap path (WCHAR*) or 0

//- Handlers (IUnknown + Invoke)

// Each handler object is { vtable, its IID }; they're static, so AddRef/Release do nothing.
typedef struct WM_WebHandler { void *lpVtbl; const IID *iid; } WM_WebHandler;
static HRESULT STDMETHODCALLTYPE wm_web_qi(WM_WebHandler *This, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, This->iid)) { *out = This; return S_OK; }
    *out = 0;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE wm_web_addref(WM_WebHandler *This) { (void)This; return 1; }
static ULONG STDMETHODCALLTYPE wm_web_release(WM_WebHandler *This) { (void)This; return 1; }
#define WM_WEB_IUNKNOWN(T) (HRESULT (STDMETHODCALLTYPE *)(T *, REFIID, void **))wm_web_qi, (ULONG (STDMETHODCALLTYPE *)(T *))wm_web_addref, (ULONG (STDMETHODCALLTYPE *)(T *))wm_web_release

static void wm_web_finish(B32 ok, WCHAR *path)
{
    if (wm_web.finished) return;
    wm_web.finished = 1;
    WCHAR *copy = 0;
    if (path) {
        size_t n = wcslen(path) + 1;
        copy = (WCHAR *)os_alloc(n * sizeof(WCHAR));
        if (copy) memcpy(copy, path, n * sizeof(WCHAR));
    }
    // Not from inside a WebView2 callback: tear down from our own message.
    PostMessageW(wm_web.hwnd, WM_MSG_WEB_FINISH, (WPARAM)ok, (LPARAM)copy);
}

// The download's progress: done or failed.
static HRESULT STDMETHODCALLTYPE wm_web_state_changed(ICoreWebView2StateChangedEventHandler *This, ICoreWebView2DownloadOperation *op, IUnknown *args)
{
    (void)This; (void)args;
    COREWEBVIEW2_DOWNLOAD_STATE state;
    if (FAILED(ICoreWebView2DownloadOperation_get_State(op, &state))) return S_OK;
    if (state == COREWEBVIEW2_DOWNLOAD_STATE_COMPLETED) {
        LPWSTR path = 0;
        ICoreWebView2DownloadOperation_get_ResultFilePath(op, &path);
        wm_web_finish(1, path);
        if (path) CoTaskMemFree(path);
    } else if (state == COREWEBVIEW2_DOWNLOAD_STATE_INTERRUPTED) {
        wm_web_finish(0, 0);
    }
    return S_OK;
}
static ICoreWebView2StateChangedEventHandlerVtbl wm_web_state_vtbl = { WM_WEB_IUNKNOWN(ICoreWebView2StateChangedEventHandler), wm_web_state_changed };
static WM_WebHandler wm_web_state_handler = { &wm_web_state_vtbl, &IID_ICoreWebView2StateChangedEventHandler };

// The page started a download: save it into our folder (keeping its name), without the download UI.
static HRESULT STDMETHODCALLTYPE wm_web_download_starting(ICoreWebView2DownloadStartingEventHandler *This, ICoreWebView2 *sender, ICoreWebView2DownloadStartingEventArgs *args)
{
    (void)This; (void)sender;
    if (wm_web.got_download) { ICoreWebView2DownloadStartingEventArgs_put_Cancel(args, TRUE); return S_OK; } // one file per window
    wm_web.got_download = 1;
    LPWSTR suggested = 0;
    ICoreWebView2DownloadStartingEventArgs_get_ResultFilePath(args, &suggested);
    WCHAR *name = suggested ? wcsrchr(suggested, L'\\') : 0;
    name = name ? name + 1 : suggested ? suggested : L"download";
    WCHAR target[2048];
    _snwprintf(target, ArrayCount(target) - 1, L"%s\\%s", wm_web.save_dir, name);
    target[ArrayCount(target) - 1] = 0;
    ICoreWebView2DownloadStartingEventArgs_put_ResultFilePath(args, target);
    ICoreWebView2DownloadStartingEventArgs_put_Handled(args, TRUE);
    if (suggested) CoTaskMemFree(suggested);
    if (SUCCEEDED(ICoreWebView2DownloadStartingEventArgs_get_DownloadOperation(args, &wm_web.op)) && wm_web.op) {
        EventRegistrationToken token;
        ICoreWebView2DownloadOperation_add_StateChanged(wm_web.op, (ICoreWebView2StateChangedEventHandler *)&wm_web_state_handler, &token);
    }
    return S_OK;
}
static ICoreWebView2DownloadStartingEventHandlerVtbl wm_web_dl_vtbl = { WM_WEB_IUNKNOWN(ICoreWebView2DownloadStartingEventHandler), wm_web_download_starting };
static WM_WebHandler wm_web_dl_handler = { &wm_web_dl_vtbl, &IID_ICoreWebView2DownloadStartingEventHandler };

// The note strip's height for the window's current width: its text, wrapped, plus padding.
static int wm_web_note_height(void)
{
    if (!wm_web.note) return 0;
    RECT rc;
    GetClientRect(wm_web.hwnd, &rc);
    WCHAR text[1024];
    GetWindowTextW(wm_web.note, text, ArrayCount(text));
    RECT tr = { 0, 0, rc.right - (int)(28 * wm_dpi_scale), 0 };
    HDC dc = GetDC(wm_web.note);
    HGDIOBJ old = SelectObject(dc, wm_web.note_font);
    DrawTextW(dc, text, -1, &tr, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
    ReleaseDC(wm_web.note, dc);
    return tr.bottom + (int)(20 * wm_dpi_scale);
}

static void wm_web_layout(void)
{
    RECT rc;
    GetClientRect(wm_web.hwnd, &rc);
    int nh = wm_web_note_height(), pad = (int)(14 * wm_dpi_scale);
    if (wm_web.note) MoveWindow(wm_web.note, pad, (int)(10 * wm_dpi_scale), rc.right - 2 * pad, nh - (int)(20 * wm_dpi_scale), TRUE);
    if (!wm_web.ctl) return;
    rc.top += nh;
    ICoreWebView2Controller_put_Bounds(wm_web.ctl, rc);
}

// The browser control exists: hook downloads and open the page.
static HRESULT STDMETHODCALLTYPE wm_web_controller_created(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *This, HRESULT err, ICoreWebView2Controller *ctl)
{
    (void)This;
    if (FAILED(err) || !ctl || !wm_web.hwnd) { wm_web_finish(0, 0); return S_OK; }
    wm_web.ctl = ctl;
    ICoreWebView2Controller_AddRef(ctl);
    ICoreWebView2Controller_get_CoreWebView2(ctl, &wm_web.view);
    wm_web_layout();
    ICoreWebView2_4 *v4 = 0;
    if (wm_web.view && SUCCEEDED(ICoreWebView2_QueryInterface(wm_web.view, &IID_ICoreWebView2_4, (void **)&v4)) && v4) {
        EventRegistrationToken token;
        ICoreWebView2_4_add_DownloadStarting(v4, (ICoreWebView2DownloadStartingEventHandler *)&wm_web_dl_handler, &token);
        ICoreWebView2_4_Release(v4);
    } else {
        wm_web_finish(0, 0); // too old a runtime to hand us the file
        return S_OK;
    }
    ICoreWebView2_Navigate(wm_web.view, wm_web.url);
    return S_OK;
}
static ICoreWebView2CreateCoreWebView2ControllerCompletedHandlerVtbl wm_web_ctl_vtbl = { WM_WEB_IUNKNOWN(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler), wm_web_controller_created };
static WM_WebHandler wm_web_ctl_handler = { &wm_web_ctl_vtbl, &IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler };

static HRESULT STDMETHODCALLTYPE wm_web_env_created(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *This, HRESULT err, ICoreWebView2Environment *env)
{
    (void)This;
    if (FAILED(err) || !env || !wm_web.hwnd) { wm_web_finish(0, 0); return S_OK; }
    wm_web.env = env;
    ICoreWebView2Environment_AddRef(env);
    ICoreWebView2Environment_CreateCoreWebView2Controller(env, wm_web.hwnd, (ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *)&wm_web_ctl_handler);
    return S_OK;
}
static ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandlerVtbl wm_web_env_vtbl = { WM_WEB_IUNKNOWN(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler), wm_web_env_created };
static WM_WebHandler wm_web_env_handler = { &wm_web_env_vtbl, &IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler };

//- Window

static void wm_web_teardown(void)
{
    if (wm_web.op) { ICoreWebView2DownloadOperation_Release(wm_web.op); wm_web.op = 0; }
    if (wm_web.view) { ICoreWebView2_Release(wm_web.view); wm_web.view = 0; }
    if (wm_web.ctl) { ICoreWebView2Controller_Close(wm_web.ctl); ICoreWebView2Controller_Release(wm_web.ctl); wm_web.ctl = 0; }
    if (wm_web.env) { ICoreWebView2Environment_Release(wm_web.env); wm_web.env = 0; }
}

static LRESULT CALLBACK wm_web_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
        case WM_SIZE: wm_web_layout(); return 0;
        case WM_CTLCOLORSTATIC: { // the note: light text on the window's dark strip
            HDC dc = (HDC)wp;
            SetTextColor(dc, RGB(0xff, 0xe3, 0xb0));
            SetBkColor(dc, RGB(0x2a, 0x22, 0x14));
            return (LRESULT)wm_web.note_brush;
        }
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            rc.bottom = wm_web_note_height();
            if (wm_web.note_brush) FillRect((HDC)wp, &rc, wm_web.note_brush);
            return 1;
        }
        case WM_CLOSE:
            if (!wm_web.finished) wm_web_finish(0, 0); // closed by the user before the download finished
            return 0;
        case WM_MSG_WEB_FINISH: {
            WCHAR *path = (WCHAR *)lp;
            WM_WebDone *done = wm_web.done;
            void *user = wm_web.user;
            B32 ok = (B32)wp && path;
            wm_web_teardown();
            DestroyWindow(hwnd);
            wm_web.hwnd = 0;
            wm_web.note = 0;
            if (wm_web.note_font) { DeleteObject(wm_web.note_font); wm_web.note_font = 0; }
            if (wm_web.note_brush) { DeleteObject(wm_web.note_brush); wm_web.note_brush = 0; }
            wm_web.done = 0;
            Temp t = scratch_begin(0);
            String8 p = path ? os_str8_from_16(t.arena, (U16 *)path, wcslen(path)) : str8(0, 0);
            if (done) done(user, p, ok, ok ? str8(0, 0) : str8_lit("the download didn't finish"));
            scratch_end(t);
            os_free(path);
            wm_request_frame();
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static B32 wm_web_available(void)
{
    LPWSTR version = 0;
    HRESULT hr = GetAvailableCoreWebView2BrowserVersionString(0, &version);
    if (version) CoTaskMemFree(version);
    return SUCCEEDED(hr) && version != 0;
}

static B32 wm_web_busy(void) { return wm_web.hwnd != 0; }

static B32 wm_web_download(String8 url, String8 title, String8 note, String8 save_dir, WM_WebDone *done, void *user)
{
    if (wm_web.hwnd) return 0; // one at a time
    static B32 registered;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = wm_web_proc;
        wc.hInstance = GetModuleHandleW(0);
        wc.hCursor = LoadCursorW(0, MAKEINTRESOURCEW(32512));
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.hIcon = wm_icon_big;
        wc.hIconSm = wm_icon_small;
        wc.lpszClassName = L"wm_web";
        RegisterClassExW(&wc);
        registered = 1;
    }
    Temp t = scratch_begin(0);
    memset(&wm_web, 0, sizeof(wm_web));
    String16 u16 = os_str16_from_8(t.arena, url), d16 = os_str16_from_8(t.arena, save_dir), t16 = os_str16_from_8(t.arena, title);
    if (u16.size >= ArrayCount(wm_web.url) || d16.size >= ArrayCount(wm_web.save_dir)) { scratch_end(t); return 0; }
    memcpy(wm_web.url, u16.str, (u16.size + 1) * sizeof(WCHAR));
    memcpy(wm_web.save_dir, d16.str, (d16.size + 1) * sizeof(WCHAR));
    wm_web.done = done;
    wm_web.user = user;

    // Centered over the app's window, owned by it (stays in front, closes with it).
    RECT owner;
    GetWindowRect(wm_hwnd, &owner);
    int w = (int)(760 * wm_dpi_scale), h = (int)(600 * wm_dpi_scale);
    int x = (owner.left + owner.right - w) / 2, y = (owner.top + owner.bottom - h) / 2;
    wm_web.hwnd = CreateWindowExW(0, L"wm_web", (WCHAR *)t16.str, WS_OVERLAPPEDWINDOW, x, y, w, h, wm_hwnd, 0, GetModuleHandleW(0), 0);
    if (!wm_web.hwnd) { scratch_end(t); return 0; }
    BOOL dark = TRUE;
    DwmSetWindowAttribute(wm_web.hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    if (note.size) {
        // A strip above the page saying why this window opened and that it goes away by itself.
        wm_web.note_brush = CreateSolidBrush(RGB(0x2a, 0x22, 0x14));
        wm_web.note_font = CreateFontW(-(int)(14 * wm_dpi_scale), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        String16 n16 = os_str16_from_8(t.arena, note);
        wm_web.note = CreateWindowExW(0, L"STATIC", (WCHAR *)n16.str, WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 0, 0, 10, 10, wm_web.hwnd, 0, GetModuleHandleW(0), 0);
        if (wm_web.note) SendMessageW(wm_web.note, WM_SETFONT, (WPARAM)wm_web.note_font, TRUE);
        wm_web_layout();
    }
    ShowWindow(wm_web.hwnd, SW_SHOWNORMAL);

    String16 data16 = os_str16_from_8(t.arena, wm_web_data_dir);
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(0, wm_web_data_dir.size ? (WCHAR *)data16.str : 0, 0, (ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *)&wm_web_env_handler);
    scratch_end(t);
    if (FAILED(hr)) { DestroyWindow(wm_web.hwnd); wm_web.hwnd = 0; return 0; }
    return 1;
}
