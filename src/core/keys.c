// keys.c - the user's own API keys (Wago, CurseForge), as entered in Settings: one file each in
// %APPDATA%\wam, encrypted for the current Windows user (DPAPI).

static String8 key_path(Arena *a, char *file)
{
    String8 appdata = os_get_env(a, "APPDATA");
    if (!appdata.size) return str8(0, 0);
    return path_join(a, path_join(a, appdata, str8_lit("wam")), str8_cstring(file));
}

// Printable ASCII without spaces, so it can't break out of the header it's sent in.
static B32 key_looks_valid(String8 key)
{
    if (key.size <= 20 || key.size > 512) return 0;
    for (U64 i = 0; i < key.size; i += 1) if (key.str[i] <= ' ' || key.str[i] > '~') return 0;
    return 1;
}

// The saved key; empty without one (or if another Windows user saved it).
static String8 key_load(Arena *a, char *file)
{
    String8 path = key_path(a, file);
    return path.size ? str8_trim(os_unprotect(a, os_read_file(a, path))) : str8(0, 0);
}

// Saves the key; an empty key deletes it.
static B32 key_save(char *file, String8 key)
{
    Temp scratch = scratch_begin(0);
    String8 path = key_path(scratch.arena, file);
    B32 ok = 0;
    if (path.size) {
        if (!key.size) {
            ok = !os_file_exists(path) || DeleteFileW((WCHAR *)os_path16(scratch.arena, path).str);
        } else {
            String8 blob = os_protect(scratch.arena, key);
            ok = blob.size && os_make_dirs(path_dirname(path)) && os_write_file(path, blob);
        }
    }
    scratch_end(scratch);
    return ok;
}
