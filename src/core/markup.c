// markup.c - addon descriptions and changelogs as rich text: headings, paragraphs, lists, quotes, code,
// tables, with bold, italic, links and pictures in them.
//
// CurseForge sends HTML, WoWInterface BBCode (and sometimes Markdown), Wago, Tukui and GitHub Markdown
// (with HTML in it). All of them become a Rich: a list of blocks, each a list of spans of text in one
// style. gui/rich.c lays it out and draws it; rich_plain gives its text.
//
// The text is whatever an author wrote, so parsing is forgiving (a tag that isn't closed, a list in a
// table...), and bounded: looking ahead for a closing bracket and the like stops after RICH_LOOK bytes,
// and a text stops at the size it's parsed with. Only https:// links are kept (the rest are just text),
// and pictures are never loaded by themselves.

typedef enum MarkupKind { MARKUP_HTML, MARKUP_BBCODE, MARKUP_MARKDOWN } MarkupKind;

enum { RICH_BOLD = 1, RICH_ITALIC = 2, RICH_CODE = 4, RICH_STRIKE = 8, RICH_IMAGE = 16 };
typedef struct RichSpan {
    struct RichSpan *next;
    String8 text;             // '\n' in it: a line break
    String8 link;             // https:// or empty
    String8 image;            // RICH_IMAGE: the picture; text is its name
    U32 flags;
} RichSpan;

typedef enum RichKind { RICH_PARA, RICH_HEADING, RICH_ITEM, RICH_PRE, RICH_RULE, RICH_CELL } RichKind;
typedef struct RichBlock {
    struct RichBlock *next;
    RichKind kind;
    RichSpan *first, *last;
    B32 gap;                  // a paragraph's distance from the block above (else right under it)
    U32 level;                // headings: 1..6
    U32 indent;               // lists: 1 = the outermost; paragraphs in a list item have its indent
    U32 quote;                // quotes around it
    U32 number;               // items: 0 = a bullet
    U32 table, row, col, cols; // cells
} RichBlock;

typedef struct Rich { U64 id; RichBlock *first, *last; } Rich; // id: unique (gui/rich.c's layout cache goes by it)

#define RICH_LOOK 2000
#define RICH_LISTS 8
#define RICH_TEXT_MAX KB(100)

typedef struct RichBuild {
    Arena *a;
    Rich *doc;
    U8 *buf; U64 n, cap;      // the spans' text, one after the other
    RichBlock *cur;           // the block text goes to, 0 = the next text starts a paragraph
    U64 look;                 // the budget for looking ahead (rich_looked)
    B32 gap;                  // a paragraph's gap is due
    B32 pre, full;
    U32 bold, italic, strike, code, quote; // tags open
    U32 md;                   // Markdown emphasis
    String8 link;
    B32 cell, head;           // in a table's cell / heading cell
    U32 tables, row, col;
    struct { B32 ordered; U32 n; U64 column; } lists[RICH_LISTS];
    U32 nlists;
    B32 bb_code;
    String8 bb_url;
} RichBuild;

//- Building

static RichBlock *rich_block(RichBuild *b, RichKind kind)
{
    RichBlock *k = push_struct(b->a, RichBlock);
    k->kind = kind;
    k->gap = b->gap && b->doc->first;
    k->indent = b->nlists;
    k->quote = b->quote;
    b->gap = 0;
    if (b->doc->last) b->doc->last->next = k; else b->doc->first = k;
    b->doc->last = k;
    b->cur = k;
    return k;
}

static void rich_put(RichBuild *b, U8 *s, U64 n)
{
    if (!n || b->full) return;
    if (b->n + n > b->cap) { b->full = 1; return; }
    if (!b->cur) rich_block(b, RICH_PARA);
    U32 flags = b->md | (b->bold || b->head ? RICH_BOLD : 0) | (b->italic ? RICH_ITALIC : 0) | (b->strike ? RICH_STRIKE : 0) | (b->code ? RICH_CODE : 0);
    RichSpan *sp = b->cur->last;
    if (!sp || sp->flags != flags || sp->link.str != b->link.str || sp->text.str + sp->text.size != b->buf + b->n) {
        sp = push_struct(b->a, RichSpan);
        sp->flags = flags;
        sp->link = b->link;
        sp->text = str8(b->buf + b->n, 0);
        if (b->cur->last) b->cur->last->next = sp; else b->cur->first = sp;
        b->cur->last = sp;
    }
    memcpy(b->buf + b->n, s, n);
    b->n += n;
    sp->text.size += n;
}

static U8 rich_last_byte(RichBuild *b) // of the current block (0: it's empty)
{
    RichSpan *sp = b->cur ? b->cur->last : 0;
    return sp && sp->text.size ? sp->text.str[sp->text.size - 1] : 0;
}

static void rich_trim(RichBuild *b) // the current block's trailing spaces and line breaks
{
    RichSpan *sp = b->cur ? b->cur->last : 0;
    while (sp && sp->text.size && (sp->text.str[sp->text.size - 1] == ' ' || sp->text.str[sp->text.size - 1] == '\n')) sp->text.size -= 1;
}

static void rich_end(RichBuild *b) { rich_trim(b); b->cur = 0; b->pre = 0; }

// A line break inside the block.
static void rich_newline(RichBuild *b)
{
    if (!b->pre) while (rich_last_byte(b) == ' ') b->cur->last->text.size -= 1;
    U8 last = rich_last_byte(b);
    if (last && (last != '\n' || b->pre)) rich_put(b, (U8 *)"\n", 1);
}

// Where a block ends (and a gap starts, or not). An item that has no text yet goes on (<li><p>), and a
// table's cell is one block.
static void rich_break(RichBuild *b, B32 gap)
{
    if (b->cell) { rich_newline(b); return; }
    if (b->cur && b->cur->kind == RICH_ITEM && !b->cur->first) return;
    rich_end(b);
    if (gap) b->gap = 1;
}

// Text with HTML's whitespace rules: runs of spaces and newlines become one space.
static void rich_char(RichBuild *b, U8 c)
{
    if (b->pre) {
        if (c == '\r') return;
        if (c == '\n') { rich_newline(b); return; }
        rich_put(b, &c, 1);
        return;
    }
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { // (at once, so it has the style it's written in)
        U8 last = rich_last_byte(b);
        if (last && last != '\n' && last != ' ') rich_put(b, (U8 *)" ", 1);
        return;
    }
    rich_put(b, &c, 1);
}

static void rich_codepoint(RichBuild *b, U32 cp)
{
    if (cp == 0xA0) cp = ' ';
    if (cp == 0x200B || cp == 0xFEFF) return; // zero width
    if (cp < 0x80) { rich_char(b, cp < 0x20 && cp != '\n' && cp != '\t' ? ' ' : (U8)cp); return; }
    U8 u[4];
    U32 n = utf8_encode(u, cp >= 0xD800 && cp <= 0xDFFF ? '?' : cp);
    rich_put(b, u, n);
}

static void rich_item(RichBuild *b, U32 number)
{
    if (b->cell) { rich_newline(b); rich_put(b, (U8 *)"\xE2\x80\xA2 ", 4); return; }
    rich_end(b);
    RichBlock *k = rich_block(b, RICH_ITEM);
    if (!k->indent) k->indent = 1;
    k->number = number;
}

static void rich_rule(RichBuild *b)
{
    if (b->cell) return;
    rich_break(b, 1);
    rich_end(b);
    rich_block(b, RICH_RULE);
    b->cur = 0;
    b->gap = 1;
}

// "https://..." from what an attribute, [url=] or (url) says, or empty.
static int rich_hex(U8 c) { return c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1; }

static String8 rich_percent_decode(Arena *a, String8 s)
{
    U8 *out = push_array_nz(a, U8, s.size + 1);
    U64 n = 0;
    for (U64 i = 0; i < s.size; i += 1) {
        int hi = s.str[i] == '%' && i + 2 < s.size ? rich_hex(s.str[i + 1]) : -1, lo = hi >= 0 ? rich_hex(s.str[i + 2]) : -1;
        if (lo >= 0) { out[n++] = (U8)(hi * 16 + lo); i += 2; }
        else out[n++] = s.str[i];
    }
    return str8(out, n);
}

static U64 markup_entity(String8 s, U64 i, U32 *cp);

static String8 rich_url(Arena *a, String8 s)
{
    s = str8_trim(s);
    if (!s.size || s.size > 1000) return str8(0, 0);
    if (str8_find(s, 0, str8_lit("&"), 0) < s.size) { // "&amp;"
        U8 *out = push_array_nz(a, U8, s.size + 1);
        U64 n = 0;
        for (U64 i = 0; i < s.size;) {
            U32 cp = 0;
            U64 len = s.str[i] == '&' ? markup_entity(s, i, &cp) : 0;
            if (len && cp < 0x80) { out[n++] = (U8)cp; i += len; }
            else out[n++] = s.str[i++];
        }
        s = str8(out, n);
    }
    // CurseForge sends links out through its own page, which says where they go (encoded twice).
    String8 key = str8_lit("linkout?remoteUrl=");
    U64 lo = str8_find(s, 0, key, 1);
    if (lo < s.size) s = rich_percent_decode(a, rich_percent_decode(a, str8_skip(s, lo + key.size)));
    if (str8_starts_with(s, str8_lit("//"), 0)) s = str8_pushf(a, "https:%.*s", S8F(s));
    if (!str8_starts_with(s, str8_lit("https://"), 1) || s.size < 9) return str8(0, 0);
    for (U64 i = 0; i < s.size; i += 1) {
        U8 c = s.str[i];
        if (c <= ' ' || c == 0x7F || c == '"' || c == '<' || c == '>' || c == '\\' || c == '`') return str8(0, 0);
    }
    return str8_copy(a, s); // (the source may be freed before the document)
}

// The part of a URL people know a site by.
static String8 rich_host(String8 url)
{
    String8 h = str8_skip(url, 8);
    U64 n = 0;
    while (n < h.size && h.str[n] != '/' && h.str[n] != '?' && h.str[n] != '#') n += 1;
    h = str8_prefix(h, n);
    return str8_starts_with(h, str8_lit("www."), 1) ? str8_skip(h, 4) : h;
}

static void rich_image(RichBuild *b, String8 src, String8 alt)
{
    String8 url = rich_url(b->a, src);
    if (!url.size || b->full) return;
    alt = str8_trim(alt);
    if (alt.size > 80) {
        U64 cut = 80;
        while (cut && (alt.str[cut] & 0xC0) == 0x80) cut -= 1; // whole characters
        alt = str8_prefix(alt, cut);
    }
    String8 name = alt.size ? alt : b->link.size ? rich_host(b->link) : str8_lit("Picture");
    if (b->n + name.size > b->cap) return;
    if (!b->cur) rich_block(b, RICH_PARA);
    RichSpan *sp = push_struct(b->a, RichSpan);
    sp->flags = RICH_IMAGE;
    sp->link = b->link;
    sp->image = url;
    sp->text = str8_copy(b->a, name);
    for (U64 i = 0; i < sp->text.size; i += 1) if (sp->text.str[i] < ' ') sp->text.str[i] = ' ';
    if (b->cur->last) b->cur->last->next = sp; else b->cur->first = sp;
    b->cur->last = sp;
}

//- Entities and tags

// "&amp;" at s[i] -> its character; returns the length consumed (0 if not an entity).
static U64 markup_entity(String8 s, U64 i, U32 *cp)
{
    static const struct { char *name; U32 cp; } names[] = {
        { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' }, { "nbsp", 0xA0 },
        { "mdash", 0x2014 }, { "ndash", 0x2013 }, { "hellip", 0x2026 }, { "rsquo", 0x2019 }, { "lsquo", 0x2018 },
        { "rdquo", 0x201D }, { "ldquo", 0x201C }, { "bull", 0x2022 }, { "copy", 0xA9 }, { "trade", 0x2122 },
        { "rarr", 0x2192 }, { "larr", 0x2190 }, { "middot", 0xB7 }, { "reg", 0xAE }, { "times", 0xD7 }, { "deg", 0xB0 },
    };
    U64 end = i + 1;
    while (end < s.size && end - i < 12 && s.str[end] != ';') end += 1;
    if (end >= s.size || s.str[end] != ';') return 0;
    String8 name = str8_substr(s, i + 1, end);
    if (name.size > 1 && name.str[0] == '#') {
        B32 hex = name.str[1] == 'x' || name.str[1] == 'X';
        U32 v = 0;
        for (U64 k = hex ? 2 : 1; k < name.size; k += 1) {
            int d = rich_hex(name.str[k]);
            if (d < 0 || (!hex && d > 9)) return 0;
            if (v < 0x110000) v = v * (hex ? 16 : 10) + (U32)d;
        }
        *cp = v && v < 0x110000 ? v : '?';
        return end - i + 1;
    }
    for (U32 k = 0; k < ArrayCount(names); k += 1) if (str8_match(name, str8_cstring(names[k].name), 0)) { *cp = names[k].cp; return end - i + 1; }
    return 0;
}

static B32 markup_alpha(U8 c) { return (c | 32) >= 'a' && (c | 32) <= 'z'; }
static B32 markup_alnum(U8 c) { return markup_alpha(c) || (c >= '0' && c <= '9'); }

// Looking ahead is paid for from a budget, so a text made to be slow ("[[[[...") isn't.
#define RICH_LOOK_TOTAL (4u << 20)
static void rich_looked(RichBuild *b, U64 n) { b->look -= Min(b->look, n + 1); }

// Where needle is in s from `from`, at most RICH_LOOK bytes on (s.size: not there).
static U64 markup_find_near(RichBuild *b, String8 s, U64 from, String8 needle)
{
    if (from >= s.size || !b->look) return s.size;
    String8 near_ = str8_prefix(s, Min(s.size, from + RICH_LOOK));
    U64 at = str8_find(near_, from, needle, 1);
    rich_looked(b, at - Min(at, from));
    return at < near_.size ? at : s.size;
}

// The end ('>') of the tag at s[i], past any '>' in a quoted attribute; s.size if there's none near.
static U64 markup_tag_end(RichBuild *b, String8 s, U64 i)
{
    U8 quote = 0;
    U64 stop = Min(s.size, i + 2 * RICH_LOOK), plain = s.size;
    if (!b->look) stop = Min(stop, i + 200);
    for (U64 k = i + 1; k < stop; k += 1) {
        U8 c = s.str[k];
        if (c == '>' && plain == s.size) plain = k;
        if (quote) { if (c == quote) quote = 0; }
        else if ((c == '"' || c == '\'') && s.str[k - 1] == '=') quote = c;
        else if (c == '>') { rich_looked(b, k - i); return k; }
    }
    rich_looked(b, stop - i);
    return plain; // (a quote that isn't closed)
}

static String8 markup_attr(String8 tag, char *name)
{
    String8 n = str8_cstring(name);
    for (U64 at = str8_find(tag, 0, n, 1); at < tag.size; at = str8_find(tag, at + 1, n, 1)) {
        U64 e = at + n.size;
        if (!at || !(char_is_space(tag.str[at - 1]) || tag.str[at - 1] == '"' || tag.str[at - 1] == '\'')) continue;
        while (e < tag.size && char_is_space(tag.str[e])) e += 1;
        if (e >= tag.size || tag.str[e] != '=') continue;
        e += 1;
        while (e < tag.size && char_is_space(tag.str[e])) e += 1;
        if (e >= tag.size) break;
        U8 q = tag.str[e];
        U64 from = e, to;
        if (q == '"' || q == '\'') { from = e + 1; to = from; while (to < tag.size && tag.str[to] != q) to += 1; }
        else { to = from; while (to < tag.size && !char_is_space(tag.str[to])) to += 1; }
        return str8_substr(tag, from, to);
    }
    return str8(0, 0);
}

static void rich_count(U32 *n, B32 closing) { if (!closing) *n += 1; else if (*n) *n -= 1; }

static void rich_list_open(RichBuild *b, B32 ordered)
{
    if (b->cell) return;
    if (!b->nlists) rich_break(b, 1); // (a list in an item goes right under it)
    rich_end(b);
    if (b->nlists < RICH_LISTS) {
        b->lists[b->nlists].ordered = ordered;
        b->lists[b->nlists].n = 0;
        b->nlists += 1;
    }
}

static void rich_list_close(RichBuild *b)
{
    if (b->cell) return;
    rich_end(b);
    if (b->nlists) b->nlists -= 1;
    if (!b->nlists) b->gap = 1;
}

static void rich_list_item(RichBuild *b)
{
    U32 number = 0;
    if (b->nlists && b->lists[b->nlists - 1].ordered) number = b->lists[b->nlists - 1].n += 1;
    rich_item(b, number);
}

// A tag (what's between '<' and '>'). Returns 0 if it isn't one this knows.
static B32 rich_tag(RichBuild *b, String8 tag)
{
    B32 closing = tag.size && tag.str[0] == '/';
    if (closing) tag = str8_skip(tag, 1);
    U64 n = 0;
    while (n < tag.size && markup_alnum(tag.str[n])) n += 1;
    if (!n || n > 12 || !markup_alpha(tag.str[0])) return 0;
    if (n < tag.size && !char_is_space(tag.str[n]) && tag.str[n] != '/') return 0;
    char name[16] = { 0 };
    for (U64 i = 0; i < n; i += 1) name[i] = (char)char_lower(tag.str[i]);
#define TAG(t) (!strcmp(name, t))
    if (TAG("br")) rich_newline(b);
    else if (TAG("b") || TAG("strong")) rich_count(&b->bold, closing);
    else if (TAG("i") || TAG("em")) rich_count(&b->italic, closing);
    else if (TAG("s") || TAG("del") || TAG("strike")) rich_count(&b->strike, closing);
    else if (TAG("code") || TAG("kbd") || TAG("tt") || TAG("samp")) { if (!b->pre) rich_count(&b->code, closing); else if (closing && b->code) b->code -= 1; }
    else if (TAG("a")) b->link = closing ? str8(0, 0) : rich_url(b->a, markup_attr(tag, "href"));
    else if (TAG("img")) rich_image(b, markup_attr(tag, "src"), markup_attr(tag, "alt"));
    else if (n == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6') {
        rich_break(b, 1);
        if (!closing && !b->cell) { if (b->cur) rich_end(b); RichBlock *k = rich_block(b, RICH_HEADING); k->level = (U32)(name[1] - '0'); k->indent = 0; }
    } else if (TAG("ul") || TAG("ol")) { if (closing) rich_list_close(b); else rich_list_open(b, TAG("ol")); }
    else if (TAG("li")) { if (!closing) rich_list_item(b); else if (!b->cell) rich_end(b); }
    else if (TAG("blockquote")) { rich_break(b, 1); if (!b->cell) rich_end(b); rich_count(&b->quote, closing); }
    else if (TAG("pre")) {
        rich_break(b, 1);
        if (!b->cell) { rich_end(b); if (!closing) { rich_block(b, RICH_PRE); b->pre = 1; } }
    } else if (TAG("hr")) rich_rule(b);
    else if (TAG("table")) {
        b->cell = b->head = 0;
        rich_break(b, 1);
        if (!closing) { b->tables += 1; b->row = b->col = 0; }
    } else if (TAG("tr")) { b->cell = b->head = 0; rich_break(b, 0); if (!closing) { b->row += 1; b->col = 0; } }
    else if (TAG("td") || TAG("th")) {
        b->cell = b->head = 0;
        rich_end(b);
        if (!closing) {
            b->head = TAG("th");
            RichBlock *k = rich_block(b, RICH_CELL);
            k->table = b->tables; k->row = b->row; k->col = b->col++;
            b->cell = 1;
        }
    } else if (TAG("p") || TAG("div") || TAG("center") || TAG("section") || TAG("details") || TAG("summary")) rich_break(b, 1);
    else if (TAG("span") || TAG("font") || TAG("u") || TAG("sub") || TAG("sup") || TAG("small") || TAG("big") || TAG("tbody") || TAG("thead")
          || TAG("iframe") || TAG("picture") || TAG("source") || TAG("video")) {}
    else return 0;
#undef TAG
    return 1;
}

// A tag, comment, script or style at s[i] (a '<'): returns the length to skip, 0 if it's just text.
// any: also skip tags that aren't known (HTML), else they're text (Markdown).
static U64 rich_markup(RichBuild *b, String8 s, U64 i, B32 any)
{
    if (i + 1 >= s.size) return 0;
    U8 c = s.str[i + 1];
    if (c == '!' ) {
        if (str8_match(str8_prefix(str8_skip(s, i), 4), str8_lit("<!--"), 0)) {
            U64 ce = str8_find(s, i + 4, str8_lit("-->"), 0);
            return (ce < s.size ? ce + 3 : s.size) - i;
        }
    } else if (c == '/' ? !(i + 2 < s.size && markup_alpha(s.str[i + 2])) : !markup_alpha(c)) return 0;
    U64 end = markup_tag_end(b, s, i);
    if (end >= s.size) return 0;
    String8 tag = str8_substr(s, i + 1, end);
    for (int k = 0; k < 2; k += 1) {
        String8 name = k ? str8_lit("style") : str8_lit("script");
        if (!str8_match(str8_prefix(tag, name.size), name, 1) || (tag.size > name.size && markup_alnum(tag.str[name.size]))) continue;
        U64 ce = str8_find(s, end, k ? str8_lit("</style") : str8_lit("</script"), 1);
        if (ce >= s.size) return s.size - i;
        U64 cend = str8_find(s, ce, str8_lit(">"), 0);
        return (cend < s.size ? cend + 1 : s.size) - i;
    }
    if (!rich_tag(b, tag) && !any) return 0;
    return end + 1 - i;
}

static void rich_html(RichBuild *b, String8 s)
{
    for (U64 i = 0; i < s.size && !b->full;) {
        U8 c = s.str[i];
        U64 len = 0;
        if (c == '<') len = rich_markup(b, s, i, 1);
        else if (c == '&') {
            U32 cp = 0;
            len = markup_entity(s, i, &cp);
            if (len) rich_codepoint(b, cp);
        }
        if (!len) { rich_char(b, c); len = 1; }
        i += len;
    }
}

//- Markdown and BBCode: line by line

// [text](url) at s[i] (the '['): the text, the url, and where it ends. 0 if it isn't one.
static B32 markup_link(RichBuild *b, String8 s, U64 i, String8 *text, String8 *url, U64 *end)
{
    U64 stop = Min(s.size, i + RICH_LOOK), close = s.size;
    int depth = 0;
    if (!b->look) return 0;
    for (U64 k = i; k < stop; k += 1) {
        if (s.str[k] == '\\') { k += 1; continue; }
        if (s.str[k] == '[') depth += 1;
        if (s.str[k] == ']' && --depth == 0) { close = k; break; }
    }
    rich_looked(b, Min(close, stop) - i);
    if (close + 1 >= s.size || s.str[close + 1] != '(') return 0;
    U64 from = close + 2, to = s.size, first = s.size;
    stop = Min(s.size, from + RICH_LOOK);
    depth = 1;
    for (U64 k = from; k < stop; k += 1) {
        if (s.str[k] == '(') depth += 1;
        if (s.str[k] == ')') { if (first == s.size) first = k; if (--depth == 0) { to = k; break; } }
    }
    rich_looked(b, Min(to, stop) - Min(from, stop));
    if (to == s.size) to = first; // parentheses that don't pair up
    if (to == s.size) return 0;
    *text = str8_substr(s, i + 1, close);
    String8 u = str8_trim(str8_substr(s, from, to));
    if (u.size && u.str[0] == '<') { u = str8_skip(u, 1); u = str8_prefix(u, str8_find(u, 0, str8_lit(">"), 0)); }
    else { U64 n = 0; while (n < u.size && !char_is_space(u.str[n])) n += 1; u = str8_prefix(u, n); } // without a "title"
    *url = u;
    *end = to + 1;
    return 1;
}

// The tag at t[i] (a '['): returns the length to skip, 0 if it's just text.
static U64 rich_bbtag(RichBuild *b, String8 t, U64 i)
{
    U64 end = markup_find_near(b, t, i, str8_lit("]"));
    if (end >= t.size) return 0;
    String8 tag = str8_substr(t, i + 1, end);
    U64 len = end + 1 - i;
    if (str8_match(tag, str8_lit("*"), 0)) { rich_list_item(b); return len; }
    B32 closing = tag.size && tag.str[0] == '/';
    if (closing) tag = str8_skip(tag, 1);
    U64 n = 0;
    while (n < tag.size && markup_alnum(tag.str[n])) n += 1;
    if (!n || n > 12 || (n < tag.size && tag.str[n] != '=' && tag.str[n] != ' ')) return 0;
    char name[16] = { 0 };
    for (U64 k = 0; k < n; k += 1) name[k] = (char)char_lower(tag.str[k]);
    String8 value = n < tag.size ? str8_trim(str8_skip(tag, n + 1)) : str8(0, 0);
    if (value.size > 1 && value.str[0] == '"' && value.str[value.size - 1] == '"') value = str8_substr(value, 1, value.size - 1);
#define TAG(t) (!strcmp(name, t))
    if (TAG("b")) rich_count(&b->bold, closing);
    else if (TAG("i")) rich_count(&b->italic, closing);
    else if (TAG("s") || TAG("strike")) rich_count(&b->strike, closing);
    else if (TAG("url")) {
        if (closing) b->link = str8(0, 0);
        else if (value.size) b->link = rich_url(b->a, value);
        else b->link = rich_url(b->a, str8_substr(t, end + 1, markup_find_near(b, t, end, str8_lit("[/url]"))));
    } else if (TAG("img")) {
        if (!closing) {
            U64 ce = markup_find_near(b, t, end, str8_lit("[/img]"));
            rich_image(b, str8_substr(t, end + 1, ce), str8(0, 0));
            return (ce < t.size ? ce + 6 : t.size) - i;
        }
    } else if (TAG("list")) { // (right under the line above: the blank lines an author wants are there)
        if (closing) { rich_end(b); if (b->nlists) b->nlists -= 1; }
        else { B32 gap = b->gap; rich_list_open(b, str8_match(value, str8_lit("1"), 0)); b->gap = gap; }
    } else if (TAG("quote")) { rich_end(b); b->gap = 1; rich_count(&b->quote, closing); }
    else if (TAG("code") || TAG("php") || TAG("highlight")) {
        rich_end(b);
        b->gap = 1;
        b->bb_code = !closing;
        if (!closing) { rich_block(b, RICH_PRE); b->pre = 1; }
    } else if (TAG("u") || TAG("color") || TAG("size") || TAG("font") || TAG("center") || TAG("left") || TAG("right") || TAG("indent")
            || TAG("email") || TAG("youtube") || TAG("spoiler") || TAG("noparse") || TAG("h1") || TAG("h2")) {}
    else return 0;
#undef TAG
    return len;
}

static void rich_inline(RichBuild *b, String8 t, B32 bbcode, int depth)
{
    for (U64 i = 0; i < t.size && !b->full;) {
        U8 c = t.str[i], next = i + 1 < t.size ? t.str[i + 1] : 0, prev = i ? t.str[i - 1] : ' ';
        U64 len = 0;
        if (bbcode && b->bb_code) { // until [/code]
            if (c == '[' && str8_match(str8_prefix(str8_skip(t, i), 2), str8_lit("[/"), 0)) len = rich_bbtag(b, t, i);
            if (!len) { rich_put(b, &c, 1); len = 1; }
            i += len;
            continue;
        }
        if (c == '\\' && !bbcode && next && next < 0x80 && next > ' ' && !markup_alnum(next)) { rich_char(b, next); i += 2; continue; }
        if (c == '`' && !bbcode) {
            U64 ticks = 1;
            while (i + ticks < t.size && t.str[i + ticks] == '`' && ticks < 8) ticks += 1;
            U64 close = markup_find_near(b, t, i + ticks, str8_prefix(str8_lit("````````"), ticks));
            if (close < t.size) {
                String8 code = str8_trim(str8_substr(t, i + ticks, close));
                b->code += 1;
                for (U64 k = 0; k < code.size; k += 1) rich_char(b, code.str[k]);
                if (b->code) b->code -= 1;
                i = close + ticks;
                continue;
            }
        }
        if (c == '[' && bbcode) len = rich_bbtag(b, t, i);
        if (!bbcode && depth < 4 && (c == '[' || (c == '!' && next == '['))) {
            String8 text, url;
            U64 end;
            if (markup_link(b, t, c == '!' ? i + 1 : i, &text, &url, &end)) {
                if (c == '!') rich_image(b, url, text);
                else {
                    String8 outer = b->link, mine = rich_url(b->a, url);
                    if (!outer.size) b->link = mine;
                    rich_inline(b, text, bbcode, depth + 1);
                    b->link = outer;
                }
                i = end;
                continue;
            }
        }
        if (c == '<' && !bbcode) {
            U64 gt = markup_find_near(b, t, i, str8_lit(">"));
            if (str8_starts_with(str8_skip(t, i + 1), str8_lit("https://"), 1) && gt < t.size) { t = str8_skip(t, i + 1); i = 0; c = t.str[0]; prev = ' '; }
            else len = rich_markup(b, t, i, 0);
        }
        if (c == '&') {
            U32 cp = 0;
            len = markup_entity(t, i, &cp);
            if (len) rich_codepoint(b, cp);
        }
        // A bare address is a link.
        if (!len && (c | 32) == 'h' && !b->link.size && !markup_alnum(prev) && str8_starts_with(str8_skip(t, i), str8_lit("https://"), 1)) {
            U64 e = i, stop = Min(t.size, i + 1000);
            while (e < stop && t.str[e] > ' ' && !strchr("<>\"`[]|*", t.str[e])) e += 1;
            while (e > i && strchr(".,;:!?)'", t.str[e - 1])) e -= 1;
            b->link = rich_url(b->a, str8_substr(t, i, e));
            if (b->link.size) {
                for (U64 k = i; k < e; k += 1) rich_char(b, t.str[k]);
                b->link = str8(0, 0);
                i = e + (e < t.size && t.str[e] == '>');
                continue;
            }
            b->link = str8(0, 0);
        }
        // **bold**, *italic*, ~~gone~~ (and with '_', not inside a word). BBCode authors use ** too.
        if (!len && (c == '*' || c == '_' || c == '~')) {
            U64 run = next == c ? 2 : 1;
            U32 flag = c == '~' ? (run == 2 ? RICH_STRIKE : 0) : run == 2 ? RICH_BOLD : bbcode ? 0 : RICH_ITALIC;
            U8 after = i + run < t.size ? t.str[i + run] : ' ';
            if (flag && (b->md & flag)) {
                if (prev != ' ' && !(c == '_' && markup_alnum(after))) { b->md &= ~flag; len = run; }
            } else if (flag && after != ' ' && !(c == '_' && markup_alnum(prev))) {
                U8 mark[2] = { c, c };
                U64 close = markup_find_near(b, t, i + run + 1, str8(mark, run));
                if (close < t.size) { b->md |= flag; len = run; }
            }
        }
        if (!len) { rich_char(b, c); len = 1; }
        i += len;
    }
    if (!depth) b->md = 0;
}

static B32 markup_is_rule(String8 t) // "---", "* * *"
{
    U64 n = 0;
    if (!t.size || !strchr("-*_", t.str[0])) return 0;
    for (U64 i = 0; i < t.size; i += 1) {
        if (t.str[i] == t.str[0]) n += 1;
        else if (t.str[i] != ' ') return 0;
    }
    return n >= 3;
}

static B32 markup_table_rule(String8 t) // "|---|:--:|"
{
    U64 dashes = 0, bars = 0;
    for (U64 i = 0; i < t.size; i += 1) {
        if (t.str[i] == '-') dashes += 1;
        else if (t.str[i] == '|') bars += 1;
        else if (t.str[i] != ':' && t.str[i] != ' ') return 0;
    }
    return dashes && bars;
}

static void rich_table_row(RichBuild *b, String8 t, B32 head)
{
    t = str8_trim(t);
    if (t.size && t.str[0] == '|') t = str8_skip(t, 1);
    if (t.size && t.str[t.size - 1] == '|') t = str8_chop(t, 1);
    b->row += 1;
    b->col = 0;
    for (U64 from = 0; from <= t.size && b->col < 32;) {
        U64 to = from;
        while (to < t.size && t.str[to] != '|') to += 1 + (t.str[to] == '\\' && to + 1 < t.size);
        rich_end(b);
        RichBlock *k = rich_block(b, RICH_CELL);
        k->table = b->tables; k->row = b->row; k->col = b->col++;
        b->cell = 1;
        b->bold += head;
        rich_inline(b, str8_trim(str8_substr(t, from, Min(to, t.size))), 0, 0);
        if (head && b->bold) b->bold -= 1;
        b->cell = 0;
        from = to + 1;
    }
    rich_end(b);
}

static void rich_lines(RichBuild *b, String8 s, B32 bbcode)
{
    B32 fence = 0, table = 0;
    while (s.size && !b->full) {
        // A comment over several lines.
        String8 ahead = str8_trim(str8_prefix(s, 64));
        if (!bbcode && !fence && str8_starts_with(ahead, str8_lit("<!--"), 0)) {
            U64 from = str8_find(s, 0, str8_lit("<!--"), 0), ce = str8_find(s, from, str8_lit("-->"), 0);
            s = str8_skip(s, ce < s.size ? ce + 3 : s.size);
            continue;
        }
        String8 line = str8_pop_line(&s);
        while (line.size && (line.str[line.size - 1] == '\r' || line.str[line.size - 1] == ' ' || line.str[line.size - 1] == '\t')) line.size -= 1;
        String8 t = str8_trim(line);
        if (!b->bb_code && (str8_starts_with(t, str8_lit("```"), 0) || str8_starts_with(t, str8_lit("~~~"), 0))) {
            rich_end(b);
            b->gap = 1;
            fence = !fence;
            if (fence) { rich_block(b, RICH_PRE); b->pre = 1; }
            continue;
        }
        if (fence || b->bb_code) {
            if (fence) rich_put(b, line.str, line.size);
            else rich_inline(b, line, 1, 0);
            if (b->pre) rich_put(b, (U8 *)"\n", 1);
            continue;
        }
        if (!t.size) { if (!b->cell) { rich_end(b); b->gap = 1; } table = 0; if (!bbcode) b->quote = 0; continue; }
        U64 column = 0;
        for (U64 i = 0; i < line.size && char_is_space(line.str[i]); i += 1) column += line.str[i] == '\t' ? 4 : 1;

        if (!b->cell) {
            if (!bbcode) { // > quotes
                U32 quote = 0;
                while (t.size && t.str[0] == '>' && quote < 8) { quote += 1; t = str8_trim(str8_skip(t, 1)); }
                if (quote != b->quote && (quote || b->quote)) {
                    if (quote || !b->cur) { rich_end(b); b->gap = 1; b->quote = quote; } // (else the quote goes on)
                }
                if (!t.size) { rich_end(b); b->gap = 1; continue; }
            }
            U64 hashes = 0;
            while (hashes < t.size && t.str[hashes] == '#') hashes += 1;
            B32 has_text = b->cur && b->cur->first && b->cur->kind == RICH_PARA;
            U64 digits = 0;
            while (digits < t.size && digits < 9 && t.str[digits] >= '0' && t.str[digits] <= '9') digits += 1;
            B32 ordered = digits && digits + 1 < t.size && (t.str[digits] == '.' || t.str[digits] == ')') && t.str[digits + 1] == ' ';
            U64 mark = t.size > 1 && strchr("-*+", t.str[0]) && t.str[1] == ' ' ? 2
                     : str8_starts_with(t, str8_lit("\xE2\x80\xA2 "), 0) ? 4 : ordered ? digits + 2 : 0;
            if (hashes && hashes < 7 && hashes < t.size && t.str[hashes] == ' ') {
                rich_end(b);
                b->nlists = 0;
                b->gap = 1;
                t = str8_trim(str8_skip(t, hashes));
                while (t.size && t.str[t.size - 1] == '#') t.size -= 1; // "# Title #"
                t = str8_trim(t);
                RichBlock *k = rich_block(b, RICH_HEADING);
                k->level = (U32)hashes;
                rich_inline(b, t, bbcode, 0);
                rich_end(b);
                b->gap = 1;
                continue;
            }
            if (!bbcode && has_text && t.size > 2 && (t.str[0] == '=' || t.str[0] == '-')) { // underlined: a heading
                U64 same = 0;
                while (same < t.size && t.str[same] == t.str[0]) same += 1;
                for (RichSpan *sp = b->cur->first; sp; sp = sp->next) if (str8_find(sp->text, 0, str8_lit("\n"), 0) < sp->text.size) same = 0; // (one line only)
                if (same == t.size) {
                    b->cur->kind = RICH_HEADING;
                    b->cur->level = t.str[0] == '=' ? 1 : 2;
                    rich_end(b);
                    b->gap = 1;
                    continue;
                }
            }
            if (markup_is_rule(t)) { b->nlists = 0; rich_rule(b); continue; }
            if (!bbcode && table) {
                if (str8_find(t, 0, str8_lit("|"), 0) < t.size) { rich_table_row(b, t, 0); continue; }
                table = 0;
                b->gap = 1;
            }
            if (!bbcode && !mark && str8_find(t, 0, str8_lit("|"), 0) < t.size) {
                String8 rest = s, under = str8_trim(str8_pop_line(&rest));
                if (under.size < RICH_LOOK && markup_table_rule(under)) {
                    rich_end(b);
                    b->gap = 1;
                    b->nlists = 0;
                    b->tables += 1; b->row = 0;
                    rich_table_row(b, t, 1);
                    s = rest;
                    table = 1;
                    continue;
                }
            }
            if (mark) {
                U32 lists = b->nlists;
                if (!bbcode || !b->nlists) { // the list it's in: by its column
                    while (b->nlists && column < b->lists[b->nlists - 1].column) b->nlists -= 1;
                    if (!b->nlists || (column > b->lists[b->nlists - 1].column + 1 && b->nlists < RICH_LISTS)) b->lists[b->nlists++].column = column;
                }
                U32 number = 0;
                for (U64 i = 0; ordered && i < digits; i += 1) number = number * 10 + (t.str[i] - '0');
                rich_item(b, number);
                rich_inline(b, str8_trim(str8_skip(t, mark)), bbcode, 0);
                if (bbcode) { rich_end(b); b->nlists = lists; }
                continue;
            }
            if (!bbcode && b->nlists && column <= b->lists[b->nlists - 1].column) { // out of this list (maybe into the one around it)
                rich_end(b);
                while (b->nlists && column <= b->lists[b->nlists - 1].column) b->nlists -= 1;
            }
            if (b->cur && b->cur->kind == RICH_ITEM && !bbcode && b->cur->first) rich_char(b, ' '); // an item's next line
            else rich_newline(b);
        } else rich_char(b, ' ');
        rich_inline(b, t, bbcode, 0);
        if (b->bb_code && b->pre && rich_last_byte(b)) rich_put(b, (U8 *)"\n", 1);
        if (bbcode && b->cur && b->cur->kind == RICH_ITEM) rich_end(b);
    }
}

// The finished document: without empty blocks, cells knowing how many columns their table has (a
// table of one column is just paragraphs: authors center things with those).
static void rich_finish(RichBuild *b)
{
    rich_end(b);
    RichBlock **link = &b->doc->first, *last = 0;
    B32 gap = 0;
    for (RichBlock *k = b->doc->first; k; k = k->next) {
        B32 text = k->kind == RICH_RULE || k->kind == RICH_CELL;
        for (RichSpan *sp = k->first; sp && !text; sp = sp->next) text = sp->text.size > 0;
        gap |= k->gap;
        if (!text) { *link = k->next; continue; }
        k->gap = last ? gap : 0;
        gap = 0;
        last = k;
        link = &k->next;
    }
    b->doc->last = last;
    for (RichBlock *k = b->doc->first; k;) {
        if (k->kind != RICH_CELL) { k = k->next; continue; }
        U32 cols = 0;
        RichBlock *end = k;
        for (; end && end->kind == RICH_CELL && end->table == k->table; end = end->next) cols = Max(cols, end->col + 1);
        for (RichBlock *c = k; c != end; c = c->next) { c->cols = cols; if (cols == 1) { c->kind = RICH_PARA; c->gap = 1; } }
        k = end;
    }
    // (Empty cells are kept, for their columns; a paragraph that was one may be empty: it draws nothing.)
}

// At most max bytes of text (the rest is left out, after "..."). Nothing in it points into s.
static Rich *rich_parse(Arena *a, String8 s, MarkupKind kind, U64 max)
{
    static volatile LONG64 ids;
    RichBuild b = { 0 };
    b.a = a;
    b.look = RICH_LOOK_TOTAL;
    b.doc = push_struct(a, Rich);
    b.doc->id = (U64)InterlockedIncrement64(&ids);
    b.cap = Min(max, s.size * 3 + 64);
    b.buf = push_array_nz(a, U8, b.cap + 8);
    if (kind == MARKUP_HTML) rich_html(&b, s);
    else rich_lines(&b, s, kind == MARKUP_BBCODE);
    if (b.full) {
        while (b.n && (b.buf[b.n - 1] & 0xC0) == 0x80) b.n -= 1; // a character cut off
        if (b.n && b.buf[b.n - 1] >= 0xC0) b.n -= 1;
        RichSpan *sp = b.cur ? b.cur->last : 0;
        if (sp && !(sp->flags & RICH_IMAGE) && sp->text.str + sp->text.size >= b.buf + b.n) sp->text.size = b.n > (U64)(sp->text.str - b.buf) ? b.n - (U64)(sp->text.str - b.buf) : 0;
        rich_trim(&b);
        b.full = 0;
        b.cap = b.n + 8;
        b.head = b.bold = b.italic = b.strike = b.code = b.md = 0;
        b.link = str8(0, 0);
        b.cell = 0;
        rich_put(&b, (U8 *)"...", 3);
    }
    rich_finish(&b);
    return b.doc;
}

//- The text of one

static String8 rich_block_plain(Arena *a, RichBlock *k) // without pictures' names
{
    U64 n = 0;
    for (RichSpan *sp = k->first; sp; sp = sp->next) if (!(sp->flags & RICH_IMAGE)) n += sp->text.size;
    U8 *out = push_array_nz(a, U8, n + 1);
    n = 0;
    for (RichSpan *sp = k->first; sp; sp = sp->next) if (!(sp->flags & RICH_IMAGE)) { memcpy(out + n, sp->text.str, sp->text.size); n += sp->text.size; }
    return str8_trim(str8(out, n));
}

static String8 rich_plain(Arena *a, Rich *doc)
{
    String8List out = { 0 };
    B32 gap = 0;
    U32 row = 0;
    for (RichBlock *k = doc ? doc->first : 0; k; k = k->next) {
        String8 t = rich_block_plain(a, k);
        gap |= k->gap;
        if (!t.size) continue;
        B32 same_row = k->kind == RICH_CELL && k->col && k->row == row;
        if (out.count) str8_list_push(a, &out, same_row ? str8_lit(" | ") : gap ? str8_lit("\n\n") : str8_lit("\n"));
        if (k->kind == RICH_ITEM) str8_list_push(a, &out, k->number ? str8_pushf(a, "%u. ", k->number) : str8_lit("\xE2\x80\xA2 "));
        str8_list_push(a, &out, t);
        row = k->row;
        gap = 0;
    }
    return str8_list_join(a, &out, str8(0, 0));
}

static B32 rich_empty(Rich *doc) { return !doc || !doc->first; }

static String8 markup_to_text(Arena *a, String8 s, MarkupKind kind) { return rich_plain(a, rich_parse(a, s, kind, RICH_TEXT_MAX)); }

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
// HTML view): its text as Markdown, when it looks like that and the HTML has no formatting of its own.
static Rich *markup_cf(Arena *a, String8 s, U64 max)
{
    Rich *doc = rich_parse(a, s, MARKUP_HTML, max);
    for (RichBlock *k = doc->first; k; k = k->next) {
        if (k->kind == RICH_HEADING || k->kind == RICH_CELL || k->kind == RICH_PRE) return doc;
        for (RichSpan *sp = k->first; sp; sp = sp->next) if (sp->flags) return doc;
    }
    String8 t = rich_plain(a, doc);
    return markup_looks_markdown(t) ? rich_parse(a, t, MARKUP_MARKDOWN, max) : doc;
}

//- Changelogs that cover several versions

// Whether a line names version `from`. Only whole versions: "1.2" mustn't match inside "11.2.5" or "1.25".
static B32 line_names_version(String8 line, String8 from)
{
    for (U64 at = str8_find(line, 0, from, 1); at < line.size; at = str8_find(line, at + 1, from, 1)) {
        U8 before = at ? line.str[at - 1] : ' ', after = at + from.size < line.size ? line.str[at + from.size] : ' ';
        B32 digitish_before = (before >= '0' && before <= '9') || before == '.';
        B32 digitish_after = (after >= '0' && after <= '9') || (after == '.' && at + from.size + 1 < line.size && line.str[at + from.size + 1] >= '0' && line.str[at + from.size + 1] <= '9');
        if (!digitish_before && !digitish_after) return 1;
    }
    return 0;
}

// Cuts the document above the first line (after the first) that names version `from`.
static void rich_until(Rich *doc, String8 from)
{
    from = str8_trim(from);
    if (from.size && (from.str[0] == 'v' || from.str[0] == 'V')) from = str8_skip(from, 1);
    if (!doc || from.size < 2) return;
    Temp scratch = scratch_begin(0);
    RichBlock *prev = 0;
    B32 first = 1;
    for (RichBlock *k = doc->first; k; prev = k, k = k->next) {
        U32 lines = 0; // before the one being looked at
        String8 rest = rich_block_plain(scratch.arena, k);
        for (; rest.size; lines += 1) {
            String8 line = str8_pop_line(&rest);
            B32 cut = !first && line_names_version(line, from);
            first = 0;
            if (!cut) continue;
            if (!lines || !k->first) { // the whole block goes
                if (prev) { prev->next = 0; doc->last = prev; }
                else doc->first = doc->last = 0;
            } else { // from its lines'th line break on
                for (RichSpan *sp = k->first; sp; sp = sp->next) {
                    U64 at = 0;
                    for (; at < sp->text.size && lines; at += 1) if (!(sp->flags & RICH_IMAGE) && sp->text.str[at] == '\n') lines -= 1;
                    if (lines) continue;
                    sp->text.size = at ? at - 1 : 0;
                    sp->next = 0;
                    k->last = sp;
                    break;
                }
                k->next = 0;
                doc->last = k;
            }
            scratch_end(scratch);
            return;
        }
    }
    scratch_end(scratch);
}
