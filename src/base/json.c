// json.c - small JSON reader (tree in an arena) + string escaping for writing.
//
// Missing keys / wrong types return &json_nil, so lookups chain safely:
//   json_int(json_get(json_get(root, "data"), "id"))

typedef enum JSON_Type { JSON_NULL, JSON_BOOL, JSON_NUM, JSON_STR, JSON_ARR, JSON_OBJ } JSON_Type;

typedef struct JSON {
    JSON_Type type;
    String8 key;
    String8 str;
    F64 num;
    B32 b;
    struct JSON *first, *last, *next;
    U64 count;
} JSON;

static JSON json_nil;

#define json_each(it, arr) for (JSON *it = (arr)->first; it; it = it->next)

static U32 utf8_encode(U8 *out, U32 cp)
{
    if (cp < 0x80) { out[0] = (U8)cp; return 1; }
    if (cp < 0x800) { out[0] = (U8)(0xC0 | (cp >> 6)); out[1] = (U8)(0x80 | (cp & 63)); return 2; }
    if (cp < 0x10000) {
        out[0] = (U8)(0xE0 | (cp >> 12)); out[1] = (U8)(0x80 | ((cp >> 6) & 63)); out[2] = (U8)(0x80 | (cp & 63));
        return 3;
    }
    out[0] = (U8)(0xF0 | (cp >> 18)); out[1] = (U8)(0x80 | ((cp >> 12) & 63));
    out[2] = (U8)(0x80 | ((cp >> 6) & 63)); out[3] = (U8)(0x80 | (cp & 63));
    return 4;
}

typedef struct JSON_Parser { Arena *arena; U8 *p, *end; B32 err; } JSON_Parser;

static void jp_ws(JSON_Parser *p)
{
    while (p->p < p->end && (*p->p == ' ' || *p->p == '\t' || *p->p == '\r' || *p->p == '\n')) p->p += 1;
}

static int jp_hex4(JSON_Parser *p)
{
    if (p->end - p->p < 4) { p->err = 1; return 0; }
    int v = 0;
    for (int i = 0; i < 4; i += 1) {
        U8 c = *p->p++;
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else { p->err = 1; return 0; }
    }
    return v;
}

static String8 jp_string(JSON_Parser *p)
{
    p->p += 1; // opening quote
    U8 *start = p->p;
    B32 has_escape = 0;
    while (p->p < p->end && *p->p != '"') {
        if (*p->p == '\\') { has_escape = 1; p->p += 1; }
        p->p += 1;
    }
    if (p->p >= p->end) { p->err = 1; return str8(0, 0); }
    U8 *stop = p->p;
    p->p += 1; // closing quote
    if (!has_escape) return str8(start, (U64)(stop - start));

    U8 *out = push_array_nz(p->arena, U8, (U64)(stop - start) + 1);
    U64 n = 0;
    JSON_Parser sub = { p->arena, start, stop, 0 };
    while (sub.p < sub.end) {
        U8 c = *sub.p++;
        if (c != '\\') { out[n++] = c; continue; }
        if (sub.p >= sub.end) break;
        U8 e = *sub.p++;
        switch (e) {
            case 'n': out[n++] = '\n'; break;
            case 't': out[n++] = '\t'; break;
            case 'r': out[n++] = '\r'; break;
            case 'b': out[n++] = '\b'; break;
            case 'f': out[n++] = '\f'; break;
            case 'u': {
                U32 cp = (U32)jp_hex4(&sub);
                if (cp >= 0xD800 && cp <= 0xDBFF && sub.end - sub.p >= 6 && sub.p[0] == '\\' && sub.p[1] == 'u') {
                    sub.p += 2;
                    U32 lo = (U32)jp_hex4(&sub);
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                n += utf8_encode(out + n, cp); // \uXXXX (6 bytes) always encodes to <= 4
                break;
            }
            default: out[n++] = e; break; // \" \\ \/
        }
    }
    out[n] = 0;
    return str8(out, n);
}

static JSON *jp_value(JSON_Parser *p, int depth);

static void jp_append(JSON *parent, JSON *child)
{
    if (parent->last) parent->last->next = child; else parent->first = child;
    parent->last = child;
    parent->count += 1;
}

static JSON *jp_value(JSON_Parser *p, int depth)
{
    jp_ws(p);
    if (p->p >= p->end || depth > 64) { p->err = 1; return &json_nil; }
    JSON *v = push_struct(p->arena, JSON);
    U8 c = *p->p;
    if (c == '{') {
        v->type = JSON_OBJ;
        p->p += 1;
        jp_ws(p);
        if (p->p < p->end && *p->p == '}') { p->p += 1; return v; }
        for (;;) {
            jp_ws(p);
            if (p->p >= p->end || *p->p != '"') { p->err = 1; return v; }
            String8 key = jp_string(p);
            jp_ws(p);
            if (p->p >= p->end || *p->p != ':') { p->err = 1; return v; }
            p->p += 1;
            JSON *child = jp_value(p, depth + 1);
            if (p->err) return v;
            child->key = key;
            jp_append(v, child);
            jp_ws(p);
            if (p->p < p->end && *p->p == ',') { p->p += 1; continue; }
            if (p->p < p->end && *p->p == '}') { p->p += 1; return v; }
            p->err = 1;
            return v;
        }
    }
    if (c == '[') {
        v->type = JSON_ARR;
        p->p += 1;
        jp_ws(p);
        if (p->p < p->end && *p->p == ']') { p->p += 1; return v; }
        for (;;) {
            JSON *child = jp_value(p, depth + 1);
            if (p->err) return v;
            jp_append(v, child);
            jp_ws(p);
            if (p->p < p->end && *p->p == ',') { p->p += 1; continue; }
            if (p->p < p->end && *p->p == ']') { p->p += 1; return v; }
            p->err = 1;
            return v;
        }
    }
    if (c == '"') { v->type = JSON_STR; v->str = jp_string(p); return v; }
    if (p->end - p->p >= 4 && memcmp(p->p, "true", 4) == 0)  { v->type = JSON_BOOL; v->b = 1; p->p += 4; return v; }
    if (p->end - p->p >= 5 && memcmp(p->p, "false", 5) == 0) { v->type = JSON_BOOL; p->p += 5; return v; }
    if (p->end - p->p >= 4 && memcmp(p->p, "null", 4) == 0)  { v->type = JSON_NULL; p->p += 4; return v; }

    char buf[64];
    int n = 0;
    while (p->p < p->end && n < 63 && (strchr("+-.eE", *p->p) || (*p->p >= '0' && *p->p <= '9'))) buf[n++] = (char)*p->p++;
    buf[n] = 0;
    if (n == 0) { p->err = 1; return v; }
    v->type = JSON_NUM;
    v->num = strtod(buf, 0);
    return v;
}

static JSON *json_parse(Arena *a, String8 text)
{
    if (text.size >= 3 && text.str[0] == 0xEF && text.str[1] == 0xBB && text.str[2] == 0xBF) text = str8_skip(text, 3); // UTF-8 BOM
    JSON_Parser p = { a, text.str, text.str + text.size, 0 };
    if (!text.size) return &json_nil;
    JSON *root = jp_value(&p, 0);
    return p.err ? &json_nil : root;
}

static JSON *json_get(JSON *obj, char *key)
{
    if (obj->type != JSON_OBJ) return &json_nil;
    String8 k = str8_cstring(key);
    json_each(it, obj) if (str8_match(it->key, k, 0)) return it;
    return &json_nil;
}

static S64 str8_to_s64(String8 s)
{
    S64 v = 0;
    B32 neg = s.size && s.str[0] == '-';
    for (U64 i = neg; i < s.size && s.str[i] >= '0' && s.str[i] <= '9'; i += 1) v = v * 10 + (s.str[i] - '0');
    return neg ? -v : v;
}

static S64 json_int(JSON *v)
{
    if (v->type == JSON_NUM) return (S64)v->num;
    if (v->type == JSON_STR) return str8_to_s64(v->str);
    return 0;
}
static String8 json_str(JSON *v) { return v->type == JSON_STR ? v->str : str8(0, 0); }
static B32 json_bool(JSON *v, B32 fallback) { return v->type == JSON_BOOL ? v->b : fallback; }

// Escapes s for use inside a JSON string literal (without the quotes).
static String8 json_escape(Arena *a, String8 s)
{
    U8 *out = push_array_nz(a, U8, s.size * 6 + 1);
    U64 n = 0;
    for (U64 i = 0; i < s.size; i += 1) {
        U8 c = s.str[i];
        if (c == '"' || c == '\\') { out[n++] = '\\'; out[n++] = c; }
        else if (c == '\n') { out[n++] = '\\'; out[n++] = 'n'; }
        else if (c < 0x20) n += (U64)sprintf((char *)out + n, "\\u%04x", c);
        else out[n++] = c;
    }
    out[n] = 0;
    return str8(out, n);
}
