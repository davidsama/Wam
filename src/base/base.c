// base.c

//- Arena

static Arena *arena_alloc(U64 reserve)
{
    reserve = AlignPow2(reserve, KB(64));
    U8 *mem = (U8 *)os_reserve(reserve);
    if (!mem) { fprintf(stderr, "fatal: could not reserve %llu bytes\n", reserve); abort(); }
    U64 commit = KB(64);
    os_commit(mem, commit);
    Arena *a = (Arena *)mem;
    a->base = mem;
    a->reserved = reserve;
    a->committed = commit;
    a->pos = AlignPow2(sizeof(Arena), 16);
    return a;
}

static void arena_release(Arena *a) { os_release(a->base, a->reserved); }

static void *arena_push(Arena *a, U64 size, B32 zero)
{
    U64 start = AlignPow2(a->pos, 16);
    U64 end = start + size;
    if (end > a->reserved) { fprintf(stderr, "fatal: arena out of reserve\n"); abort(); }
    if (end > a->committed) {
        U64 new_commit = Min(AlignPow2(end, MB(1)), a->reserved);
        os_commit(a->base + a->committed, new_commit - a->committed);
        a->committed = new_commit;
    }
    a->pos = end;
    void *result = a->base + start;
    if (zero) memset(result, 0, size);
    return result;
}

static void arena_pop_to(Arena *a, U64 pos)
{
    U64 min_pos = AlignPow2(sizeof(Arena), 16);
    a->pos = Max(pos, min_pos);
}

static void arena_clear(Arena *a) { arena_pop_to(a, 0); }
static Temp temp_begin(Arena *a) { Temp t = { a, a->pos }; return t; }
static void temp_end(Temp t) { arena_pop_to(t.arena, t.pos); }

thread_static Arena *tl_scratch[2];

static Temp scratch_begin(Arena *conflict)
{
    for (int i = 0; i < 2; i += 1) {
        if (!tl_scratch[i]) tl_scratch[i] = arena_alloc(GB(1));
        if (tl_scratch[i] != conflict) return temp_begin(tl_scratch[i]);
    }
    return temp_begin(tl_scratch[0]); // unreachable
}

static void scratch_release_thread(void)
{
    for (int i = 0; i < 2; i += 1) {
        if (tl_scratch[i]) { arena_release(tl_scratch[i]); tl_scratch[i] = 0; }
    }
}

//- Characters

static U8 char_lower(U8 c) { return (c >= 'A' && c <= 'Z') ? (U8)(c + 32) : c; }
static B32 char_is_space(U8 c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }

//- Strings

static String8 str8(U8 *s, U64 n) { String8 r = { s, n }; return r; }
static String8 str8_cstring(char *c) { return str8((U8 *)c, c ? strlen(c) : 0); }

static String8 str8_copy(Arena *a, String8 s)
{
    U8 *m = push_array_nz(a, U8, s.size + 1);
    if (s.size) memcpy(m, s.str, s.size);
    m[s.size] = 0;
    return str8(m, s.size);
}

static String8 str8_pushfv(Arena *a, char *fmt, va_list args)
{
    va_list args2;
    va_copy(args2, args);
    int n = vsnprintf(0, 0, fmt, args2);
    va_end(args2);
    if (n < 0) n = 0;
    U8 *buf = push_array_nz(a, U8, (U64)n + 1);
    vsnprintf((char *)buf, (size_t)n + 1, fmt, args);
    return str8(buf, (U64)n);
}

static String8 str8_pushf(Arena *a, char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    String8 r = str8_pushfv(a, fmt, args);
    va_end(args);
    return r;
}

static String8 str8_prefix(String8 s, U64 n) { s.size = Min(n, s.size); return s; }
static String8 str8_skip(String8 s, U64 n) { n = Min(n, s.size); s.str += n; s.size -= n; return s; }
static String8 str8_chop(String8 s, U64 n) { s.size -= Min(n, s.size); return s; }

static String8 str8_substr(String8 s, U64 lo, U64 hi)
{
    hi = Min(hi, s.size);
    lo = Min(lo, hi);
    return str8(s.str + lo, hi - lo);
}

static String8 str8_trim(String8 s)
{
    while (s.size && char_is_space(s.str[0])) { s.str += 1; s.size -= 1; }
    while (s.size && char_is_space(s.str[s.size - 1])) { s.size -= 1; }
    return s;
}

static String8 str8_lower(Arena *a, String8 s)
{
    String8 r = str8_copy(a, s);
    for (U64 i = 0; i < r.size; i += 1) r.str[i] = char_lower(r.str[i]);
    return r;
}

static B32 str8_match(String8 a, String8 b, B32 ci)
{
    if (a.size != b.size) return 0;
    if (!ci) return a.size == 0 || memcmp(a.str, b.str, a.size) == 0;
    for (U64 i = 0; i < a.size; i += 1) {
        if (char_lower(a.str[i]) != char_lower(b.str[i])) return 0;
    }
    return 1;
}

static B32 str8_starts_with(String8 s, String8 p, B32 ci) { return s.size >= p.size && str8_match(str8_prefix(s, p.size), p, ci); }
static B32 str8_ends_with(String8 s, String8 p, B32 ci) { return s.size >= p.size && str8_match(str8_skip(s, s.size - p.size), p, ci); }

static U64 str8_find(String8 hay, U64 start, String8 needle, B32 ci)
{
    if (needle.size == 0 || needle.size > hay.size) return hay.size;
    for (U64 i = start; i + needle.size <= hay.size; i += 1) {
        if (str8_match(str8(hay.str + i, needle.size), needle, ci)) return i;
    }
    return hay.size;
}

static String8 str8_pop_line(String8 *rest)
{
    U64 i = 0;
    while (i < rest->size && rest->str[i] != '\n') i += 1;
    String8 line = str8(rest->str, i);
    *rest = str8_skip(*rest, i + 1);
    if (line.size && line.str[line.size - 1] == '\r') line.size -= 1;
    return line;
}

static void str8_list_push(Arena *a, String8List *l, String8 s)
{
    String8Node *n = push_struct(a, String8Node);
    n->string = s;
    if (l->last) l->last->next = n; else l->first = n;
    l->last = n;
    l->count += 1;
    l->total_size += s.size;
}

static String8 str8_list_join(Arena *a, String8List *l, String8 sep)
{
    U64 size = l->total_size + (l->count ? (l->count - 1) * sep.size : 0);
    U8 *buf = push_array_nz(a, U8, size + 1);
    U8 *p = buf;
    for (String8Node *n = l->first; n; n = n->next) {
        if (n->string.size) memcpy(p, n->string.str, n->string.size);
        p += n->string.size;
        if (n->next && sep.size) { memcpy(p, sep.str, sep.size); p += sep.size; }
    }
    *p = 0;
    return str8(buf, size);
}

//- Paths

static String8 path_join(Arena *a, String8 dir, String8 name)
{
    B32 has_sep = dir.size && (dir.str[dir.size - 1] == '\\' || dir.str[dir.size - 1] == '/');
    return str8_pushf(a, "%.*s%s%.*s", S8F(dir), has_sep ? "" : "\\", S8F(name));
}

static String8 path_dirname(String8 path)
{
    for (U64 i = path.size; i > 0; i -= 1) {
        if (path.str[i - 1] == '\\' || path.str[i - 1] == '/') return str8_prefix(path, i - 1);
    }
    return str8(path.str, 0);
}

static String8 path_filename(String8 path)
{
    for (U64 i = path.size; i > 0; i -= 1) {
        if (path.str[i - 1] == '\\' || path.str[i - 1] == '/') return str8_skip(path, i);
    }
    return path;
}

static String8 path_ext(String8 path)
{
    String8 name = path_filename(path);
    for (U64 i = name.size; i > 0; i -= 1) {
        if (name.str[i - 1] == '.') return str8_skip(name, i - 1);
    }
    return str8(name.str + name.size, 0);
}

static String8 path_normalize_slashes(Arena *a, String8 path)
{
    String8 r = str8_copy(a, path);
    for (U64 i = 0; i < r.size; i += 1) if (r.str[i] == '/') r.str[i] = '\\';
    return r;
}

//- UTF-8

static UnicodeDecode utf8_decode(U8 *s, U64 max)
{
    UnicodeDecode r = { '?', 1 };
    if (max == 0) { r.advance = 0; return r; }
    U8 c = s[0];
    if (c < 0x80) { r.codepoint = c; return r; }
    U32 len = (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
    if (len == 0 || len > max) return r;
    U32 cp = c & (0x7F >> len);
    for (U32 i = 1; i < len; i += 1) {
        if ((s[i] & 0xC0) != 0x80) return r;
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    r.codepoint = cp;
    r.advance = len;
    return r;
}
