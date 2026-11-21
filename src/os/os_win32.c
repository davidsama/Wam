// os_win32.c - expects <windows.h> to be included by the unity build entry point.
#include <wincrypt.h>

static void *os_reserve(U64 size) { return VirtualAlloc(0, size, MEM_RESERVE, PAGE_READWRITE); }
static B32 os_commit(void *ptr, U64 size) { return VirtualAlloc(ptr, size, MEM_COMMIT, PAGE_READWRITE) != 0; }
static void os_release(void *ptr, U64 size) { (void)size; VirtualFree(ptr, 0, MEM_RELEASE); }

static String16 os_str16_from_8(Arena *a, String8 s)
{
    int n = s.size ? MultiByteToWideChar(CP_UTF8, 0, (char *)s.str, (int)s.size, 0, 0) : 0;
    U16 *buf = push_array_nz(a, U16, (U64)n + 1);
    if (n) MultiByteToWideChar(CP_UTF8, 0, (char *)s.str, (int)s.size, (WCHAR *)buf, n);
    buf[n] = 0;
    String16 r = { buf, (U64)n };
    return r;
}

static String8 os_str8_from_16(Arena *a, U16 *s, U64 len)
{
    int n = len ? WideCharToMultiByte(CP_UTF8, 0, (WCHAR *)s, (int)len, 0, 0, 0, 0) : 0;
    U8 *buf = push_array_nz(a, U8, (U64)n + 1);
    if (n) WideCharToMultiByte(CP_UTF8, 0, (WCHAR *)s, (int)len, (char *)buf, n, 0, 0);
    buf[n] = 0;
    return str8(buf, (U64)n);
}

// Absolute paths get the \\?\ prefix so paths longer than MAX_PATH (260) work; addon zips
// with deep Libs\ folders exceed it easily. That form needs backslashes and no doubled separators.
static String16 os_path16(Arena *a, String8 path)
{
    if (path.size < 3 || path.str[1] != ':' || (path.str[2] != '\\' && path.str[2] != '/')) return os_str16_from_8(a, path);
    U8 *buf = push_array_nz(a, U8, path.size + 5);
    memcpy(buf, "\\\\?\\", 4);
    U64 n = 4;
    for (U64 i = 0; i < path.size; i += 1) {
        U8 c = path.str[i] == '/' ? '\\' : path.str[i];
        if (c == '\\' && buf[n - 1] == '\\' && n > 4) continue;
        buf[n++] = c;
    }
    return os_str16_from_8(a, str8(buf, n));
}

static String8 os_read_file(Arena *a, String8 path)
{
    String8 result = { 0 };
    Temp scratch = scratch_begin(a);
    String16 p16 = os_path16(scratch.arena, path);
    HANDLE h = CreateFileW((WCHAR *)p16.str, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, 0);
    scratch_end(scratch);
    if (h == INVALID_HANDLE_VALUE) return result;
    LARGE_INTEGER size;
    if (GetFileSizeEx(h, &size)) {
        U64 total_size = (U64)size.QuadPart;
        U8 *buf = push_array_nz(a, U8, total_size + 1);
        U64 total = 0;
        while (total < total_size) {
            DWORD chunk = (DWORD)Min(total_size - total, MB(64));
            DWORD got = 0;
            if (!ReadFile(h, buf + total, chunk, &got, 0) || got == 0) break;
            total += got;
        }
        buf[total] = 0;
        result = str8(buf, total);
    }
    CloseHandle(h);
    return result;
}

static String8 os_read_file_range(Arena *a, String8 path, U64 offset, U64 size)
{
    String8 result = { 0 };
    Temp scratch = scratch_begin(a);
    String16 p16 = os_path16(scratch.arena, path);
    HANDLE h = CreateFileW((WCHAR *)p16.str, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    scratch_end(scratch);
    if (h == INVALID_HANDLE_VALUE) return result;
    LARGE_INTEGER pos;
    pos.QuadPart = (LONGLONG)offset;
    if (SetFilePointerEx(h, pos, 0, FILE_BEGIN)) {
        U8 *buf = push_array_nz(a, U8, size + 1);
        U64 total = 0;
        while (total < size) {
            DWORD got = 0;
            DWORD chunk = (DWORD)Min(size - total, MB(64));
            if (!ReadFile(h, buf + total, chunk, &got, 0) || got == 0) break;
            total += got;
        }
        buf[total] = 0;
        result = str8(buf, total);
    }
    CloseHandle(h);
    return result;
}

static String8 os_downloads_dir(Arena *a)
{
    // FOLDERID_Downloads {374DE290-123F-4565-9164-39C4925E467B}; defined here to avoid linking uuid.lib.
    static const GUID downloads = { 0x374de290, 0x123f, 0x4565, { 0x91, 0x64, 0x39, 0xc4, 0x92, 0x5e, 0x46, 0x7b } };
    PWSTR path = 0;
    String8 result = { 0 };
    if (SUCCEEDED(SHGetKnownFolderPath(&downloads, 0, 0, &path)) && path) result = os_str8_from_16(a, (U16 *)path, wcslen(path));
    if (path) CoTaskMemFree(path);
    return result;
}

static B32 os_write_file(String8 path, String8 data)
{
    Temp scratch = scratch_begin(0);
    String16 p16 = os_path16(scratch.arena, path);
    HANDLE h = CreateFileW((WCHAR *)p16.str, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    scratch_end(scratch);
    if (h == INVALID_HANDLE_VALUE) return 0;
    B32 ok = 1;
    U64 total = 0;
    while (total < data.size) {
        DWORD chunk = (DWORD)Min(data.size - total, MB(64));
        DWORD wrote = 0;
        if (!WriteFile(h, data.str + total, chunk, &wrote, 0) || wrote == 0) { ok = 0; break; }
        total += wrote;
    }
    CloseHandle(h);
    return ok;
}

static DWORD os_attributes(String8 path)
{
    Temp scratch = scratch_begin(0);
    String16 p16 = os_path16(scratch.arena, path);
    DWORD attr = GetFileAttributesW((WCHAR *)p16.str);
    scratch_end(scratch);
    return attr;
}

static B32 os_file_exists(String8 path) { DWORD a = os_attributes(path); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }
static B32 os_dir_exists(String8 path)  { DWORD a = os_attributes(path); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }

static OS_FileInfoArray os_list_dir(Arena *a, String8 path)
{
    OS_FileInfoArray result = { 0 };
    Temp scratch = scratch_begin(a);
    String8 pattern = path_join(scratch.arena, path, str8_lit("*"));
    String16 p16 = os_path16(scratch.arena, pattern);

    typedef struct Node { struct Node *next; OS_FileInfo info; } Node;
    Node *first = 0, *last = 0;
    U64 count = 0;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((WCHAR *)p16.str, FindExInfoBasic, &fd, FindExSearchNameMatch, 0, FIND_FIRST_EX_LARGE_FETCH);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            WCHAR *n = fd.cFileName;
            if (n[0] == '.' && (n[1] == 0 || (n[1] == '.' && n[2] == 0))) continue;
            Node *node = push_struct(scratch.arena, Node);
            node->info.name = os_str8_from_16(a, (U16 *)n, wcslen(n));
            node->info.is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            node->info.is_link = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
            node->info.size = ((U64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            U64 ft = ((U64)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
            node->info.mtime = ft >= 116444736000000000ull ? (ft - 116444736000000000ull) / 10000000ull : 0;
            if (last) last->next = node; else first = node;
            last = node;
            count += 1;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    result.v = push_array(a, OS_FileInfo, count);
    for (Node *n = first; n; n = n->next) result.v[result.count++] = n->info;
    scratch_end(scratch);
    return result;
}

static String8 os_get_env(Arena *a, char *name)
{
    Temp scratch = scratch_begin(a);
    String16 n16 = os_str16_from_8(scratch.arena, str8_cstring(name));
    WCHAR buf[4096];
    DWORD len = GetEnvironmentVariableW((WCHAR *)n16.str, buf, ArrayCount(buf));
    scratch_end(scratch);
    if (len == 0 || len >= ArrayCount(buf)) return str8(0, 0);
    return os_str8_from_16(a, (U16 *)buf, len);
}

static B32 os_make_dirs(String8 path)
{
    while (path.size && (path.str[path.size - 1] == '\\' || path.str[path.size - 1] == '/')) path.size -= 1;
    if (path.size <= 3) return os_dir_exists(path); // drive root
    Temp scratch = scratch_begin(0);
    String16 p16 = os_path16(scratch.arena, path);
    B32 ok = CreateDirectoryW((WCHAR *)p16.str, 0) != 0;
    if (!ok) {
        DWORD err = GetLastError();
        if (err == ERROR_ALREADY_EXISTS) ok = os_dir_exists(path);
        else if (err == ERROR_PATH_NOT_FOUND) ok = os_make_dirs(path_dirname(path)) && CreateDirectoryW((WCHAR *)p16.str, 0);
    }
    scratch_end(scratch);
    return ok;
}

static B32 os_move(String8 from, String8 to)
{
    Temp scratch = scratch_begin(0);
    String16 f = os_path16(scratch.arena, from);
    String16 t = os_path16(scratch.arena, to);
    // Antivirus scanners briefly hold freshly written files open, which blocks
    // renaming their folder; retry for up to ~3 seconds before giving up.
    B32 ok = 0;
    for (int attempt = 0; attempt < 30; attempt += 1) {
        if (MoveFileExW((WCHAR *)f.str, (WCHAR *)t.str, 0)) { ok = 1; break; }
        DWORD err = GetLastError();
        if (err != ERROR_ACCESS_DENIED && err != ERROR_SHARING_VIOLATION && err != ERROR_LOCK_VIOLATION) break;
        Sleep(100);
    }
    scratch_end(scratch);
    return ok;
}

static B32 os_replace_file(String8 from, String8 to)
{
    Temp scratch = scratch_begin(0);
    String16 f = os_path16(scratch.arena, from);
    String16 t = os_path16(scratch.arena, to);
    B32 ok = MoveFileExW((WCHAR *)f.str, (WCHAR *)t.str, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    scratch_end(scratch);
    return ok;
}

static B32 os_is_link(String8 path)
{
    DWORD a = os_attributes(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
}

static B32 os_delete_tree(String8 path)
{
    Temp scratch = scratch_begin(0);
    String16 p16 = os_path16(scratch.arena, path);
    B32 ok = 1;
    DWORD attr = GetFileAttributesW((WCHAR *)p16.str);
    if (attr == INVALID_FILE_ATTRIBUTES) { scratch_end(scratch); return 1; }
    if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        SetFileAttributesW((WCHAR *)p16.str, FILE_ATTRIBUTE_NORMAL);
        ok = DeleteFileW((WCHAR *)p16.str) != 0;
    } else {
        // A junction/symlink: remove the link itself, never what it points to.
        if (!(attr & FILE_ATTRIBUTE_REPARSE_POINT)) {
            OS_FileInfoArray files = os_list_dir(scratch.arena, path);
            for (U64 i = 0; i < files.count; i += 1) {
                ok &= os_delete_tree(path_join(scratch.arena, path, files.v[i].name));
            }
        }
        ok &= RemoveDirectoryW((WCHAR *)p16.str) != 0;
    }
    scratch_end(scratch);
    return ok;
}

static B32 os_recycle(String8 path)
{
    if (os_is_link(path)) return os_delete_tree(path); // only removes the link
    Temp scratch = scratch_begin(0);
    String16 p16 = os_str16_from_8(scratch.arena, path); // the shell API doesn't take \\?\ paths
    WCHAR *from = push_array(scratch.arena, WCHAR, p16.size + 2); // double-null terminated
    memcpy(from, p16.str, p16.size * sizeof(WCHAR));
    SHFILEOPSTRUCTW op = { 0 };
    op.wFunc = FO_DELETE;
    op.pFrom = from;
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    B32 ok = SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
    scratch_end(scratch);
    return ok;
}

static B32 os_file_version(String8 exe, U32 out[4])
{
    Temp scratch = scratch_begin(0);
    String16 p16 = os_str16_from_8(scratch.arena, exe);
    B32 ok = 0;
    DWORD handle = 0;
    DWORD size = GetFileVersionInfoSizeW((WCHAR *)p16.str, &handle);
    if (size) {
        void *buf = push_array(scratch.arena, U8, size);
        if (GetFileVersionInfoW((WCHAR *)p16.str, 0, size, buf)) {
            // Prefer the FileVersion string ("1.60.1.70862"): Blizzard packs the numeric
            // VS_FIXEDFILEINFO differently (1.60.1 shows up as 160.1.7086).
            struct { WORD lang, codepage; } *tr = 0;
            UINT len = 0;
            if (VerQueryValueW(buf, L"\\VarFileInfo\\Translation", (void **)&tr, &len) && tr && len >= 4) {
                WCHAR key[64];
                swprintf(key, 64, L"\\StringFileInfo\\%04x%04x\\FileVersion", tr->lang, tr->codepage);
                WCHAR *text = 0;
                if (VerQueryValueW(buf, key, (void **)&text, &len) && text && len) {
                    U32 part = 0;
                    out[0] = out[1] = out[2] = out[3] = 0;
                    for (WCHAR *c = text; *c && part < 4; c += 1) {
                        if (*c >= '0' && *c <= '9') out[part] = out[part] * 10 + (U32)(*c - '0');
                        else if (*c == '.' || *c == ',') part += 1;
                        else if (*c != ' ') break;
                    }
                    ok = part >= 2;
                }
            }
            VS_FIXEDFILEINFO *fi = 0;
            if (!ok && VerQueryValueW(buf, L"\\", (void **)&fi, &len) && fi) {
                out[0] = HIWORD(fi->dwFileVersionMS); out[1] = LOWORD(fi->dwFileVersionMS);
                out[2] = HIWORD(fi->dwFileVersionLS); out[3] = LOWORD(fi->dwFileVersionLS);
                ok = 1;
            }
        }
    }
    scratch_end(scratch);
    return ok;
}

static B32 os_tree_has_links(String8 path)
{
    Temp scratch = scratch_begin(0);
    B32 found = 0;
    OS_FileInfoArray files = os_list_dir(scratch.arena, path);
    for (U64 i = 0; i < files.count && !found; i += 1) {
        if (files.v[i].is_link) found = 1;
        else if (files.v[i].is_dir) found = os_tree_has_links(path_join(scratch.arena, path, files.v[i].name));
    }
    scratch_end(scratch);
    return found;
}

static String8 os_system_dir(Arena *a)
{
    WCHAR buf[MAX_PATH];
    UINT n = GetSystemDirectoryW(buf, ArrayCount(buf));
    if (!n || n >= ArrayCount(buf)) return str8(0, 0);
    return os_str8_from_16(a, (U16 *)buf, n);
}

static OS_RunResult os_run(Arena *a, String8 exe, String8 cmdline, U32 timeout_ms)
{
    OS_RunResult result = { 0 };
    Temp scratch = scratch_begin(a);
    SECURITY_ATTRIBUTES sa = { sizeof(sa), 0, TRUE };
    HANDLE out_read = 0, out_write = 0;
    HANDLE in_null = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, 0);
    if (!CreatePipe(&out_read, &out_write, &sa, 0)) { if (in_null != INVALID_HANDLE_VALUE) CloseHandle(in_null); goto done; }
    SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_null;
    si.hStdOutput = out_write;
    si.hStdError = out_write;
    PROCESS_INFORMATION pi = { 0 };
    String16 exe16 = os_str16_from_8(scratch.arena, exe);
    String16 cmd16 = os_str16_from_8(scratch.arena, cmdline); // CreateProcessW may modify it: it's our copy
    BOOL ok = CreateProcessW((WCHAR *)exe16.str, (WCHAR *)cmd16.str, 0, 0, TRUE, CREATE_NO_WINDOW, 0, 0, &si, &pi);
    CloseHandle(out_write);
    if (in_null != INVALID_HANDLE_VALUE) CloseHandle(in_null);
    if (!ok) { CloseHandle(out_read); goto done; }
    result.started = 1;

    String8List chunks = { 0 };
    U64 start = GetTickCount64();
    for (;;) {
        DWORD avail = 0;
        while (PeekNamedPipe(out_read, 0, 0, 0, &avail, 0) && avail) {
            U8 *buf = push_array_nz(scratch.arena, U8, avail);
            DWORD got = 0;
            if (!ReadFile(out_read, buf, avail, &got, 0) || !got) break;
            if (chunks.total_size < MB(1)) str8_list_push(scratch.arena, &chunks, str8(buf, got));
        }
        if (WaitForSingleObject(pi.hProcess, 15) == WAIT_OBJECT_0) {
            // Drain whatever is left after exit.
            while (PeekNamedPipe(out_read, 0, 0, 0, &avail, 0) && avail) {
                U8 *buf = push_array_nz(scratch.arena, U8, avail);
                DWORD got = 0;
                if (!ReadFile(out_read, buf, avail, &got, 0) || !got) break;
                if (chunks.total_size < MB(1)) str8_list_push(scratch.arena, &chunks, str8(buf, got));
            }
            break;
        }
        if (GetTickCount64() - start > timeout_ms) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 2000);
            result.timed_out = 1;
            break;
        }
    }
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    result.exit_code = code;
    result.output = str8_list_join(a, &chunks, str8_lit(""));
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(out_read);
done:
    scratch_end(scratch);
    return result;
}

static U64 os_now_us(void)
{
    static LARGE_INTEGER freq;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (U64)(now.QuadPart * 1000000 / freq.QuadPart);
}

typedef struct OS_ThreadStart { OS_ThreadFunc *fn; void *param; } OS_ThreadStart;

static DWORD WINAPI os_thread_entry(void *p)
{
    OS_ThreadStart start = *(OS_ThreadStart *)p;
    HeapFree(GetProcessHeap(), 0, p);
    U32 r = start.fn(start.param);
    scratch_release_thread();
    return r;
}

typedef struct OS_ParallelCtx { OS_ParallelFunc *fn; void *user; U64 count; volatile LONG64 next; } OS_ParallelCtx;

static DWORD WINAPI os_parallel_entry(void *p)
{
    OS_ParallelCtx *ctx = (OS_ParallelCtx *)p;
    for (;;) {
        U64 i = (U64)InterlockedIncrement64(&ctx->next) - 1;
        if (i >= ctx->count) break;
        ctx->fn(ctx->user, i);
    }
    scratch_release_thread();
    return 0;
}

// fn(user, i) for i in [0, count) on up to max_threads threads (at most 32); returns when all are done.
static void os_parallel_for_threads(U64 count, OS_ParallelFunc *fn, void *user, U64 max_threads)
{
    U64 threads = Min(Min(max_threads, 32), count);
    if (threads <= 1) { for (U64 i = 0; i < count; i += 1) fn(user, i); return; }
    OS_ParallelCtx ctx = { fn, user, count, 0 };
    HANDLE handles[32];
    U32 started = 0;
    for (U64 t = 0; t < threads; t += 1) {
        HANDLE h = CreateThread(0, 0, os_parallel_entry, &ctx, 0, 0);
        if (h) handles[started++] = h;
    }
    if (!started) { for (U64 i = 0; i < count; i += 1) fn(user, i); return; }
    WaitForMultipleObjects(started, handles, TRUE, INFINITE);
    for (U32 t = 0; t < started; t += 1) CloseHandle(handles[t]);
}

// The same on as many threads as there are processors (at most 8): for work that uses the CPU.
// Work that mostly waits on the network wants more (os_parallel_for_threads).
static void os_parallel_for(U64 count, OS_ParallelFunc *fn, void *user)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    os_parallel_for_threads(count, fn, user, Min((U64)Max(si.dwNumberOfProcessors, 1), 8));
}

static void os_thread_start(OS_ThreadFunc *fn, void *param)
{
    OS_ThreadStart *s = (OS_ThreadStart *)HeapAlloc(GetProcessHeap(), 0, sizeof(OS_ThreadStart));
    s->fn = fn;
    s->param = param;
    HANDLE h = CreateThread(0, 0, os_thread_entry, s, 0, 0);
    if (h) CloseHandle(h);
}

static String8 os_dpapi(Arena *a, String8 in, B32 protect)
{
    DATA_BLOB src = { (DWORD)in.size, in.str }, dst = { 0 };
    BOOL ok = protect ? CryptProtectData(&src, L"wam", 0, 0, 0, CRYPTPROTECT_UI_FORBIDDEN, &dst)
                      : CryptUnprotectData(&src, 0, 0, 0, 0, CRYPTPROTECT_UI_FORBIDDEN, &dst);
    String8 r = { 0 };
    if (ok && dst.pbData) {
        r.str = push_array_nz(a, U8, dst.cbData + 1);
        memcpy(r.str, dst.pbData, dst.cbData);
        r.str[dst.cbData] = 0;
        r.size = dst.cbData;
        SecureZeroMemory(dst.pbData, dst.cbData);
    }
    if (dst.pbData) LocalFree(dst.pbData);
    return r;
}

static String8 os_protect(Arena *a, String8 data) { return os_dpapi(a, data, 1); }
static String8 os_unprotect(Arena *a, String8 blob) { return blob.size ? os_dpapi(a, blob, 0) : str8(0, 0); }

// InstallLocation of every program in Windows' uninstall list whose DisplayName starts with
// name_prefix (64- and 32-bit views, machine and user). Read-only.
static String8List os_installed_program_dirs(Arena *a, char *name_prefix)
{
    String8List result = { 0 };
    Temp scratch = scratch_begin(a);
    String8 prefix = str8_cstring(name_prefix);
    struct { HKEY root; REGSAM view; } hives[] = {
        { HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY }, { HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY }, { HKEY_CURRENT_USER, 0 },
    };
    for (int h = 0; h < (int)ArrayCount(hives); h += 1) {
        HKEY base;
        if (RegOpenKeyExW(hives[h].root, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", 0,
                          KEY_READ | hives[h].view, &base) != ERROR_SUCCESS) continue;
        for (DWORD i = 0;; i += 1) {
            WCHAR sub[256];
            DWORD sub_len = ArrayCount(sub);
            if (RegEnumKeyExW(base, i, sub, &sub_len, 0, 0, 0, 0) != ERROR_SUCCESS) break;
            WCHAR name[512], loc[1024];
            DWORD name_bytes = sizeof(name) - sizeof(WCHAR), loc_bytes = sizeof(loc) - sizeof(WCHAR);
            if (RegGetValueW(base, sub, L"DisplayName", RRF_RT_REG_SZ, 0, name, &name_bytes) != ERROR_SUCCESS) continue;
            String8 display = os_str8_from_16(scratch.arena, (U16 *)name, wcslen(name));
            if (!str8_starts_with(display, prefix, 1)) continue;
            if (RegGetValueW(base, sub, L"InstallLocation", RRF_RT_REG_SZ, 0, loc, &loc_bytes) != ERROR_SUCCESS) continue;
            if (loc[0]) str8_list_push(a, &result, os_str8_from_16(a, (U16 *)loc, wcslen(loc)));
        }
        RegCloseKey(base);
    }
    scratch_end(scratch);
    return result;
}
