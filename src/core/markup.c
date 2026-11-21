// markup.c - addon descriptions and changelogs as plain text: paragraphs and bullet lines.
//
// CurseForge sends HTML, WoWInterface BBCode (and sometimes Markdown), Tukui Markdown. The GUI
// draws plain wrapped text, so all of them become: lines separated by '\n', blank lines between
// paragraphs, "â€¢ " before list items. Links keep their text, images and scripts disappear.

typedef enum MarkupKind { MARKUP_HTML, MARKUP_BBCODE, MARKUP_MARKDOWN } MarkupKind;

typedef struct MarkupOut { U8 *buf; U64 n, cap; B32 space; } MarkupOut;

static void tout_byte(MarkupOut *o, U8 c) { if (o->n < o->cap) o->buf[o->n++] = c; }

// Text with HTML's whitespace rules: runs of spaces and newlines become one space.
static void tout_text(MarkupOut *o, U8 c)
{
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { o->space = 1; return; }
    if (o->space && o->n && o->buf[o->n - 1] != '\n') tout_byte(o, ' ');
    o->space = 0;
    tout_byte(o, c);
}

// Ends the line (count 1) or the paragraph (count 2), never more than one blank line.
static void tout_break(MarkupOut *o, U32 count)
{
    o->space = 0;
    while (o->n && o->buf[o->n - 1] == ' ') o->n -= 1;
    if (!o->n) return;
    U32 have = 0;
    for (U64 i = o->n; i > 0 && o->buf[i - 1] == '\n' && have < 2; i -= 1) have += 1;
    while (have < count) { tout_byte(o, '\n'); have += 1; }
}

static void tout_str(MarkupOut *o, char *s) { while (*s) tout_byte(o, (U8)*s++); o->space = 0; }

static void tout_utf8(MarkupOut *o, U32 cp)
{
    if (cp == 0xA0) cp = ' ';
    if (cp < 0x80) { tout_text(o, (U8)cp); return; }
    if (o->space && o->n && o->buf[o->n - 1] != '\n') tout_byte(o, ' ');
    o->space = 0;
    if (cp < 0x800) { tout_byte(o, (U8)(0xC0 | (cp >> 6))); tout_byte(o, (U8)(0x80 | (cp & 0x3F))); }
    else if (cp < 0x10000) { tout_byte(o, (U8)(0xE0 | (cp >> 12))); tout_byte(o, (U8)(0x80 | ((cp >> 6) & 0x3F))); tout_byte(o, (U8)(0x80 | (cp & 0x3F))); }
    else { tout_byte(o, (U8)(0xF0 | (cp >> 18))); tout_byte(o, (U8)(0x80 | ((cp >> 12) & 0x3F))); tout_byte(o, (U8)(0x80 | ((cp >> 6) & 0x3F))); tout_byte(o, (U8)(0x80 | (cp & 0x3F))); }
}

// "&amp;" at s[i] -> its character; returns the length consumed (0 if not an entity).
static U64 markup_entity(String8 s, U64 i, U32 *cp)
{
    static const struct { char *name; U32 cp; } names[] = {
        { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' }, { "nbsp", 0xA0 },
        { "mdash", 0x2014 }, { "ndash", 0x2013 }, { "hellip", 0x2026 }, { "rsquo", 0x2019 }, { "lsquo", 0x2018 },
        { "rdquo", 0x201D }, { "ldquo", 0x201C }, { "bull", 0x2022 }, { "copy", 0xA9 }, { "trade", 0x2122 },
    };
    U64 end = i + 1;
    while (end < s.size && end - i < 12 && s.str[end] != ';') end += 1;
    if (end >= s.size || s.str[end] != ';') return 0;
    String8 name = str8_substr(s, i + 1, end);
    if (name.size > 1 && name.str[0] == '#') {
        B32 hex = name.str[1] == 'x' || name.str[1] == 'X';
        U32 v = 0;
        for (U64 k = hex ? 2 : 1; k < name.size; k += 1) {
            U8 c = name.str[k];
            U32 d = c >= '0' && c <= '9' ? c - '0' : hex && (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : 99;
            if (d == 99) return 0;
            v = v * (hex ? 16 : 10) + d;
        }
        *cp = v && v < 0x110000 ? v : '?';
        return end - i + 1;
    }
    for (U32 k = 0; k < ArrayCount(names); k += 1) if (str8_match(name, str8_cstring(names[k].name), 0)) { *cp = names[k].cp; return end - i + 1; }
    return 0;
}

static B32 markup_tag_is(String8 tag, char *name)
{
    String8 n = str8_cstring(name);
    return tag.size >= n.size && str8_match(str8_prefix(tag, n.size), n, 1) && (tag.size == n.size || !((tag.str[n.size] | 32) >= 'a' && (tag.str[n.size] | 32) <= 'z'));
}

static void markup_html(MarkupOut *o, String8 s)
{
    for (U64 i = 0; i < s.size;) {
        U8 c = s.str[i];
        if (c == '<') {
            U64 end = str8_find(s, i, str8_lit(">"), 0);
            if (end >= s.size) break;
            String8 tag = str8_substr(s, i + 1, end);
            B32 closing = tag.size && tag.str[0] == '/';
            if (closing) tag = str8_skip(tag, 1);
            if (tag.size && tag.str[0] == '!') { // comment
                U64 ce = str8_find(s, i, str8_lit("-->"), 0);
                i = str8_match(str8_prefix(tag, 3), str8_lit("!--"), 0) && ce < s.size ? ce + 3 : end + 1;
                continue;
            }
            if (!closing && (markup_tag_is(tag, "script") || markup_tag_is(tag, "style"))) {
                U64 ce = str8_find(s, end, markup_tag_is(tag, "script") ? str8_lit("</script") : str8_lit("</style"), 1);
                i = ce < s.size ? ce : s.size;
                continue;
            }
            if (markup_tag_is(tag, "br")) tout_break(o, 1);
            else if (markup_tag_is(tag, "li")) { tout_break(o, 1); if (!closing) tout_str(o, "\xE2\x80\xA2 "); }
            else if (markup_tag_is(tag, "p") || markup_tag_is(tag, "div") || markup_tag_is(tag, "ul") || markup_tag_is(tag, "ol")
                  || markup_tag_is(tag, "table") || markup_tag_is(tag, "blockquote") || markup_tag_is(tag, "pre") || markup_tag_is(tag, "hr")
                  || (tag.size == 2 && (tag.str[0] | 32) == 'h' && tag.str[1] >= '1' && tag.str[1] <= '6')
                  || (tag.size > 2 && (tag.str[0] | 32) == 'h' && tag.str[1] >= '1' && tag.str[1] <= '6' && tag.str[2] == ' ')) tout_break(o, 2);
            else if (markup_tag_is(tag, "tr")) tout_break(o, 1);
            else if (markup_tag_is(tag, "td") || markup_tag_is(tag, "th")) o->space = 1;
            i = end + 1;
            continue;
        }
        if (c == '&') {
            U32 cp = 0;
            U64 len = markup_entity(s, i, &cp);
            if (len) { tout_utf8(o, cp); i += len; continue; }
        }
        tout_text(o, c);
        i += 1;
    }
}

// Line-based: Markdown headings, bullets and emphasis; BBCode tags. Newlines are kept.
static void markup_lines(MarkupOut *o, String8 s, B32 bbcode)
{
    B32 in_code = 0;
    while (s.size) {
        String8 line = str8_pop_line(&s);
        while (line.size && (line.str[line.size - 1] == '\r' || line.str[line.size - 1] == ' ')) line.size -= 1;
        String8 t = str8_trim(line);
        if (str8_starts_with(t, str8_lit("```"), 0)) { in_code = !in_code; continue; }
        if (!t.size) { tout_break(o, 2); continue; }
        if (!in_code) {
            U64 hashes = 0;
            while (hashes < t.size && t.str[hashes] == '#') hashes += 1;
            if (hashes && hashes < t.size && t.str[hashes] == ' ') { tout_break(o, 2); t = str8_trim(str8_skip(t, hashes)); }
            else if (t.size > 1 && (t.str[0] == '-' || t.str[0] == '*' || t.str[0] == '+') && t.str[1] == ' ') {
                tout_break(o, 1); tout_str(o, "\xE2\x80\xA2 "); t = str8_trim(str8_skip(t, 2));
            } else if (str8_match(t, str8_lit("---"), 0) || str8_match(t, str8_lit("***"), 0)) { tout_break(o, 2); continue; }
        }
        for (U64 i = 0; i < t.size;) {
            U8 c = t.str[i];
            if (bbcode && c == '[') {
                U64 end = str8_find(t, i, str8_lit("]"), 0);
                String8 tag = end < t.size ? str8_substr(t, i + 1, end) : str8(0, 0);
                B32 closing = tag.size && tag.str[0] == '/';
                String8 name = closing ? str8_skip(tag, 1) : tag;
                if (name.size && name.size < 24 && (name.str[0] | 32) >= 'a' && (name.str[0] | 32) <= 'z') {
                    if (markup_tag_is(name, "img")) { // drop the image and its URL
                        U64 ce = str8_find(t, end, str8_lit("[/img]"), 1);
                        i = ce < t.size ? ce + 6 : t.size;
                        continue;
                    }
                    if (!closing && markup_tag_is(name, "list")) tout_break(o, 1);
                    i = end + 1;
                    continue;
                }
                if (str8_match(tag, str8_lit("*"), 0)) { tout_break(o, 1); tout_str(o, "\xE2\x80\xA2 "); i = end + 1; continue; }
            }
            if (!in_code && (c == '*' || c == '_') && i + 1 < t.size && t.str[i + 1] == c) { i += 2; continue; } // **bold**
            if (!in_code && c == '`') { i += 1; continue; }
            if (!in_code && !bbcode && c == '!' && i + 1 < t.size && t.str[i + 1] == '[') { // ![alt](url): images are dropped
                U64 close = str8_find(t, i, str8_lit("]("), 0), paren = close < t.size ? str8_find(t, close, str8_lit(")"), 0) : t.size;
                if (close < t.size && paren < t.size) { i = paren + 1; continue; }
            }
            if (!in_code && !bbcode && c == '[' && i + 2 < t.size && t.str[i + 1] == '!' && t.str[i + 2] == '[') { // [![alt](img)](url): a linked image, dropped
                U64 close = str8_find(t, i, str8_lit("]("), 0), paren = close < t.size ? str8_find(t, close, str8_lit(")"), 0) : t.size;
                if (paren + 2 < t.size && t.str[paren + 1] == ']' && t.str[paren + 2] == '(') {
                    U64 end = str8_find(t, paren + 2, str8_lit(")"), 0);
                    if (end < t.size) { i = end + 1; continue; }
                }
            }
            if (!in_code && c == '[' && !bbcode) { // [text](url) -> text
                U64 close = str8_find(t, i, str8_lit("]("), 0), paren = close < t.size ? str8_find(t, close, str8_lit(")"), 0) : t.size;
                if (close < t.size && paren < t.size) {
                    for (U64 k = i + 1; k < close; k += 1) tout_byte(o, t.str[k]);
                    i = paren + 1;
                    continue;
                }
            }
            if (c == '&') {
                U32 cp = 0;
                U64 len = markup_entity(t, i, &cp);
                if (len) { tout_utf8(o, cp); i += len; continue; }
            }
            tout_byte(o, c == '\t' ? ' ' : c);
            o->space = 0;
            i += 1;
        }
        tout_break(o, 1);
    }
}

static String8 markup_to_text(Arena *a, String8 s, MarkupKind kind)
{
    MarkupOut o = { 0 };
    o.cap = s.size * 2 + 16;
    o.buf = push_array_nz(a, U8, o.cap);
    if (kind == MARKUP_HTML) markup_html(&o, s);
    else markup_lines(&o, s, kind == MARKUP_BBCODE);
    while (o.n && (o.buf[o.n - 1] == '\n' || o.buf[o.n - 1] == ' ')) o.n -= 1;
    U64 start = 0;
    while (start < o.n && (o.buf[start] == '\n' || o.buf[start] == ' ')) start += 1;
    return str8(o.buf + start, o.n - start);
}

// Plain text that's really Markdown: **bold**, [text](link), or a "# Heading" line.
static B32 markup_looks_markdown(String8 t)
{
    if (str8_find(t, 0, str8_lit("**"), 0) < t.size || str8_find(t, 0, str8_lit("]("), 0) < t.size) return 1;
    for (String8 rest = t; rest.size;) {
        String8 line = str8_pop_line(&rest);
        if (str8_starts_with(line, str8_lit("# "), 0) || str8_starts_with(line, str8_lit("## "), 0) || str8_starts_with(line, str8_lit("### "), 0)) return 1;
    }
    return 0;
}

// CurseForge's HTML, which sometimes wraps Markdown the author pasted (shown as is on its site's
// HTML view): the HTML pass, then the Markdown one when what's left still looks like Markdown.
static String8 markup_cf_html(Arena *a, String8 s)
{
    String8 t = markup_to_text(a, s, MARKUP_HTML);
    return markup_looks_markdown(t) ? markup_to_text(a, t, MARKUP_MARKDOWN) : t;
}
