// http.c - small WinHTTP client: HTTPS only, follows redirects, optional progress.

typedef struct HTTP_Result { B32 ok; U32 status; String8 body; String8 error; U32 retry_after; /* seconds, from a 429/503's Retry-After */ } HTTP_Result;
typedef void HTTP_ProgressFunc(void *user, U64 got, U64 total);

static HINTERNET volatile http_session;

// One WinHTTP session for the process. Several threads make requests (the task worker, search
// as you type), so the first ones may race here: the loser closes its session and uses the winner's.
static HINTERNET http_get_session(void)
{
    if (http_session) return http_session;
    #define HTTP_WIDE_(s) L##s
    #define HTTP_WIDE(s) HTTP_WIDE_(s)
    HINTERNET s = WinHttpOpen(L"wam/" HTTP_WIDE(WAM_VERSION) L" (WoW addon manager)", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return 0;
    DWORD decompress = WINHTTP_DECOMPRESSION_FLAG_ALL;
    WinHttpSetOption(s, WINHTTP_OPTION_DECOMPRESSION, &decompress, sizeof(decompress));
    DWORD http2 = WINHTTP_PROTOCOL_FLAG_HTTP2;
    WinHttpSetOption(s, WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL, &http2, sizeof(http2));
    HINTERNET prev = InterlockedCompareExchangePointer((PVOID volatile *)&http_session, s, 0);
    if (prev) { WinHttpCloseHandle(s); return prev; }
    return s;
}

// redirect (optional): redirects aren't followed; it gets where one points. (Followed ones may carry
// the headers along, to whatever host that is.)
static HTTP_Result http_request_full(Arena *a, char *method, String8 host, U16 port, String8 path, String8 headers,
                                     String8 body, HTTP_ProgressFunc *progress, void *user, String8 *redirect)
{
    HTTP_Result result = { 0 };
    Temp scratch = scratch_begin(a);
    HINTERNET session = http_get_session();
    HINTERNET conn = 0, req = 0;
    if (!session) { result.error = str8_lit("WinHttpOpen failed"); goto done; }

    String16 host16 = os_str16_from_8(scratch.arena, host);
    String16 method16 = os_str16_from_8(scratch.arena, str8_cstring(method));
    String16 path16 = os_str16_from_8(scratch.arena, path);
    String16 headers16 = os_str16_from_8(scratch.arena, headers);

    conn = WinHttpConnect(session, (WCHAR *)host16.str, port, 0);
    if (!conn) { result.error = str8_lit("could not connect"); goto done; }
    req = WinHttpOpenRequest(conn, (WCHAR *)method16.str, (WCHAR *)path16.str, 0, WINHTTP_NO_REFERER,
                             WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!req) { result.error = str8_lit("could not open request"); goto done; }

    if (redirect) {
        DWORD no_redirects = WINHTTP_DISABLE_REDIRECTS;
        if (!WinHttpSetOption(req, WINHTTP_OPTION_DISABLE_FEATURE, &no_redirects, sizeof(no_redirects))) {
            result.error = str8_lit("could not open request");
            goto done;
        }
    }
    if (!WinHttpSendRequest(req, headers16.size ? (WCHAR *)headers16.str : WINHTTP_NO_ADDITIONAL_HEADERS,
                            headers16.size ? (DWORD)-1L : 0,
                            body.size ? body.str : WINHTTP_NO_REQUEST_DATA, (DWORD)body.size, (DWORD)body.size, 0) ||
        !WinHttpReceiveResponse(req, 0)) {
        result.error = str8_pushf(a, "network error %lu", GetLastError());
        goto done;
    }

    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    result.status = status;
    if (redirect && status >= 300 && status < 400) {
        WCHAR loc[4096];
        DWORD loc_bytes = sizeof(loc);
        if (WinHttpQueryHeaders(req, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, loc, &loc_bytes, WINHTTP_NO_HEADER_INDEX)) {
            *redirect = os_str8_from_16(a, (U16 *)loc, loc_bytes / sizeof(WCHAR));
        }
        result.error = str8_pushf(a, "HTTP %u", status);
        goto done;
    }
    if (status == 429 || status == 503) {
        DWORD retry = 0;
        size = sizeof(retry);
        if (WinHttpQueryHeaders(req, WINHTTP_QUERY_RETRY_AFTER | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                &retry, &size, WINHTTP_NO_HEADER_INDEX)) result.retry_after = retry;
        size = sizeof(status);
    }
    DWORD content_length = 0;
    size = sizeof(content_length);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &content_length, &size, WINHTTP_NO_HEADER_INDEX);

    String8List chunks = { 0 };
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
        U8 *buf = push_array_nz(scratch.arena, U8, avail);
        DWORD got = 0;
        if (!WinHttpReadData(req, buf, avail, &got) || got == 0) break;
        str8_list_push(scratch.arena, &chunks, str8(buf, got));
        if (progress) progress(user, chunks.total_size, content_length);
    }
    result.body = str8_list_join(a, &chunks, str8_lit(""));
    result.ok = status >= 200 && status < 300;
    if (!result.ok) result.error = str8_pushf(a, "HTTP %u", status);

done:
    if (req) WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    scratch_end(scratch);
    return result;
}

static HTTP_Result http_request_ex(Arena *a, char *method, String8 host, U16 port, String8 path, String8 headers,
                                   String8 body, HTTP_ProgressFunc *progress, void *user)
{
    return http_request_full(a, method, host, port, path, headers, body, progress, user, 0);
}

// Percent-encodes what may not appear in a URL as is: spaces and other control characters, bytes
// outside ASCII (UTF-8 file names), and " < > \ ^ ` { | }. Everything else, % included (already
// encoded parts), stays. Returns url itself when there's nothing to encode. Sites hand out links
// like "https://.../MSBT 5.2.9 In Action.png"; wam keeps picture links in space-separated lists.
static String8 url_escape_unsafe(Arena *a, String8 url)
{
    U64 extra = 0;
    for (U64 i = 0; i < url.size; i += 1) {
        U8 c = url.str[i];
        if (c <= ' ' || c >= 0x7F || strchr("\"<>\\^`{|}", c)) extra += 2;
    }
    if (!extra) return url;
    U8 *out = push_array_nz(a, U8, url.size + extra + 1);
    U64 n = 0;
    for (U64 i = 0; i < url.size; i += 1) {
        U8 c = url.str[i];
        if (c <= ' ' || c >= 0x7F || strchr("\"<>\\^`{|}", c)) n += (U64)sprintf((char *)out + n, "%%%02X", c);
        else out[n++] = c;
    }
    return str8(out, n);
}

//- Rate limits
//
// An API that answers 429 (too many requests) isn't asked again until its pause is over: requests
// in the meantime fail at once, without touching the network, so wam never keeps knocking on a
// door that told it to wait. The pause is the server's Retry-After, else a minute (5 s .. 5 min).

typedef struct HTTP_Pause { S64 volatile until_ms; } HTTP_Pause;

// Seconds left of the pause (0 = not paused).
static U32 http_pause_left(HTTP_Pause *p)
{
    S64 left = p->until_ms - (S64)GetTickCount64();
    return left > 0 ? (U32)((left + 999) / 1000) : 0;
}

static void http_pause_start(HTTP_Pause *p, U32 retry_after)
{
    S64 secs = retry_after ? Clamp((S64)retry_after, 5, 300) : 60;
    S64 until = (S64)GetTickCount64() + secs * 1000;
    if (until > p->until_ms) InterlockedExchange64(&p->until_ms, until);
}

// GET a full URL. Plain http:// URLs are upgraded to https://. redirect: see http_request_full.
static HTTP_Result http_get_url_ex(Arena *a, String8 url, String8 headers, HTTP_ProgressFunc *progress, void *user, String8 *redirect)
{
    HTTP_Result result = { 0 };
    Temp scratch = scratch_begin(a);
    url = url_escape_unsafe(scratch.arena, url);
    if (str8_starts_with(url, str8_lit("http://"), 1)) url = str8_pushf(scratch.arena, "https://%.*s", S8F(str8_skip(url, 7)));
    String16 url16 = os_str16_from_8(scratch.arena, url);
    URL_COMPONENTSW uc = { sizeof(uc) };
    uc.dwHostNameLength = (DWORD)-1;
    uc.dwUrlPathLength = (DWORD)-1;
    uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl((WCHAR *)url16.str, (DWORD)url16.size, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) {
        result.error = str8_lit("bad URL");
        scratch_end(scratch);
        return result;
    }
    String8 host = os_str8_from_16(scratch.arena, (U16 *)uc.lpszHostName, uc.dwHostNameLength);
    String8 path = os_str8_from_16(scratch.arena, (U16 *)uc.lpszUrlPath, uc.dwUrlPathLength + uc.dwExtraInfoLength);
    result = http_request_full(a, "GET", host, uc.nPort, path, headers, str8(0, 0), progress, user, redirect);
    scratch_end(scratch);
    return result;
}

static HTTP_Result http_get_url(Arena *a, String8 url, String8 headers, HTTP_ProgressFunc *progress, void *user)
{
    return http_get_url_ex(a, url, headers, progress, user, 0);
}

static String8 url_encode(Arena *a, String8 s)
{
    U8 *out = push_array_nz(a, U8, s.size * 3 + 1);
    U64 n = 0;
    for (U64 i = 0; i < s.size; i += 1) {
        U8 c = s.str[i];
        B32 safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        if (safe) out[n++] = c;
        else n += (U64)sprintf((char *)out + n, "%%%02X", c);
    }
    out[n] = 0;
    return str8(out, n);
}

// Where a URL redirects to, without following it: GitHub's releases/latest answers 302 with the
// newest tag in Location, which isn't counted against the API's 60 requests an hour.
typedef struct HTTP_Redirect { B32 ok; U32 status; String8 location; String8 error; } HTTP_Redirect;

static HTTP_Redirect http_redirect_target(Arena *a, String8 url)
{
    HTTP_Redirect result = { 0 };
    Temp scratch = scratch_begin(a);
    String16 url16 = os_str16_from_8(scratch.arena, url);
    URL_COMPONENTSW uc = { sizeof(uc) };
    uc.dwHostNameLength = (DWORD)-1;
    uc.dwUrlPathLength = (DWORD)-1;
    uc.dwExtraInfoLength = (DWORD)-1;
    HINTERNET session = http_get_session(), conn = 0, req = 0;
    if (!session || !WinHttpCrackUrl((WCHAR *)url16.str, (DWORD)url16.size, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) {
        result.error = str8_lit("bad URL");
        goto done;
    }
    String16 host = os_str16_from_8(scratch.arena, os_str8_from_16(scratch.arena, (U16 *)uc.lpszHostName, uc.dwHostNameLength));
    String16 path = os_str16_from_8(scratch.arena, os_str8_from_16(scratch.arena, (U16 *)uc.lpszUrlPath, uc.dwUrlPathLength + uc.dwExtraInfoLength));
    conn = WinHttpConnect(session, (WCHAR *)host.str, uc.nPort, 0);
    req = conn ? WinHttpOpenRequest(conn, L"HEAD", (WCHAR *)path.str, 0, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : 0;
    if (!req) { result.error = str8_lit("could not connect"); goto done; }
    DWORD no_redirects = WINHTTP_DISABLE_REDIRECTS;
    WinHttpSetOption(req, WINHTTP_OPTION_DISABLE_FEATURE, &no_redirects, sizeof(no_redirects));
    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(req, 0)) {
        result.error = str8_pushf(a, "network error %lu", GetLastError());
        goto done;
    }
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    result.status = status;
    WCHAR loc[2048];
    DWORD loc_bytes = sizeof(loc);
    if (WinHttpQueryHeaders(req, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, loc, &loc_bytes, WINHTTP_NO_HEADER_INDEX)) {
        result.location = os_str8_from_16(a, (U16 *)loc, loc_bytes / sizeof(WCHAR));
    }
    result.ok = 1;
done:
    if (req) WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    scratch_end(scratch);
    return result;
}
