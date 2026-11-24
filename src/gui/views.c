// gui/views.c - wam's three views (Installed, Get addons, Downloads) and what they need: the
// remove plan, browsing and searching the catalog, the download folder watcher. Drawn with the UI library (ui/).
// Part of main_gui.c's unity build.

//- Drawing helpers
//
// Short names for the UI library's theme colors (ui/ui.h).
#define C_BG        ui.theme.bg
#define C_PANEL     ui.theme.panel
#define C_HOVER     ui.theme.hover
#define C_BUTTON    ui.theme.button
#define C_BUTTON_HI ui.theme.button_hi
#define C_BORDER    ui.theme.border
#define C_BORDER_HI ui.theme.border_hi
#define C_SELECTED  ui.theme.selected
#define C_LINE      ui.theme.line
#define C_TEXT      ui.theme.text
#define C_DIM       ui.theme.dim
#define C_FAINT     ui.theme.faint
#define C_ACCENT    ui.theme.accent
#define C_ACCENT_HI ui.theme.accent_hi
#define C_ACCENT_BG ui.theme.accent_bg
#define C_GREEN     ui.theme.ok
#define C_DANGER    ui.theme.danger
#define C_DANGER_HI ui.theme.danger_hi
#define C_ERROR     ui.theme.error
#define C_WARN      ui.theme.warn

#include "gui/rich.c"       // descriptions and changelogs (uses the colors above)

static String8 fmt_interface(Arena *a, String8 s)
{
    int n = 0, digits = 0;
    for (U64 i = 0; i < s.size && s.str[i] >= '0' && s.str[i] <= '9'; i += 1, digits += 1) n = n * 10 + (s.str[i] - '0');
    if (digits >= 5) return str8_pushf(a, "%d.%d.%d", n / 10000, (n / 100) % 100, n % 100);
    return s;
}

// A provider pill. Filled = where wam installs the addon from; outlined = also published there.
static const U32 src_fg[SRC_COUNT] = { 0xffd2c2, 0xd9ccff, 0xffe6a8, 0xd0d7de, 0xbfe8ff };
static const U32 src_bg[SRC_COUNT] = { 0x4a2618, 0x3a2b66, 0x4a3a12, 0x2d333b, 0x123a52 };
static const U32 src_hi[SRC_COUNT] = { 0x6a3622, 0x523d8f, 0x6a531a, 0x3d4550, 0x1b5475 };

static void draw_source_pill(Rect r, int src, B32 filled, B32 hot)
{
    const U32 *fg = src_fg, *bg = src_bg, *hi = src_hi;
    F32 radius = (r.y1 - r.y0) * 0.5f;
    if (filled || hot) r_rect(r, ui_hex(hot ? hi[src] : bg[src], 1), radius, 0, 0.5f);
    else r_rect(r, ui_hex(bg[src], 1), radius, ui_px(), 0.5f);
    ui_text_centered(ui.font.caption, r, str8_cstring(source_names[src]), ui_hex(fg[src], filled || hot ? 1.0f : 0.75f));
}

// CurseForge addons whose author lets only CurseForge's own website serve the files (see
// SearchResult.site_only): a tag after the source pill, so a download window never comes as a surprise.
static String8 site_tag_label(U8 site_only)
{
    return site_only == SITE_ONLY_PAGE ? str8_lit("\xE2\x86\x97 website download") : site_only == SITE_ONLY_GITHUB ? str8_lit("via GitHub") : str8(0, 0);
}

// Draws the tag at x (vertically centered on cy) if it ends before max_x; returns the x after it.
static F32 draw_site_tag(F32 x, F32 cy, U8 site_only, F32 max_x)
{
    String8 label = site_tag_label(site_only);
    if (!label.size) return x;
    F32 s = ui.scale, h = ui.font.caption->line_height + 2 * s, w = f_text_width(ui.font.caption, label) + 14 * s;
    if (x + w > max_x) return x;
    Rect r = rect(x, cy - h * 0.5f, x + w, cy + h * 0.5f);
    if (site_only == SITE_ONLY_PAGE) {
        r_rect(r, ui_hex(0x4a3612, 1), h * 0.5f, 0, 0.5f);
        ui_text_centered(ui.font.caption, r, label, ui_hex(0xffd88a, 1));
    } else {
        r_rect(r, ui_hex(0x2d333b, 1), h * 0.5f, ui_px(), 0.5f);
        ui_text_centered(ui.font.caption, r, label, ui_hex(0xd0d7de, 0.8f));
    }
    return r.x1;
}

// The catalog's site_only for an installed addon's id (0 when it isn't in the catalog).
static U8 site_only_of(String8 id)
{
    SearchResult *e = id_is(id, "cf:") ? cat_find(&app.cat, id) : 0;
    return e ? e->site_only : 0;
}

//- An addon row: the same look in Installed and Get addons
//
// Picture, then three lines that get quieter as they go: the name (and what's new, in Installed),
// what the addon does, and a faint line of facts ending in where it's published (each source a
// link to its page; the first is where wam installs it from). Each view adds its own controls
// around it (Installed: a checkbox before, Update / Remove after; Get addons: Install after).
#define ADDON_ROW_H 64

typedef struct RowLook {
    Rect rr;                 // the row
    Rect list;               // the list it's in (the mouse only counts inside it)
    F32 lead;                // space before the picture (Installed's checkbox)
    F32 text_right;          // text stops here; the view's buttons are right of it
    String8 icon_url;        // the picture, else the name's initial on the source's color
    int icon_src;            // SRC_*, -1 = none (grey)
    String8 title;
    V4 title_color;
    U8 *marks; U32 mark_count; // fuzzy-match positions to highlight in the title
    U32 pill_count;          // the sources it's published on, named at the end of the facts
    int pill_src[4];
    String8 pill_url[4];
    B32 pill_filled[4];      // where wam installs it from (named first, a little brighter)
    U8 site_only;            // SITE_ONLY_*: said after the sources
    String8 meta;            // the facts ("author · 12M downloads · updated 2 days ago")
    String8 extra;           // after the name, in its own color (Installed: "→ 1.2.4")
    V4 extra_color;
    B32 extra_link;          // the extra is a link: underlined on hover (the caller handles the click)
    Rect extra_rect;         // out: where the extra was drawn (empty if it didn't fit)
    String8 line2;           // what it does (a summary)
    V4 line2_color;
} RowLook;

// A picture, or the initial of name on src's color (grey for -1) until (or unless) one loads.
static void draw_icon(Rect r, String8 url, String8 name, int src, F32 radius)
{
    int c = src >= 0 ? src : SRC_GITHUB;
    ui_avatar(r, url, name, ui_hex(src_fg[c], 1), ui_hex(src_bg[c], 1), radius);
}

// The row's hover or selection: a rounded fill inset from the list's edges.
static void draw_row_fill(Rect rr, V4 color)
{
    F32 s = ui.scale;
    r_rect(rect(rr.x0 + 6 * s, rr.y0 + 2 * s, rr.x1 - 6 * s, rr.y1 - 2 * s), color, 8 * s, 0, 0.5f);
}

// Draws the row's content. Returns 1 if the last press was on one of its links (the caller
// shouldn't treat that click as one on the row); a source clicked opens its page.
static B32 draw_row_look(RowLook *l)
{
    F32 s = ui.scale;
    Arena *fa = ui.frame_arena;
    Rect rr = l->rr;
    F32 row_h = rr.y1 - rr.y0;
    F32 icon = 40 * s, ix = rr.x0 + 16 * s + l->lead;
    draw_icon(rect(ix, rr.y0 + (row_h - icon) * 0.5f, ix + icon, rr.y0 + (row_h + icon) * 0.5f), l->icon_url, l->title, l->icon_src, 9 * s);
    F32 tx = ix + icon + 14 * s, right = l->text_right, tw = right - tx;
    F32 lh_name = ui.font.bold->line_height, lh = ui.font.caption->line_height, gap = 2 * s;
    B32 has_facts = l->meta.size || l->pill_count || l->site_only;
    F32 h = lh_name + (l->line2.size ? gap + lh : 0) + (has_facts ? gap + lh : 0);
    F32 y = rr.y0 + (row_h - h) * 0.5f;
    B32 pressed_link = 0;

    // The name, then the extra.
    F32 x = l->marks ? ui_text_highlighted(ui.font.bold, tx, y, l->title, l->marks, l->mark_count, l->title_color, C_ACCENT_HI, tw)
                     : f_draw_text(ui.font.bold, tx, y, l->title, l->title_color, tw);
    l->extra_rect = rect(0, 0, 0, 0);
    if (l->extra.size && right - x - 12 * s > 30 * s) {
        F32 ex = x + 12 * s, ey = y + (lh_name - lh) * 0.5f, max_w = right - ex;
        if (l->extra_link) {
            ui_link_text(ui.font.caption, ex, ey, l->extra, l->extra_color, max_w, &l->extra_rect);
            if (point_in(l->extra_rect, ui.in.press_x, ui.in.press_y)) pressed_link = 1; // a link: not a click on the row
        } else {
            F32 end = f_draw_text(ui.font.caption, ex, ey, l->extra, l->extra_color, max_w);
            l->extra_rect = rect(ex - 4 * s, ey - 3 * s, end + 4 * s, ey + lh + 3 * s);
        }
    }
    y += lh_name + gap;

    // What it does.
    if (l->line2.size) {
        f_draw_text(ui.font.caption, tx, y, l->line2, l->line2_color, tw);
        y += lh + gap;
    }

    // The facts, then the sources (links), then how the files come.
    if (has_facts) {
        String8 dot = str8_lit("  \xC2\xB7  ");
        F32 mx = tx;
        if (l->meta.size) mx = f_draw_text(ui.font.caption, mx, y, l->meta, C_FAINT, right - mx);
        for (U32 k = 0; k < l->pill_count && right - mx > 40 * s; k += 1) {
            String8 sep = k ? str8_lit(", ") : l->meta.size ? dot : str8(0, 0);
            mx = f_draw_text(ui.font.caption, mx, y, sep, C_FAINT, right - mx);
            char *src_name = source_names[l->pill_src[k]];
            V4 c = l->pill_filled[k] ? C_DIM : C_FAINT;
            if (l->pill_url[k].size) {
                Rect hit;
                mx = ui_link_text(ui.font.caption, mx, y, str8_cstring(src_name), c, right - mx, &hit);
                if (point_in(hit, ui.in.press_x, ui.in.press_y)) pressed_link = 1;
                if (ui_mouse_in(hit) && ui_mouse_in(l->list)) {
                    ui_tooltip(hit, l->pill_filled[k] ? str8_pushf(fa, "wam installs it from %s. Click to open its page there.", src_name)
                                                      : str8_pushf(fa, "Also on %s. Click to open its page there.", src_name));
                    if (ui_clicked(hit)) os_open_url(l->pill_url[k]);
                }
            } else {
                mx = f_draw_text(ui.font.caption, mx, y, str8_cstring(src_name), c, right - mx);
            }
        }
        String8 tag = site_tag_label(l->site_only);
        if (tag.size && right - mx > 80 * s) {
            mx = f_draw_text(ui.font.caption, mx, y, dot, C_FAINT, right - mx);
            f_draw_text(ui.font.caption, mx, y, tag, l->site_only == SITE_ONLY_PAGE ? C_WARN : C_FAINT, right - mx);
        }
    }
    return pressed_link;
}

//- Views

// What a pending removal would do: mark[i] 1 = chosen, 2 = unused dependency (3, meanwhile: already being removed), plus
// rows that stay but need something being removed.
typedef struct RemovePlan { U8 *mark; String8List chosen, deps, broken, keys; } RemovePlan;

static RemovePlan remove_plan(Arena *a, Snapshot *snap, KeySet *chosen, B32 with_deps)
{
    RemovePlan p = { 0 };
    U64 n = snap ? snap->row_count : 0;
    p.mark = push_array(a, U8, n + 1);
    if (!n) return p;
    // Reuse the manager's dependency walk on the rows' edges.
    MgrUnits u = { push_array(a, MgrUnit, n), n };
    for (U64 i = 0; i < n; i += 1) {
        u.v[i].req = snap->rows[i].req;
        u.v[i].req_count = snap->rows[i].req_count;
        if (row_hidden(snap->rows[i].key)) { u.v[i].req_count = 0; p.mark[i] = 3; } // already on its way out: it needs nothing
        else if (keyset_has(chosen, snap->rows[i].key)) p.mark[i] = 1;
    }
    mgr_mark_unused_deps(&u, p.mark);
    for (U64 i = 0; i < n; i += 1) if (p.mark[i] == 3) p.mark[i] = 0;
    for (U64 i = 0; i < n; i += 1) {
        Row *r = &snap->rows[i];
        if (p.mark[i] == 1) str8_list_push(a, &p.chosen, r->title);
        if (p.mark[i] == 2) str8_list_push(a, &p.deps, r->title);
        if (p.mark[i] == 2 && !with_deps) p.mark[i] = 0;
        if (p.mark[i]) str8_list_push(a, &p.keys, r->key);
    }
    for (U64 i = 0; i < n; i += 1) if (mgr_unit_needs_marked(&u, p.mark, i)) str8_list_push(a, &p.broken, snap->rows[i].title);
    return p;
}

static void remove_open(KeySet *keys); // remove.c

// The Remove button of a row: that addon, or every checked one when it's among them.
static void start_remove(String8 key)
{
    KeySet *keys = push_struct(ui.frame_arena, KeySet);
    if (keyset_has(&app.selected, key)) *keys = app.selected; else keyset_add(keys, key);
    remove_open(keys);
}

static void browse_link(void);
static void changes_open(String8 id); // changes.c

static B32 draw_installed(Rect list)
{
    Arena *fa = ui.frame_arena;
    F32 s = ui.scale;
    browse_link(); // the catalog's two-source addons (their other pill)
    Snapshot *snap = app.snap;
    // What's listed: installs on their way first (feedback.c), then the rows, minus those being removed.
    PendingInstall *pend[8];
    U32 npend = feedback_installs(pend, ArrayCount(pend));
    U64 nrows = snap ? snap->row_count : 0;
    U32 *vis = push_array(fa, U32, nrows + 1);
    U64 nvis = 0;
    for (U64 r = 0; r < nrows; r += 1) if (!row_hidden(snap->rows[r].key)) vis[nvis++] = (U32)r;
    U64 count = npend + nvis;
    F32 row_h = ADDON_ROW_H * s;
    UI_List l = ui_list_begin(&app.scroll[VIEW_INSTALLED], list, (S64)count, row_h, 0);
    B32 animating = l.animating;

    // While a listing is on its way: a thin sliding bar along the top of the list. What's shown
    // meanwhile is this flavor's last listing (or a "Loading" line the first time).
    if (app.wow_root.size && (!snap || app.refresh_served < app.refresh_asked)) {
        animating |= ui_loading_bar(list);
    }

    F32 actions_w = 250 * s; // Overwrite edits, Remove and the pin
    F32 col_actions = list.x1 - 16 * s - actions_w;

    for (S64 i = l.first; i < l.last; i += 1) {
        String8 key = i < (S64)npend ? str8_pushf(fa, "pending %u", pend[i]->task) : snap->rows[vis[i - npend]].key;
        Rect rr = ui_list_row(&l, i, key);
        F32 ry = rr.y0;
        if (i < (S64)npend) {
            // An install on its way: its picture and name, and how far it is.
            PendingInstall *p = pend[i];
            RowLook look = { 0 };
            look.rr = rr; look.list = list; look.lead = 26 * s; look.text_right = col_actions - 8 * s;
            look.icon_url = str8_cstring(p->icon); look.icon_src = p->src;
            look.title = str8_cstring(p->name); look.title_color = C_DIM;
            look.line2 = p->task > app.tasks_done + 1 ? str8_lit("Waiting for wam's other work to finish first") : str8_lit("Downloading and installing");
            look.line2_color = C_FAINT;
            draw_row_look(&look);
            F32 pbh = 28 * s, pby = ry + (row_h - pbh) * 0.5f;
            animating |= draw_install_progress(rect(rr.x1 - 16 * s - 140 * s, pby, rr.x1 - 16 * s, pby + pbh), p);
            continue;
        }
        Row *row = &snap->rows[vis[i - npend]];
        B32 checked = keyset_has(&app.selected, row->key);
        B32 row_hot = ui_mouse_in(rr) && ui_mouse_in(list);
        if (checked) draw_row_fill(rr, C_SELECTED);
        else if (row_hot) draw_row_fill(rr, C_HOVER);

        // The same look as Get addons: picture, name and what's new, the description, then facts
        // and sources (links to their pages: where wam installs it from first).
        // The catalog knows its picture and summary: under its tracked id, else an id its .toc names.
        SearchResult *e = 0;
        for (U32 k = 0; k < row->cat_id_count && !e; k += 1) e = cat_find(&app.cat, row->cat_ids[k]);
        B32 e_is_tracked = e && str8_match(e->id, row->id, 0);
        RowLook look = { 0 };
        look.rr = rr; look.list = list; look.lead = 26 * s; look.text_right = col_actions - 8 * s;
        look.icon_url = e ? e->icon : str8(0, 0);
        if (!look.icon_url.size && id_is(row->id, "gh:")) {
            // A GitHub addon outside the catalog: its owner's picture.
            String8 repo = id_rest(row->id);
            U64 slash = str8_find(repo, 0, str8_lit("/"), 0);
            look.icon_url = str8_pushf(fa, "https://avatars.githubusercontent.com/%.*s?s=64", (int)slash, repo.str);
        }
        look.icon_src = row->id.size ? source_of_id(row->id) : -1;
        look.title = row->title; look.title_color = C_TEXT;
        look.pill_count = row->link_count;
        for (U32 k = 0; k < row->link_count; k += 1) { look.pill_src[k] = row->link_src[k]; look.pill_url[k] = row->links[k]; look.pill_filled[k] = k == 0 && row->link_primary; }
        // The catalog knows it on another source too (cat_link), though its .toc doesn't say so.
        SearchResult *twin = cat_twin(&app.cat, e);
        if (twin && source_of_id(twin->id) >= 0 && twin->website.size && look.pill_count < ArrayCount(look.pill_src)) {
            B32 listed = 0;
            for (U32 k = 0; k < look.pill_count; k += 1) listed |= look.pill_src[k] == source_of_id(twin->id);
            if (!listed) { look.pill_src[look.pill_count] = source_of_id(twin->id); look.pill_url[look.pill_count++] = twin->website; }
        }
        look.site_only = e_is_tracked ? e->site_only : 0; // only says how wam installs it when that's where from
        // An update: "→ version", a link to what it brings (the changes popup).
        if (row->update) {
            String8 latest = row->latest.size ? row->latest : str8_lit("new version");
            look.extra = row->pinned ? str8_pushf(fa, "pinned  \xC2\xB7  %.*s available", S8F(latest))
                       : str8_pushf(fa, "\xE2\x86\x92 %.*s%s", S8F(latest), row->edited ? "  \xC2\xB7  edited locally" : "");
            look.extra_color = row->pinned ? C_DIM : row->edited ? C_WARN : C_GREEN;
            look.extra_link = 1;
        } else if (row->pinned) {
            look.extra = str8_lit("pinned");
            look.extra_color = C_DIM;
        }
        String8List meta = { 0 };
        String8 author = row->author.size ? row->author : e ? e->author : str8(0, 0);
        if (author.size) str8_list_push(fa, &meta, author);
        if (row->version.size) str8_list_push(fa, &meta, row->version);
        if (row->iface.size) str8_list_push(fa, &meta, str8_pushf(fa, "for %.*s", S8F(fmt_interface(fa, row->iface))));
        look.meta = str8_list_join(fa, &meta, str8_lit("  \xC2\xB7  "));
        // Under it: the summary (catalog, else the .toc's notes), else its folders.
        look.line2 = e && e->summary.size ? e->summary : row->notes.size ? row->notes : row->sub;
        look.line2_color = C_DIM;
        B32 pressed_pill = draw_row_look(&look);
        if (look.extra_link && ui_mouse_in(look.extra_rect) && ui_mouse_in(list)) {
            ui_tooltip(look.extra_rect, str8_lit("What's new in this update"));
            if (ui_clicked(look.extra_rect)) changes_open(row->id);
        }

        // Checkbox; clicking the row's text toggles it too (but not a click on a link).
        Rect name_area = rect(rr.x0, ry, col_actions - 8 * s, ry + row_h);
        B32 toggle = ui_checkbox(str8_lit("select"), rr.x0 + 18 * s, ry + row_h * 0.5f, checked);
        if (!toggle && !pressed_pill && ui_mouse_in(name_area) && ui_mouse_in(list) && ui.in.released && point_in(name_area, ui.in.press_x, ui.in.press_y)) toggle = 1;
        if (toggle) { if (checked) keyset_remove(&app.selected, row->key); else keyset_add(&app.selected, row->key); }

        // Actions, right-aligned.
        F32 bh = 28 * s, by = ry + (row_h - bh) * 0.5f, x = rr.x1 - 16 * s;
        String8 remove_label = str8_lit("Remove");
        Rect rb = rect(x - ui_button_width(remove_label), by, x, by + bh);
        if (ui_button(rb, remove_label, C_TEXT, C_BUTTON, C_BUTTON_HI)) {
            start_remove(row->key);
        }
        x = rb.x0 - 8 * s;
        if (row->update && !row->pinned && app.updating.count && keyset_has(&app.updating, row->key)) {
            String8 busy = str8_lit("Updating...");
            animating |= ui_busy_label(x, ry + row_h * 0.5f, busy);
            x -= f_text_width(ui.font.normal, busy) + 32 * s;
        } else if (row->update && !row->pinned) {
            // Website-only files open CurseForge's download page in a window: the arrow says so.
            B32 via_page = site_only_of(row->id) == SITE_ONLY_PAGE;
            String8 label = row->edited ? str8_lit("Overwrite edits###update") : via_page ? str8_lit("Update \xE2\x86\x97###update") : str8_lit("Update###update");
            Rect ub = rect(x - ui_button_width(label), by, x, by + bh);
            // A single-row update is explicit, so it also replaces local edits (the label says so).
            if (ui_button(ub, label, C_TEXT, row->edited ? ui_hex(0x6b4a12, 1) : C_ACCENT_BG, row->edited ? ui_hex(0x86601c, 1) : C_ACCENT)) {
                String8List one = { 0 };
                str8_list_push(fa, &one, row->key);
                feedback_update(push_task_ex(TASK_UPDATE, row->key, 1), &one);
            }
            x = ub.x0 - 8 * s;
        }
        if (row->id.size) {
            // Pin: keep this version. Shown while pinned (lit), else while the row is hovered.
            Rect pb = rect(x - bh, by, x, by + bh);
            B32 hot = ui_mouse_in(pb) && ui_mouse_in(list);
            if (row->pinned || row_hot) {
                if (hot) r_rect(pb, C_BUTTON_HI, 6 * s, 0, 0.5f);
                ui_text_centered(ui.font.glyphs_big, pb, row->pinned ? str8_lit("\xEE\xA1\x82") : str8_lit("\xEE\x9C\x98"), // PinnedFill / Pin
                                 row->pinned ? C_ACCENT_HI : hot ? C_TEXT : C_FAINT);
                if (hot) ui_tooltip(pb, row->pinned ? str8_lit("Pinned: updates leave it at this version. Click to unpin.")
                                                    : str8_lit("Pin: keep this version (updates leave it alone)"));
                if (ui_clicked(pb)) push_task_ex(TASK_PIN, row->id, !row->pinned);
            }
        } else if (!row->id.size) {
            String8 label = str8_lit("not tracked");
            F32 w = f_text_width(ui.font.caption, label);
            f_draw_text(ui.font.caption, x - w, ry + (row_h - ui.font.caption->line_height) * 0.5f, label, C_FAINT, 0);
        }
    }

    if (count == 0 && !app.wow_root.size) {
        // Nothing found: say so and offer the folder picker right here.
        ui_text_centered(ui.font.normal, rect(list.x0, list.y0 + 40 * s, list.x1, list.y0 + 70 * s),
                           str8_lit("wam couldn't find World of Warcraft on this PC."), C_DIM);
        ui_text_centered(ui.font.caption, rect(list.x0, list.y0 + 70 * s, list.x1, list.y0 + 92 * s),
                           str8_lit("Pick the folder that has _retail_, _classic_era_ or another game folder in it (e.g. C:\\Program Files (x86)\\World of Warcraft)."), C_FAINT);
        String8 label = str8_lit("Choose World of Warcraft folder...");
        F32 bw = ui_button_width(label), cx = (list.x0 + list.x1) * 0.5f, by = list.y0 + 108 * s;
        if (ui_button(rect(cx - bw * 0.5f, by, cx + bw * 0.5f, by + 32 * s), label, C_TEXT, C_ACCENT_BG, C_ACCENT)) wm_defer(browse_for_root);
    } else if (count == 0) {
        String8 msg = !snap ? str8_pushf(ui.frame_arena, "Loading %s addons...", wow_flavors[app.flavor].label)
                    : str8_lit("No addons installed for this flavor yet. Find some under Get addons, or drop a .zip / .rar here.");
        ui_text_centered(ui.font.normal, rect(list.x0, list.y0, list.x1, list.y0 + 120 * s), msg, C_DIM);
    }
    ui_list_end(&l);
    return animating;
}

static B32 snapshot_has_id(String8 id)
{
    if (!app.snap) return 0;
    for (U64 i = 0; i < app.snap->row_count; i += 1) if (str8_match(app.snap->rows[i].id, id, 1)) return 1;
    return 0;
}

//- Get addons: browse and search the catalog
//
// The list is the catalog for this flavor (core/catalog.c), filtered and sorted locally, so every
// keystroke, sort and filter answers at once. With an empty query it browses (addons for this
// game version, most downloaded first); with a query it fuzzy-matches names. A moment after typing
// stops, the query also goes to CurseForge / Wago on the suggestion lane; scrolling near the end
// asks CurseForge for the next page of the same sort. What comes back joins the catalog, and the
// list re-sorts without moving the row under the mouse or the selected one.

// Delayed requests (after typing pauses, after the selection rests). A timer (wm_timer_start) wakes
// an idle window, but only fires when nothing is waiting to be drawn, so while something animates
// the frame checks the deadlines itself (frame_timers).
static U64 suggest_due_ms, details_due_ms; // 0 = nothing pending
static void suggest_timer(void);
static void details_timer(void);
static void frame_timers(void)
{
    U64 now = os_now_ms();
    if (suggest_due_ms && now >= suggest_due_ms) suggest_timer();
    if (details_due_ms && now >= details_due_ms) details_timer();
}

static String8 fmt_ago(Arena *a, U64 mtime);
static char *sort_names[SORT_COUNT] = { "Best match", "Most downloaded", "Updated", "New" };
static const U64 sort_prefetched[SORT_COUNT] = { 0, CAT_PAGES_DOWNLOADS * CF_PAGE_SIZE, CAT_PAGES_UPDATED * CF_PAGE_SIZE, CAT_PAGES_RELEASED * CF_PAGE_SIZE };

static String8 browse_query(void) { return str8_trim(str8((U8 *)app.query.s, app.query.len)); }
static BrowseSort browse_sort(void)
{
    if (browse_query().size) return app.sort_query;
    return app.sort_browse == SORT_BEST ? SORT_DOWNLOADS : app.sort_browse; // nothing to match against
}

// Fetch (or refetch) the catalog for this flavor: the disk cache first, then the network if needed.
static void catalog_request(B32 force)
{
    if (!app.wow_root.size) return;
    push_task_ex(TASK_CATALOG, str8(0, 0), force);
}

static void catalog_reset(void)
{
    if (!app.cat.arena) app.cat.arena = arena_alloc(GB(1));
    arena_clear(app.cat.arena);
    cat_init(&app.cat, app.cat.arena);
    memset(app.src_count, 0, sizeof(app.src_count));
    app.catalog_saved = 0;
    app.catalog_failed = 0;
    app.sugg_last_query[0] = 0;
    app.sel_id[0] = app.sel_restore[0] = 0;
    app.sel_index = app.hover_index = -1;
    app.order_valid = 0;
    app.more_done = 0;
    app.more_query[0] = 0;
    app.more_sort = -1;
    // Details depend on the game version too (which file, which changelog).
    for (U32 i = 0; i < DETAIL_CACHE; i += 1) if (app.details[i]) { arena_release(app.details[i]->arena); app.details[i] = 0; }
    app.detail_want[0] = app.detail_asked[0] = 0;
    catalog_request(0);
}

static void catalog_merge(SearchResult *v, U64 count)
{
    for (U64 i = 0; i < count; i += 1) {
        B32 is_new = !cat_find(&app.cat, v[i].id);
        SearchResult *e = cat_put(&app.cat, &v[i]);
        int src = e ? source_of_id(e->id) : -1;
        if (is_new && e && src >= 0) app.src_count[src] += 1;
    }
}

//- One addon on two sources: one row
//
// An addon on both CurseForge and Wago (cat_link pairs their entries) is one row in Get addons. The
// row is its install source's entry (cat_install_pick), or the other one when the source chips hide
// that source; the other source's pill is outlined after it. Its facts are both entries' together.

// Pairs the catalog's entries (again, when the catalog or the installed addons changed); an
// installed addon whose .toc names both ids pairs them for sure.
static void browse_link(void)
{
    S32 seq = app.snap ? app.snap->seq : 0;
    if (app.link_valid && app.link_cat_version == app.cat.version && app.link_snap == app.snap && app.link_snap_seq == seq) return;
    app.link_valid = 1;
    app.link_cat_version = app.cat.version;
    app.link_snap = app.snap;
    app.link_snap_seq = seq;
    Arena *fa = ui.frame_arena;
    U64 n = app.snap ? app.snap->row_count : 0, count = 0;
    CatPair *known = push_array(fa, CatPair, n + 1);
    for (U64 i = 0; i < n; i += 1) {
        Row *row = &app.snap->rows[i];
        String8 cf = { 0 }, wago = { 0 };
        for (U32 k = 0; k < row->cat_id_count; k += 1) {
            if (!cf.size && id_is(row->cat_ids[k], "cf:")) cf = row->cat_ids[k];
            if (!wago.size && id_is(row->cat_ids[k], "wago:")) wago = row->cat_ids[k];
        }
        if (cf.size && wago.size) known[count++] = (CatPair){ cf, wago };
    }
    cat_link(&app.cat, known, count);
}

static B32 src_shown(SearchResult *e)
{
    int src = source_of_id(e->id);
    return src < 0 || !(app.src_hidden & (1u << src));
}

// The entry e's row shows and installs (e itself unless it has a twin).
static SearchResult *row_entry(SearchResult *e)
{
    SearchResult *t = cat_twin(&app.cat, e);
    if (!t) return e;
    SearchResult *pick = cat_install_pick(&app.cat, e, app.wago_key[0] != 0), *other = pick == e ? t : e;
    return !src_shown(pick) && src_shown(other) ? other : pick;
}

typedef struct RowFacts { B32 available; S64 downloads, updated, created; U32 cats; U64 subcats; } RowFacts;

// Downloads add up; the latest update, the first release; both sources' categories.
static RowFacts row_facts(SearchResult *e)
{
    RowFacts f = { e->available, e->downloads, e->updated, e->created, e->cats, e->subcats };
    SearchResult *t = cat_twin(&app.cat, e);
    if (t) {
        f.available |= t->available;
        f.downloads += Max(t->downloads, 0);
        f.updated = Max(f.updated, t->updated);
        if (t->created > 0 && (!f.created || t->created < f.created)) f.created = t->created;
        f.cats |= t->cats;
        f.subcats |= t->subcats;
    }
    return f;
}

// Installed from either source.
static SearchResult *row_installed(SearchResult *e)
{
    if (snapshot_has_id(e->id)) return e;
    SearchResult *t = cat_twin(&app.cat, e);
    return t && snapshot_has_id(t->id) ? t : 0;
}

// The row's id matches e's or its twin's.
static B32 row_has_id(SearchResult *e, String8 id)
{
    SearchResult *t = cat_twin(&app.cat, e);
    return str8_match(e->id, id, 0) || (t && str8_match(t->id, id, 0));
}

static int order_compare_sort; // for qsort
static F32 *order_rank;        // per catalog index, best match only

static int order_compare(const void *pa, const void *pb)
{
    RowFacts x = row_facts(&app.cat.v[*(const U32 *)pa]), y = row_facts(&app.cat.v[*(const U32 *)pb]);
    if (x.available != y.available) return y.available - x.available; // unavailable last
    S64 kx = 0, ky = 0;
    switch (order_compare_sort) {
        case SORT_BEST: {
            F32 rx = order_rank[*(const U32 *)pa], ry = order_rank[*(const U32 *)pb];
            if (rx != ry) return rx < ry ? 1 : -1;
        } break;
        case SORT_UPDATED: kx = x.updated; ky = y.updated; break;
        case SORT_NEW: kx = x.created; ky = y.created; break;
        default: break;
    }
    if (kx != ky) return ky > kx ? 1 : -1;
    if (x.downloads != y.downloads) return y.downloads > x.downloads ? 1 : -1;
    return (int)(*(const U32 *)pa) - (int)(*(const U32 *)pb);
}

// Rebuilds app.order when the query, sort, filters, catalog or installed set changed. Keeps the
// selected row, else the hovered one, else (when scrolled) the top one, where it was on screen.
static void browse_order(F32 row_h)
{
    String8 q = browse_query();
    int sort = browse_sort();
    B32 query_same = str8_match(q, str8_cstring(app.order_query), 0) && q.size < sizeof(app.order_query);
    B32 view_same = app.order_valid && query_same && sort == app.order_sort && app.src_hidden == app.order_hidden && app.hide_installed == app.order_hide_inst
                 && app.category == app.order_category && app.subcategory == app.order_subcategory
                 && (app.wago_key[0] != 0) == app.order_wago_key; // a key changes which source two-source rows install from
    B32 data_same = app.cat.version == app.order_cat_version && app.snap == app.order_snap && (!app.snap || app.snap->seq == app.order_snap_seq);
    if (view_same && data_same) return;
    browse_link();

    // Only new data (not a new query or filter): remember a row to keep in place.
    S64 anchor = -1;
    if (view_same) {
        F32 pos = app.scroll[VIEW_SEARCH].pos;
        S64 first = (S64)(pos / row_h);
        // The hovered row counts unless the user is typing (then the best new match belongs on top).
        B32 typing = os_now_ms() - app.last_type_ms < 600;
        anchor = app.sel_index >= 0 ? app.sel_index : app.hover_index >= 0 && !typing ? app.hover_index : pos > 0 ? first : -1;
        if (anchor >= (S64)app.order_count) anchor = -1;
    }
    char anchor_id[160] = { 0 };
    if (anchor >= 0) copy_cstr(anchor_id, sizeof(anchor_id), app.cat.v[app.order[anchor]].id);

    Arena *fa = ui.frame_arena;
    U64 n = app.cat.count;
    U8 *installed = push_array(fa, U8, n + 1);
    if (app.snap) {
        for (U64 i = 0; i < app.snap->row_count; i += 1) {
            SearchResult *e = cat_find(&app.cat, app.snap->rows[i].id);
            if (e) installed[e - app.cat.v] = 1;
        }
    }
    if (!app.order_arena) app.order_arena = arena_alloc(GB(1));
    arena_clear(app.order_arena);
    app.order = push_array(app.order_arena, U32, n + 1);
    order_rank = push_array(fa, F32, n + 1);
    S32 *score = push_array(fa, S32, n + 1);
    S32 best = 0;
    U64 count = 0;
    for (U64 i = 0; i < n; i += 1) {
        SearchResult *e = &app.cat.v[i];
        if (row_entry(e) != e) continue; // an addon on two sources: one row, its twin's
        if (!src_shown(e)) continue;
        SearchResult *t = cat_twin(&app.cat, e);
        if (app.hide_installed && (installed[i] || (t && installed[t - app.cat.v]))) continue;
        RowFacts facts = row_facts(e);
        if (!q.size) {
            if (!facts.available) continue; // browsing: only what installs on this game version
        } else {
            FuzzyMatch fm = fuzzy_match(q, e->name);
            if (t) { FuzzyMatch ft = fuzzy_match(q, t->name); if (ft.score > fm.score) fm = ft; } // either name
            if (fm.score < 0) continue;
            score[i] = fm.score;
            best = Max(best, fm.score);
            // Popularity breaks near-ties: "dbm" should put Deadly Boss Mods (300M downloads) above a
            // 3M-download plugin whose name merely starts with DBM. About +6 per tenfold downloads.
            F32 pop = facts.downloads > 0 ? (F32)(log10((F64)facts.downloads) * 6.0) : 0;
            // The exact name beats any popularity ("elvui" -> ElvUI itself, though Tukui reports no downloads).
            if (str8_match(str8_trim(e->name), q, 1) || (t && str8_match(str8_trim(t->name), q, 1))) pop += 60;
            order_rank[i] = (F32)fm.score + pop;
        }
        app.order[count++] = (U32)i;
    }
    // A query keeps only reasonable matches: with thousands of names, any few letters appear in
    // order somewhere, and those stragglers would crowd the other sorts.
    if (q.size) {
        U64 kept = 0;
        for (U64 k = 0; k < count; k += 1) if (score[app.order[k]] * 10 >= best * 7) app.order[kept++] = app.order[k];
        count = kept;
    }
    // Category counts (for the menu) among what the other filters show, then the category itself.
    memset(app.cat_counts, 0, sizeof(app.cat_counts));
    memset(app.sub_counts, 0, sizeof(app.sub_counts));
    app.cat_any = count;
    U32 want = app.category >= 0 ? 1u << app.category : 0;
    U64 want_sub = app.subcategory >= 0 ? 1ull << app.subcategory : 0;
    U64 kept_cat = 0;
    for (U64 k = 0; k < count; k += 1) {
        RowFacts f = row_facts(&app.cat.v[app.order[k]]);
        for (U32 b = 0; b < CAT_CATEGORY_COUNT; b += 1) app.cat_counts[b] += (f.cats >> b) & 1;
        for (U32 b = 0; b < CAT_SUBCATEGORY_COUNT; b += 1) app.sub_counts[b] += (f.subcats >> b) & 1;
        if (want_sub ? (f.subcats & want_sub) != 0 : !want || (f.cats & want)) app.order[kept_cat++] = app.order[k];
    }
    count = kept_cat;
    order_compare_sort = sort;
    qsort(app.order, count, sizeof(U32), order_compare);
    app.order_count = count;

    // Where the selection and the anchor went.
    app.sel_index = -1;
    S64 anchor_new = -1;
    for (U64 k = 0; k < count && (app.sel_id[0] || anchor_id[0]); k += 1) {
        SearchResult *e = &app.cat.v[app.order[k]];
        if (app.sel_id[0] && app.sel_index < 0 && row_has_id(e, str8_cstring(app.sel_id))) app.sel_index = (S64)k;
        if (anchor_id[0] && anchor_new < 0 && row_has_id(e, str8_cstring(anchor_id))) anchor_new = (S64)k;
    }
    if (app.sel_id[0]) app.sel_restore[0] = 0; // the user picked something else first
    for (U64 k = 0; k < count && app.sel_restore[0]; k += 1) {
        if (!row_has_id(&app.cat.v[app.order[k]], str8_cstring(app.sel_restore))) continue;
        app.sel_index = (S64)k;
        app.sel_id[0] = 1; // copied below
        app.sel_restore[0] = 0;
        app.sel_reveal = 1;
    }
    if (app.sel_index < 0) app.sel_id[0] = 0;
    else copy_cstr(app.sel_id, sizeof(app.sel_id), app.cat.v[app.order[app.sel_index]].id); // the row's entry now
    if (anchor >= 0 && anchor_new >= 0 && anchor_new != anchor) {
        F32 d = (F32)(anchor_new - anchor) * row_h;
        app.scroll[VIEW_SEARCH].pos = Max(app.scroll[VIEW_SEARCH].pos + d, 0);
        app.scroll[VIEW_SEARCH].target = Max(app.scroll[VIEW_SEARCH].target + d, 0);
    }
    if (!view_same) app.hover_index = -1;

    copy_cstr(app.order_query, sizeof(app.order_query), q);
    app.order_sort = sort;
    app.order_hidden = app.src_hidden;
    app.order_hide_inst = app.hide_installed;
    app.order_category = app.category;
    app.order_subcategory = app.subcategory;
    app.order_wago_key = app.wago_key[0] != 0;
    app.order_cat_version = app.cat.version;
    app.order_snap = app.snap;
    app.order_snap_seq = app.snap ? app.snap->seq : 0;
    app.order_valid = 1;
}

// The CurseForge category the list is filtered by (0: all).
static S64 browse_cf_category(void)
{
    return app.subcategory >= 0 ? cat_subcategories[app.subcategory].cf_id : app.category >= 0 ? cat_categories[app.category].cf_id : 0;
}

// Asks the suggestion lane for one page of (sort, query) results.
static void request_page(int sort, String8 q, U64 index)
{
    Task t = make_task(TASK_SUGGEST, q, 0, app.flavor);
    t.sort = sort;
    t.index = index;
    t.category = browse_cf_category();
    dispatch_task(t);
    app.more_asked = app.suggest_asked;
    app.more_waiting = 1;
    app.more_rows = app.order_count;
}

#define MORE_EMPTY_MAX 6

// The paging state follows the current sort + query.
static void paging_sync(void)
{
    // Other filters show other rows: pages may be worth asking for again.
    U64 filters = ((U64)(app.category + 1) << 40) ^ ((U64)(app.subcategory + 1) << 24) ^ ((U64)app.src_hidden << 1) ^ (U64)(app.hide_installed != 0);
    if (filters != app.more_filters) { app.more_filters = filters; app.more_empty = 0; app.more_waiting = 0; }
    String8 q = browse_query();
    int sort = browse_sort();
    S64 category = browse_cf_category();
    if (sort == app.more_sort && category == app.more_category && str8_match(q, str8_cstring(app.more_query), 0)) return;
    // The same search in another category: ask it again there (its first page isn't one of these).
    B32 again = q.size && category != app.more_category;
    app.more_sort = sort;
    app.more_category = category;
    copy_cstr(app.more_query, sizeof(app.more_query), q);
    app.more_index = q.size || category ? 0 : sort_prefetched[sort]; // browsing it all starts after what the catalog prefetched
    app.more_done = !app.cf_key[0];
    app.more_empty = 0;
    app.more_waiting = 0;
    if (again && !suggest_due_ms) { app.sugg_last_query[0] = 0; suggest_due_ms = os_now_ms(); wm_timer_start(suggest_timer, 1); }
}

// Typing changed the query: back to the top, and ask the network once typing pauses.
static void query_changed(void)
{
    app.sel_id[0] = app.sel_restore[0] = 0;
    app.sel_index = -1;
    app.last_type_ms = os_now_ms();
    app.scroll[VIEW_SEARCH].pos = app.scroll[VIEW_SEARCH].target = 0;
    wm_timer_start(suggest_timer, 150);
    suggest_due_ms = os_now_ms() + 150;
}

static void suggest_timer(void)
{
    wm_timer_stop(suggest_timer);
    suggest_due_ms = 0;
    if (app.view != VIEW_SEARCH) return;
    String8 q = browse_query();
    if (q.size < 2 || q.size >= sizeof(app.sugg_last_query)) return;
    String8 id = mgr_parse_spec(ui.frame_arena, q);
    if (id.size && !id_is(id, "cf:")) return; // a pasted link, not a name
    int sort = browse_sort();
    if (str8_match(q, str8_cstring(app.sugg_last_query), 1) && sort == app.sugg_last_sort) return;
    copy_cstr(app.sugg_last_query, sizeof(app.sugg_last_query), q);
    app.sugg_last_sort = sort;
    paging_sync();
    request_page(sort, q, 0);
}

// A page (or a typed search) came back.
static void search_result_arrived(SearchSnap *ss)
{
    if (ss->flavor != app.flavor || ss->gen != app.root_gen) return;
    catalog_merge(ss->v, ss->count);
    if (ss->sort == app.more_sort && ss->category == app.more_category && str8_match(ss->query, str8_cstring(app.more_query), 0)) {
        if (ss->failed || ss->cf_count < CF_PAGE_SIZE) app.more_done = 1;
        app.more_index = Max(app.more_index, ss->index + CF_PAGE_SIZE);
        if (app.more_index > CF_INDEX_MAX) app.more_done = 1;
    }
}

static void catalog_arrived(CatalogSnap *cs)
{
    if (cs->flavor != app.flavor || cs->gen != app.root_gen) return;
    if (cs->cat.count) {
        catalog_merge(cs->cat.v, cs->cat.count);
        app.catalog_saved = Max(app.catalog_saved, cs->saved);
        app.catalog_failed = 0;
    } else if (cs->final && !app.cat.count) {
        app.catalog_failed = 1;
        if (cs->error.size) worker_set_status(str8_pushf(ui.frame_arena, "Could not load the addon list (%.*s)", S8F(cs->error)), 1);
    }
}

static void browse_install(SearchResult *r)
{
    SearchResult *have = row_installed(r); // from either source
    if (have) { worker_set_status(str8_pushf(ui.frame_arena, "%.*s is already installed (from %s)", S8F(r->name), have->source ? have->source : "elsewhere"), 0); return; }
    if (!r->available) { worker_set_status(str8_pushf(ui.frame_arena, "%.*s has no download for this game version", S8F(r->name)), 1); return; }
    if (feedback_installing(r->id)) return; // already on its way
    feedback_install(push_task(TASK_INSTALL, r->id), r->id, r->name, r->icon);
}

static void browse_select(S64 index)
{
    if (index < 0 || index >= (S64)app.order_count) { app.sel_id[0] = 0; app.sel_index = -1; return; }
    app.sel_index = index;
    copy_cstr(app.sel_id, sizeof(app.sel_id), app.cat.v[app.order[index]].id);
    app.sel_reveal = 1;
}

// Up / Down / Page Up / Page Down / Home / End in Get addons. Returns 1 if it used the key.
static B32 browse_key(WM_Key key, B32 ctrl)
{
    if (!app.order_count) return 0;
    S64 page = 8, i = app.sel_index;
    switch (key) {
        case WM_KEY_DOWN:      browse_select(i < 0 ? 0 : Min(i + 1, (S64)app.order_count - 1)); return 1;
        case WM_KEY_UP:        browse_select(i <= 0 ? -1 : i - 1); return 1; // up from the first row: back to the field
        case WM_KEY_PAGE_DOWN: browse_select(Min(Max(i, 0) + page, (S64)app.order_count - 1)); return 1;
        case WM_KEY_PAGE_UP:   browse_select(Max(i - page, 0)); return 1;
        case WM_KEY_HOME:      if (ctrl) { browse_select(0); return 1; } return 0;
        case WM_KEY_END:       if (ctrl) { browse_select((S64)app.order_count - 1); return 1; } return 0;
        default: return 0;
    }
}

static B32 browse_enter(void)
{
    if (app.sel_index < 0 || app.sel_index >= (S64)app.order_count) return 0;
    browse_install(&app.cat.v[app.order[app.sel_index]]);
    return 1;
}

// The category menu, with how many addons each has under the other filters. A category with
// subcategories opens a submenu: the whole category first, then each subcategory.
static void show_category_menu(void)
{
    Arena *a = ui.frame_arena;
    enum { MAX = 4 + CAT_CATEGORY_COUNT * 3 + CAT_SUBCATEGORY_COUNT };
    WM_MenuItem items[MAX];
    S32 cat_of[MAX], sub_of[MAX]; // what each item picks; cat -2 = nothing (separator, submenu)
    U32 n = 0;
    #define MENU_ADD(label_, parent_, checked_, cat_, sub_) do { items[n] = (WM_MenuItem){ label_, parent_, checked_ }; cat_of[n] = cat_; sub_of[n] = sub_; n += 1; } while (0)
    MENU_ADD(str8_pushf(a, "All categories  (%.*s)", S8F(str8_thousands(a, (S64)app.cat_any))), -1, app.category < 0, -1, -1);
    MENU_ADD(str8_lit("-"), -1, 0, -2, -1);
    for (U32 c = 0; c < CAT_CATEGORY_COUNT; c += 1) {
        if (!app.cat_counts[c] && (S32)c != app.category) continue;
        String8 label = str8_pushf(a, "%s  (%.*s)", cat_categories[c].name, S8F(str8_thousands(a, (S64)app.cat_counts[c])));
        B32 has_subs = 0;
        for (U32 sc = 0; sc < CAT_SUBCATEGORY_COUNT && !has_subs; sc += 1)
            has_subs = cat_sub_parent(sc) == (S32)c && (app.sub_counts[sc] || (S32)sc == app.subcategory);
        if (!has_subs) { MENU_ADD(label, -1, (S32)c == app.category, (S32)c, -1); continue; }
        S32 parent = (S32)n;
        MENU_ADD(label, -1, (S32)c == app.category, -2, -1);
        MENU_ADD(str8_pushf(a, "All %s  (%.*s)", cat_categories[c].name, S8F(str8_thousands(a, (S64)app.cat_counts[c]))), parent,
                 (S32)c == app.category && app.subcategory < 0, (S32)c, -1);
        MENU_ADD(str8_lit("-"), parent, 0, -2, -1);
        for (U32 sc = 0; sc < CAT_SUBCATEGORY_COUNT; sc += 1) {
            if (cat_sub_parent(sc) != (S32)c || (!app.sub_counts[sc] && (S32)sc != app.subcategory)) continue;
            MENU_ADD(str8_pushf(a, "%s  (%.*s)", cat_subcategories[sc].name, S8F(str8_thousands(a, (S64)app.sub_counts[sc]))), parent,
                     (S32)sc == app.subcategory, (S32)c, (S32)sc);
        }
    }
    #undef MENU_ADD
    S32 pick = wm_popup_menu_tree(app.cat_btn, items, n);
    if (pick >= 0 && cat_of[pick] != -2 && (cat_of[pick] != app.category || sub_of[pick] != app.subcategory)) {
        app.category = cat_of[pick];
        app.subcategory = sub_of[pick];
        app.scroll[VIEW_SEARCH].pos = app.scroll[VIEW_SEARCH].target = 0;
        wm_request_frame();
    }
}

static void browse_set_sort(BrowseSort sort)
{
    B32 searching = browse_query().size > 0;
    if (searching) app.sort_query = sort; else app.sort_browse = sort;
    app.scroll[VIEW_SEARCH].pos = app.scroll[VIEW_SEARCH].target = 0;
    app.sel_id[0] = app.sel_restore[0] = 0;
    if (searching) { app.sugg_last_query[0] = 0; suggest_due_ms = os_now_ms(); wm_timer_start(suggest_timer, 1); } // results for this sort too
    wm_request_frame();
}

// "Best match" is only a sort while there's a query to match.
static void show_sort_menu(void)
{
    String8 items[SORT_COUNT];
    int first = browse_query().size ? 0 : 1;
    U32 n = 0;
    for (int k = first; k < SORT_COUNT; k += 1) items[n++] = str8_cstring(sort_names[k]);
    S32 pick = wm_popup_menu(app.sort_btn, items, n, (S32)browse_sort() - first);
    if (pick >= 0) browse_set_sort((BrowseSort)(pick + first));
}

static const int filter_sources[] = { SRC_CURSEFORGE, SRC_WOWI, SRC_WAGO, SRC_TUKUI };

// Each source, checked while it's shown: picking one turns it on or off.
static void show_sources_menu(void)
{
    WM_MenuItem items[2 + ArrayCount(filter_sources)];
    int src_of[ArrayCount(items)];
    U32 n = 0;
    items[n] = (WM_MenuItem){ str8_lit("All sources"), -1, 0 }; src_of[n++] = -1;
    items[n] = (WM_MenuItem){ str8_lit("-"), -1, 0 }; src_of[n++] = -1;
    for (U32 k = 0; k < ArrayCount(filter_sources); k += 1) {
        int src = filter_sources[k];
        if (!app.src_count[src]) continue;
        items[n] = (WM_MenuItem){ str8_cstring(source_names[src]), -1, !(app.src_hidden & (1u << src)) };
        src_of[n++] = src;
    }
    S32 pick = wm_popup_menu_tree(app.src_btn, items, n);
    if (pick < 0) return;
    if (src_of[pick] < 0) app.src_hidden = 0;
    else app.src_hidden ^= 1u << src_of[pick];
    app.scroll[VIEW_SEARCH].pos = app.scroll[VIEW_SEARCH].target = 0;
    wm_request_frame();
}

// What the Sources filter says: all of them, the one or two shown, or how many.
static String8 sources_label(Arena *a)
{
    String8List shown = { 0 };
    U32 have = 0;
    for (U32 k = 0; k < ArrayCount(filter_sources); k += 1) {
        int src = filter_sources[k];
        if (!app.src_count[src]) continue;
        have += 1;
        if (!(app.src_hidden & (1u << src))) str8_list_push(a, &shown, str8_cstring(source_names[src]));
    }
    if (shown.count == have) return str8_lit("All");
    if (!shown.count) return str8_lit("None");
    if (shown.count <= 2) return str8_list_join(a, &shown, str8_lit(", "));
    return str8_pushf(a, "%u of %u", (U32)shown.count, have);
}

// A filter: its name, faint, then what it's set to (at most value_max wide) and an arrow. `on`: it
// narrows the list, so it's drawn brighter. *x moves past it. True when clicked; *out is where it is.
static B32 draw_filter(F32 *x, F32 y, F32 h, String8 name, String8 value, F32 value_max, B32 on, Rect *out)
{
    F32 s = ui.scale, pad = 8 * s, gap = 6 * s;
    F_Font *f = ui.font.caption;
    String8 arrow = str8_lit("\xE2\x96\xBE");
    F32 nw = f_text_width(f, name), vw = Min(f_text_width(f, value), Max(value_max, 0)), aw = f_text_width(f, arrow);
    Rect b = rect(*x, y, *x + pad + nw + gap + vw + gap + aw + pad, y + h);
    UI_Id id = ui_id(name);
    UI_Signal sg = ui_signal(id, b, UI_FOCUSABLE);
    F32 t = ui_anim(id, (F32)sg.hovering), ty = (b.y0 + b.y1 - f->line_height) * 0.5f;
    if (t > 0) { V4 c = C_HOVER; c.w *= t; r_rect(b, c, 6 * s, 0, 0.5f); }
    F32 tx = b.x0 + pad;
    f_draw_text(f, tx, ty, name, C_DIM, 0);
    tx += nw + gap;
    f_draw_text(f, tx, ty, value, on ? C_ACCENT_HI : C_TEXT, vw + 1);
    f_draw_text(f, tx + vw + gap, ty, arrow, ui_mix(C_DIM, C_TEXT, t), 0);
    ui_focus_ring(id, b, 6 * s);
    *x = b.x1 + 6 * s;
    *out = b;
    return sg.clicked;
}

// Sort, Category and Sources (menus), the count and "Hide installed", in one row.
static F32 draw_browse_filters(Rect r)
{
    F32 s = ui.scale;
    Arena *fa = ui.frame_arena;
    F32 h = 26 * s, y = r.y0 + (r.y1 - r.y0 - h) * 0.5f, x = r.x0;

    // Right side first, as the filters stop before it: "Hide installed".
    String8 hl = str8_lit("Hide installed");
    F32 hx = r.x1 - f_text_width(ui.font.caption, hl), cbx = hx - 22 * s;
    f_draw_text(ui.font.caption, hx, r.y0 + (r.y1 - r.y0 - ui.font.caption->line_height) * 0.5f, hl, C_DIM, 0);
    Rect hit = rect(cbx, y, r.x1, y + h);
    B32 toggle = ui_checkbox(str8_lit("hide installed"), cbx, (r.y0 + r.y1) * 0.5f, app.hide_installed);
    if (ui_clicked(hit) && !point_in(rect(cbx - 6 * s, y, cbx + 22 * s, y + h), ui.in.press_x, ui.in.press_y)) toggle = 1;
    if (toggle) app.hide_installed = !app.hide_installed;

    Rect b;
    if (draw_filter(&x, y, h, str8_lit("Sort"), str8_cstring(sort_names[browse_sort()]), 200 * s, 0, &b)) { app.sort_btn = b; wm_defer(show_sort_menu); }
    String8 cat = app.subcategory >= 0 ? str8_pushf(fa, "%s \xE2\x80\xBA %s", cat_categories[app.category].name, cat_subcategories[app.subcategory].name)
                : app.category >= 0 ? str8_cstring(cat_categories[app.category].name) : str8_lit("All");
    if (draw_filter(&x, y, h, str8_lit("Category"), cat, 200 * s, app.category >= 0, &b)) { app.cat_btn = b; wm_defer(show_category_menu); }
    String8 src = sources_label(fa);
    F32 room = cbx - 16 * s - x - f_text_width(ui.font.caption, str8_lit("Sources")) - 40 * s; // what a narrow window leaves
    if (draw_filter(&x, y, h, str8_lit("Sources"), src, room, !str8_match(src, str8_lit("All"), 0), &b)) { app.src_btn = b; wm_defer(show_sources_menu); }

    // Before "Hide installed", when there's room: the count (or what's loading).
    String8 info = app.catalog_served < app.catalog_asked && app.cat.count ? str8_lit("Updating the list...")
                 : str8_pushf(fa, "%.*s addon%s", S8F(str8_thousands(fa, (S64)app.order_count)), app.order_count == 1 ? "" : "s");
    F32 iw = f_text_width(ui.font.caption, info);
    if (cbx - 16 * s - iw > x) f_draw_text(ui.font.caption, cbx - 16 * s - iw, r.y0 + (r.y1 - r.y0 - ui.font.caption->line_height) * 0.5f, info, C_FAINT, 0);
    return r.y1 - r.y0;
}

// An addon's picture, or its initial on its source's color until (or unless) one loads.
static void draw_addon_icon(Rect r, SearchResult *e, F32 radius)
{
    draw_icon(r, e->icon, e->name, source_of_id(e->id) >= 0 ? source_of_id(e->id) : SRC_CURSEFORGE, radius);
}

//- The detail panel

static DetailSnap *details_find(String8 id)
{
    for (U32 i = 0; i < DETAIL_CACHE; i += 1) if (app.details[i] && str8_match(app.details[i]->d.id, id, 0)) return app.details[i];
    return 0;
}

static void details_arrived(DetailSnap *ds)
{
    if (ds->flavor != app.flavor || ds->gen != app.root_gen) { arena_release(ds->arena); return; }
    ds->arrived_ms = os_now_ms();
    for (U32 i = 0; i < DETAIL_CACHE; i += 1) {
        DetailSnap *old = app.details[i];
        if (!old || !str8_match(old->d.id, ds->d.id, 0)) continue;
        if (old->d.ok || !ds->d.ok) { arena_release(ds->arena); return; }
        arena_release(old->arena); app.details[i] = ds; // a retry that worked replaces the failure
        return;
    }
    DetailSnap **slot = &app.details[app.details_next++ % DETAIL_CACHE];
    if (*slot) arena_release((*slot)->arena);
    *slot = ds;
}

// Whether to show that something is being waited for: not for the first moment, or a wait too short to
// notice would flash an indicator. key: what's waited for (a new one starts over).
#define WAIT_SHOWN_AFTER_MS 250
typedef struct Wait { U64 key, since_ms; } Wait;
static Wait detail_wait, gallery_wait;

static void wait_timer(void) { wm_timer_stop(wait_timer); wm_request_frame(); }

static B32 wait_shown(Wait *w, U64 key)
{
    U64 now = os_now_ms();
    if (w->key != key) { w->key = key; w->since_ms = now; }
    if (now - w->since_ms >= WAIT_SHOWN_AFTER_MS) return 1;
    wm_timer_start(wait_timer, (U32)(WAIT_SHOWN_AFTER_MS - (now - w->since_ms)) + 1); // come back then
    return 0;
}

// The panel follows the selection. Details are asked for at once, unless the selection is moving along (it
// changed a moment ago: an arrow key held down, which repeats about 30 times a second); then once it rests,
// so that doesn't send a request per row.
#define DETAILS_REST_MS 120
static void details_timer(void);
static void details_follow(void)
{
    if (str8_match(str8_cstring(app.sel_id), str8_cstring(app.detail_want), 0)) return;
    copy_cstr(app.detail_want, sizeof(app.detail_want), str8_cstring(app.sel_id));
    app.detail_scroll.pos = app.detail_scroll.target = 0;
    app.gallery_index = app.gallery_first = 0;
    static U64 changed_ms;
    U64 now = os_now_ms();
    B32 moving = now - changed_ms < DETAILS_REST_MS;
    changed_ms = now;
    if (!app.sel_id[0]) return;
    if (!moving) { details_timer(); return; }
    wm_timer_start(details_timer, DETAILS_REST_MS);
    details_due_ms = now + DETAILS_REST_MS;
}

static void details_timer(void)
{
    wm_timer_stop(details_timer);
    details_due_ms = 0;
    wm_request_frame(); // (the gallery's large picture waited for this too)
    String8 want = str8_cstring(app.detail_want);
    DetailSnap *have = want.size ? details_find(want) : 0;
    // A failure shown for a few seconds already is asked again (it stays on screen until the answer comes).
    if (have && !have->d.ok && os_now_ms() - have->arrived_ms > 5000) { have = 0; app.detail_asked[0] = 0; }
    if (!want.size || have || str8_match(want, str8_cstring(app.detail_asked), 0)) return;
    copy_cstr(app.detail_asked, sizeof(app.detail_asked), want);
    push_task(TASK_DETAILS, want);
}

typedef struct Shot { String8 thumb, full; } Shot;

// "thumb|full thumb ..." (see SearchResult.shots).
static U32 parse_shots(String8 s, Shot *out, U32 max)
{
    U32 n = 0;
    while (s.size && n < max) {
        U64 sp = str8_find(s, 0, str8_lit(" "), 0);
        String8 item = str8_prefix(s, sp);
        s = str8_skip(s, sp + 1);
        if (!item.size) continue;
        U64 bar = str8_find(item, 0, str8_lit("|"), 0);
        out[n].thumb = str8_prefix(item, bar);
        out[n].full = bar < item.size ? str8_skip(item, bar + 1) : out[n].thumb;
        n += 1;
    }
    return n;
}

//- The picture viewer: one screenshot at full size over the whole window, with the addon's others
// a click or an arrow key away. Esc, ✕ or a click beside the picture closes it.

static void viewer_open(Shot *shots, U32 n, U32 index)
{
    app.viewer_count = 0;
    for (U32 i = 0; i < n && app.viewer_count < ArrayCount(app.viewer_urls); i += 1) {
        if (shots[i].full.size >= sizeof(app.viewer_urls[0])) continue;
        copy_cstr(app.viewer_urls[app.viewer_count++], sizeof(app.viewer_urls[0]), shots[i].full);
    }
    app.viewer_index = Min(index, app.viewer_count ? app.viewer_count - 1 : 0);
    app.viewer_open = app.viewer_count > 0;
    app.viewer_fresh = 1; // the click that opened it isn't one on the viewer
    wm_request_frame();
}

static void viewer_step(int d)
{
    if (!app.viewer_open || app.viewer_count < 2) return;
    app.viewer_index = (U32)(((int)app.viewer_index + d + (int)app.viewer_count) % (int)app.viewer_count);
    wm_request_frame();
}

// Keys while the viewer is open (it takes them all, and the typing). Returns 1 if it was open.
static B32 viewer_event(WM_Event *e)
{
    if (!app.viewer_open) return 0;
    if (e->kind != WM_EVENT_KEY) return 1;
    WM_Key k = e->key;
    if (k == WM_KEY_ESCAPE) app.viewer_open = 0;
    else if (k == WM_KEY_LEFT || k == WM_KEY_PAGE_UP) viewer_step(-1);
    else if (k == WM_KEY_RIGHT || k == WM_KEY_PAGE_DOWN || k == WM_KEY_SPACE) viewer_step(1);
    return 1;
}

// Draws the viewer over everything (the frame set the overlay to the whole window).
static B32 draw_viewer(void)
{
    F32 s = ui.scale, W = ui.width, H = ui.height;
    Arena *fa = ui.frame_arena;
    B32 animating = 0;
    if (app.viewer_fresh) { app.viewer_fresh = 0; ui.in.released = 0; }
    ui_overlay_begin();
    Rect full = rect(0, 0, W, H);
    r_rect(full, ui_hex(0x05070a, 0.9f), 0, 0, 0);
    String8 url = str8_cstring(app.viewer_urls[app.viewer_index]);
    Rect area = rect(72 * s, 56 * s, W - 72 * s, H - 40 * s);
    Rect drawn = rect(0, 0, 0, 0);
    if (!ui_image_full(area, url, 6 * s, &drawn)) {
        String8 msg = ui_image_failed(url) ? str8_lit("Couldn't load this picture.") : str8_lit("Loading...");
        ui_text_centered(ui.font.normal, rect(0, H * 0.5f - 12 * s, W, H * 0.5f + 12 * s), msg, ui_hex(0xffffff, 0.7f));
        if (!ui_image_failed(url)) animating |= ui_loading_bar(rect(W * 0.3f, H * 0.5f + 20 * s, W * 0.7f, H * 0.5f + 22 * s));
    }
    B32 used = 0; // a button took the click
    F32 bs = 36 * s;
    if (ui_glass_button(rect(W - 16 * s - bs, 12 * s, W - 16 * s, 12 * s + bs), str8_lit("✕"), ui.font.normal)) { app.viewer_open = 0; used = 1; }
    if (app.viewer_count > 1) {
        ui_text_centered(ui.font.normal, rect(0, 12 * s, W, 12 * s + bs), str8_pushf(fa, "%u / %u", app.viewer_index + 1, app.viewer_count), ui_hex(0xffffff, 0.75f));
        F32 ab = 44 * s, cy = H * 0.5f;
        if (ui_glass_button(rect(16 * s, cy - ab * 0.5f, 16 * s + ab, cy + ab * 0.5f), str8_lit("‹"), ui.font.title)) { viewer_step(-1); used = 1; }
        if (ui_glass_button(rect(W - 16 * s - ab, cy - ab * 0.5f, W - 16 * s, cy + ab * 0.5f), str8_lit("›"), ui.font.title)) { viewer_step(1); used = 1; }
    }
    // The page with the full picture, for saving it.
    String8 ol = str8_lit("Open in browser");
    F32 ow = f_text_width(ui.font.caption, ol) + 24 * s;
    Rect ob = rect(16 * s, 12 * s + (bs - 26 * s) * 0.5f, 16 * s + ow, 12 * s + (bs + 26 * s) * 0.5f);
    if (ui_glass_button(ob, ol, ui.font.caption)) { os_open_url(url); used = 1; }
    // A click beside the picture closes the viewer.
    if (!used && ui.in.released && point_in(full, ui.in.press_x, ui.in.press_y) && !point_in(drawn, ui.in.press_x, ui.in.press_y)) app.viewer_open = 0;
    ui_overlay_end();
    return animating;
}

static String8 fmt_year(Arena *a, S64 unix_secs)
{
    OS_DateTime d = os_utc_from_unix(unix_secs);
    return d.year ? str8_pushf(a, "%u", d.year) : str8(0, 0);
}

// A small text button for a link; returns the x after it.
static F32 detail_link(F32 x, F32 y, F32 h, String8 label, String8 url)
{
    url = web_link(ui.frame_arena, url);
    if (!url.size) return x;
    F32 w = ui_button_width(label);
    if (ui_button(rect(x, y, x + w, y + h), label, C_TEXT, C_BUTTON, C_BUTTON_HI)) os_open_url(url);
    return x + w + 8 * ui.scale;
}

// The addon's support links, as buttons that open in the browser and say where they go first
// (authors choose the links; one in a description may be someone else's). On CurseForge always its page too: only that has its Donate button.
// Returns the height (0: nothing to show).
static F32 draw_support(Rect p, F32 x, F32 y, F32 w, AddonDetails *d)
{
    F32 s = ui.scale, bh = 30 * s, gap = 8 * s, y0 = y;
    SupportLink v[SUPPORT_MAX + 1];
    U32 n = d->support_count;
    for (U32 i = 0; i < n; i += 1) v[i] = d->support[i];
    B32 cf_page = id_is(d->id, "cf:") && d->website.size;
    if (cf_page) v[n++] = (SupportLink){ str8_lit("CurseForge page"), web_link(ui.frame_arena, d->website) };
    if (!n) return 0;
    f_draw_text(ui.font.bold, x, y, str8_lit("Support links"), ui.theme.heading, w);
    y += ui.font.bold->line_height + 6 * s;
    F32 bx = x;
    for (U32 i = 0; i < n; i += 1) {
        B32 page = cf_page && i == n - 1;
        F32 bw = ui_button_width(v[i].name);
        if (bx > x && bx + bw > x + w) { bx = x; y += bh + gap; }
        Rect r = rect(bx, y, bx + bw, y + bh);
        if (ui_button(r, str8_pushf(ui.frame_arena, "%.*s###support%u", S8F(v[i].name), i), C_TEXT, C_BUTTON, C_BUTTON_HI)) os_open_url(v[i].url);
        if (ui_mouse_in(r) && ui_mouse_in(p)) {
            String8 url = v[i].url;
            ui_tooltip(r, page ? str8_lit("Its page has a Donate button if the author turned it on")
                         : url.size > 90 ? str8_pushf(ui.frame_arena, "%.*s...", 87, url.str) : url);
        }
        bx += bw + gap;
    }
    return y + bh - y0;
}

// The Support button's menu, for an addon with several links.
static void show_support_menu(void)
{
    DetailSnap *ds = details_find(str8_cstring(app.sel_id));
    if (!ds || !ds->d.support_count) return;
    // Copies: the menu runs a message loop, in which newer details may take the snapshot's place.
    Temp scratch = scratch_begin(0);
    String8 items[SUPPORT_MAX], urls[SUPPORT_MAX];
    U32 n = ds->d.support_count;
    for (U32 i = 0; i < n; i += 1) {
        SupportLink *l = &ds->d.support[i];
        // Where it goes, without a query ('&' would mean something in a menu).
        String8 page = str8_skip(l->url, 8);
        if (str8_starts_with(page, str8_lit("www."), 1)) page = str8_skip(page, 4);
        U64 k = 0;
        while (k < page.size && k < 48 && page.str[k] != '?' && page.str[k] != '&' && page.str[k] != '#') k += 1;
        urls[i] = str8_copy(scratch.arena, l->url);
        items[i] = str8_pushf(scratch.arena, "%.*s  (%.*s)", S8F(l->name), S8F(str8_prefix(page, k)));
    }
    S32 pick = wm_popup_menu(app.support_btn, items, n, -1);
    if (pick >= 0 && pick < (S32)n) os_open_url(urls[pick]);
    scratch_end(scratch);
}

// The button near the top: one link opens, several drop down a menu. Only its heart when there's
// no room for more. Returns the x after it. It comes with the details, like the links after it.
static F32 draw_support_button(Rect p, F32 x, F32 y, F32 h, F32 x1, AddonDetails *d)
{
    if (!d->support_count) return x;
    F32 s = ui.scale;
    String8 label = str8_lit("\xE2\x99\xA5 Support");
    F32 w = ui_button_width(label);
    if (x + w > x1) { label = str8_lit("\xE2\x99\xA5###\xE2\x99\xA5 Support"); w = ui_button_width(label); }
    Rect r = rect(x, y, x + w, y + h);
    if (ui_button(r, label, C_TEXT, C_BUTTON, C_BUTTON_HI)) {
        app.support_btn = r;
        if (d->support_count == 1) os_open_url(d->support[0].url);
        else wm_defer(show_support_menu);
    }
    if (ui_mouse_in(r) && ui_mouse_in(p)) {
        String8List names = { 0 };
        for (U32 i = 0; i < d->support_count; i += 1) str8_list_push(ui.frame_arena, &names, d->support[i].name);
        String8 url = d->support[0].url;
        ui_tooltip(r, d->support_count > 1 ? str8_pushf(ui.frame_arena, "Support links on the addon's page: %.*s", S8F(str8_list_join(ui.frame_arena, &names, str8_lit(", "))))
                     : url.size > 90 ? str8_pushf(ui.frame_arena, "%.*s...", 87, url.str) : url);
    }
    return x + w + 8 * s;
}

// The screenshots: one shown whole, and a filmstrip of them all to pick from under it. Returns the height.
static F32 draw_gallery(Rect p, F32 x, F32 y, F32 w, Shot *shots, U32 n, B32 *animating)
{
    F32 s = ui.scale, gap = 6 * s;
    if (app.gallery_index >= n) app.gallery_index = 0;
    Rect stage = rect(x, y, x + w, (F32)(int)(y + Min(w * 9.0f / 16.0f, (p.y1 - p.y0) * 0.6f)));
    B32 in_view = ui_mouse_in(ui.clip);
    if (stage.y1 > p.y0 && stage.y0 < p.y1) {
        Shot *shot = &shots[app.gallery_index];
        F32 cx = (stage.x0 + stage.x1) * 0.5f, cy = (stage.y0 + stage.y1) * 0.5f;
        r_rect(stage, ui_hex(0x000000, 0.30f), 8 * s, 0, 0.5f);
        // The whole picture is asked for when the details are (at once, unless the selection is moving), so holding an arrow
        // key doesn't fetch one per row; until it's there, its thumbnail, and then the next one loads too.
        B32 rests = !details_due_ms || ui_picture_ready(shot->full);
        if (rests && ui_picture(stage, shot->full, 2 * s, 8 * s, !app.viewer_open, 0)) {
            gallery_wait.key = 0;
            String8 next = shots[(app.gallery_index + 1) % n].full;
            if (n > 1 && !ui_image_url_gif(next)) ui_picture_ahead(next); // (GIFs are big)
        } else if (rests && ui_picture_failed(shot->full)) {
            ui_text_centered(ui.font.caption, stage, str8_lit("Couldn't load this picture."), C_FAINT);
        } else {
            ui_image(ui_image_fit(stage, UI_IMG_THUMB_W, UI_IMG_THUMB_H, 1e6f), shot->thumb, UI_IMG_THUMB, 8 * s, ui_hex(0xffffff, 0.6f));
            if (rests && wait_shown(&gallery_wait, ui_image_key(shot->full))) *animating |= ui_spinner(cx, cy, 9 * s);
        }
        B32 hot = in_view && ui_mouse_in(stage), used = 0;
        if (n > 1) {
            // Arrows and "2 / 9", on dark plates: the picture under them may be light.
            F32 b = 32 * s;
            for (int k = 0; k < 2; k += 1) {
                F32 bx = k ? stage.x1 - b - 8 * s : stage.x0 + 8 * s;
                Rect r = rect(bx, cy - b * 0.5f, bx + b, cy + b * 0.5f);
                if (!hot) continue;
                r_rect(r, ui_hex(0x05070a, 0.72f), b * 0.5f, 0, 0.5f);
                if (ui_glass_button(r, k ? str8_lit("\xE2\x80\xBA###gallery_next") : str8_lit("\xE2\x80\xB9###gallery_prev"), ui.font.title)) {
                    app.gallery_index = (app.gallery_index + (k ? 1 : n - 1)) % n;
                    wm_request_frame();
                }
                used |= ui_mouse_in(r) || point_in(r, ui.in.press_x, ui.in.press_y);
            }
            String8 count = str8_pushf(ui.frame_arena, "%u / %u", app.gallery_index + 1, n);
            F32 cw = f_text_width(ui.font.caption, count) + 16 * s, ch = ui.font.caption->line_height + 4 * s;
            Rect cr = rect(stage.x1 - cw - 8 * s, stage.y1 - ch - 8 * s, stage.x1 - 8 * s, stage.y1 - 8 * s);
            r_rect(cr, ui_hex(0x05070a, 0.72f), ch * 0.5f, 0, 0.5f);
            ui_text_centered(ui.font.caption, cr, count, ui_hex(0xffffff, 0.9f));
        }
        if (hot && !used) {
            ui.cursor = UI_CURSOR_HAND;
            if (ui_clicked(stage)) viewer_open(shots, n, app.gallery_index); // at full size, over the window
        }
    }
    if (n < 2) return stage.y1 - y;

    // The filmstrip: as many as fit, moving along to keep the shown one (and the next) in it.
    U32 fit = (U32)Max((w + gap) / (72 * s + gap), 1);
    F32 tw = (w - gap * (F32)(fit - 1)) / (F32)fit, th = (F32)(int)(tw * 9.0f / 16.0f), ty = stage.y1 + gap;
    U32 first = n > fit ? Min(app.gallery_first, n - fit) : 0, margin = fit > 2 ? 1 : 0;
    U32 i = app.gallery_index;
    if (n > fit && !ui.in.down) { // (not under a press)
        if (i < first + margin) first = i > margin ? i - margin : 0;
        if (i + margin >= first + fit) first = Min(i + margin + 1 - fit, n - fit);
    }
    app.gallery_first = first;
    if (ty + th > p.y0 && ty < p.y1) {
        for (U32 k = 0; k < fit && first + k < n; k += 1) {
            U32 at = first + k;
            F32 tx = x + (F32)k * (tw + gap);
            Rect r = rect((F32)(int)tx, ty, (F32)(int)(tx + tw), ty + th);
            B32 hot = in_view && ui_mouse_in(r), cur = at == app.gallery_index;
            if (!ui_image(r, shots[at].thumb, UI_IMG_THUMB, 5 * s, ui_hex(0xffffff, cur || hot ? 1.0f : 0.55f))) r_rect(r, C_BUTTON, 5 * s, 0, 0.5f);
            if (ui_image_url_gif(shots[at].full)) draw_gif_badge(r);
            if (cur) r_rect(r, C_ACCENT, 5 * s, 2 * ui_px(), 0.5f);
            else if (hot) r_rect(r, C_BORDER_HI, 5 * s, ui_px(), 0.5f);
            if (hot) ui.cursor = UI_CURSOR_HAND;
            if (hot && ui_clicked(r)) { app.gallery_index = at; wm_request_frame(); }
        }
    }
    return ty + th - y;
}

static B32 draw_detail(Rect p, SearchResult *e)
{
    F32 s = ui.scale;
    Arena *fa = ui.frame_arena;
    B32 animating = 0;
    ui.clip = p;
    r_set_clip(p);
    r_rect(p, ui_hex(0x000000, 0.14f), 0, 0, 0);
    r_rect(rect(p.x0, p.y0, p.x0 + ui_px(), p.y1), C_BORDER, 0, 0, 0);
    DetailSnap *ds = details_find(e->id);
    AddonDetails *d = ds ? &ds->d : 0;
    B32 loading = !ds;
    if (!loading) detail_wait.key = 0;
    B32 show_loading = loading && !details_due_ms && wait_shown(&detail_wait, ui_image_key(e->id));
    animating |= ui_scroll_list(&app.detail_scroll, p, app.detail_content_h);
    if (show_loading) animating |= ui_loading_bar(rect(p.x0, p.y0, p.x1, p.y0 + 2 * s));

    F32 x = p.x0 + 20 * s, w = p.x1 - 20 * s - x;
    F32 y0 = p.y0 + 18 * s - app.detail_scroll.pos, y = y0;

    // Header: picture, name, source and author.
    Rect ic = rect(x, y, x + 56 * s, y + 56 * s);
    draw_addon_icon(ic, e, 12 * s);
    F32 hx = ic.x1 + 14 * s, hw = p.x1 - 44 * s - hx;
    f_draw_text(ui.font.title, hx, y + 2 * s, e->name, C_TEXT, hw);
    F32 ly = y + 2 * s + ui.font.title->line_height + 6 * s;
    int src = source_of_id(e->id) >= 0 ? source_of_id(e->id) : SRC_CURSEFORGE;
    String8 src_name = str8_cstring(source_names[src]);
    F32 pill_w = f_text_width(ui.font.caption, src_name) + 14 * s, pill_h = ui.font.caption->line_height + 2 * s;
    draw_source_pill(rect(hx, ly, hx + pill_w, ly + pill_h), src, 1, 0);
    F32 ax = hx + pill_w;
    // Also on another source: its pill, outlined (a link to its page).
    SearchResult *twin = cat_twin(&app.cat, e);
    if (twin && source_of_id(twin->id) >= 0) {
        int tsrc = source_of_id(twin->id);
        F32 tw = f_text_width(ui.font.caption, str8_cstring(source_names[tsrc])) + 14 * s;
        Rect tr = rect(ax + 6 * s, ly, ax + 6 * s + tw, ly + pill_h);
        B32 hot = ui_mouse_in(tr) && twin->website.size;
        draw_source_pill(tr, tsrc, 0, hot);
        if (hot && ui_clicked(tr)) os_open_url(twin->website);
        ax = tr.x1;
    }
    if (e->site_only) ax = draw_site_tag(ax + 6 * s, ly + pill_h * 0.5f, e->site_only, hx + hw);
    if (e->author.size) f_draw_text(ui.font.caption, ax + 10 * s, ly + 1 * s, str8_pushf(fa, "by %.*s", S8F(e->author)), C_DIM, hx + hw - ax - 10 * s);
    y = Max(ic.y1, ly + pill_h) + 14 * s;

    // Facts (of both sources, for an addon on two).
    String8List facts = { 0 };
    RowFacts rf = row_facts(e);
    if (rf.downloads > 0) str8_list_push(fa, &facts, str8_pushf(fa, "%.*s download%s", S8F(str8_thousands(fa, rf.downloads)), rf.downloads == 1 ? "" : "s"));
    if (rf.updated > 0) str8_list_push(fa, &facts, str8_pushf(fa, "updated %.*s", S8F(fmt_ago(fa, (U64)rf.updated))));
    if (rf.created > 0) str8_list_push(fa, &facts, str8_pushf(fa, "since %.*s", S8F(fmt_year(fa, rf.created))));
    if (facts.count) { y += ui_text_wrapped(ui.font.caption, x, y, w, str8_list_join(fa, &facts, str8_lit("  ·  ")), C_FAINT, p.y0, p.y1); }
    // What only the details say: one line, with its place kept whether they're here yet or not (and whether
    // they say it or not), so nothing below moves when they come.
    String8List late = { 0 };
    if (d && d->version.size) str8_list_push(fa, &late, str8_pushf(fa, "Latest: %.*s", S8F(d->version)));
    if (d && d->favorites > 0) str8_list_push(fa, &late, str8_pushf(fa, "%.*s favorite%s", S8F(str8_thousands(fa, d->favorites)), d->favorites == 1 ? "" : "s"));
    if (late.count) f_draw_text(ui.font.caption, x, y + 2 * s, str8_list_join(fa, &late, str8_lit("  ·  ")), C_FAINT, w);
    y += ui.font.caption->line_height + 2 * s;
    y += 12 * s;

    // Actions: install (for an addon on two sources, also from the other), then the addon's pages.
    F32 bh = 30 * s, bx = x;
    SearchResult *have = row_installed(e);
    if (have) {
        String8 label = twin ? str8_pushf(fa, "Installed from %s", have->source ? have->source : "elsewhere") : str8_lit("Installed");
        f_draw_text(ui.font.normal, bx, y + (bh - ui.font.normal->line_height) * 0.5f, label, C_GREEN, 0);
        bx += f_text_width(ui.font.normal, label) + 16 * s;
    } else {
        if (!e->available) {
            String8 label = twin ? str8_pushf(fa, "No file on %s for this game version", e->source ? e->source : "this source") : str8_lit("No file for this game version");
            f_draw_text(ui.font.caption, bx, y + (bh - ui.font.caption->line_height) * 0.5f, label, C_FAINT, 0);
            bx += f_text_width(ui.font.caption, label) + 16 * s;
        } else if (feedback_installing(e->id)) {
            F32 pw = 140 * s;
            animating |= draw_install_progress(rect(bx, y, bx + pw, y + bh), feedback_installing(e->id));
            bx += pw + 12 * s;
        } else {
            String8 label = e->site_only == SITE_ONLY_PAGE ? str8_lit("Install \xE2\x86\x97") : str8_lit("Install");
            F32 iw = ui_button_width(label) + 16 * s;
            if (ui_button(rect(bx, y, bx + iw, y + bh), label, C_TEXT, C_ACCENT_BG, C_ACCENT)) browse_install(e);
            bx += iw + 12 * s;
        }
        // The other source (Wago only with a key): "From Wago", "From CurseForge ↗".
        if (twin && twin->available && (source_of_id(twin->id) != SRC_WAGO || app.wago_key[0])) {
            String8 label = str8_pushf(fa, "From %s%s", twin->source ? twin->source : "the other source", twin->site_only == SITE_ONLY_PAGE ? " \xE2\x86\x97" : "");
            F32 iw = ui_button_width(label) + 8 * s;
            if (ui_button(rect(bx, y, bx + iw, y + bh), label, C_TEXT, C_BUTTON, C_BUTTON_HI)) browse_install(twin);
            bx += iw + 12 * s;
        }
    }
    bx = detail_link(bx, y, bh, str8_lit("Page"), d && d->website.size ? d->website : e->website);
    if (d) {
        bx = draw_support_button(p, bx, y, bh, x + w, d);
        bx = detail_link(bx, y, bh, str8_lit("Source"), d->source_url);
        bx = detail_link(bx, y, bh, str8_lit("Issues"), d->issues_url);
    }
    y += bh + 16 * s;
    ui_divider(x, x + w, y);
    y += 16 * s;

    // How a website-only addon gets installed, before anyone clicks.
    if (e->site_only) {
        String8 note = e->site_only == SITE_ONLY_PAGE
            ? str8_lit("Its author lets only CurseForge's own website serve the files. Installing or updating it opens "
                       "its download page in a small wam window: the download starts by itself, the window closes when it's "
                       "done, and wam installs the file.")
            : str8_lit("Its author lets only CurseForge's own website serve the files, so wam installs the release from the "
                       "addon's GitHub page instead. If that release is older than CurseForge's, wam opens CurseForge's "
                       "download page in a small window and installs the file it downloads.");
        F32 pad = 10 * s, nh = ui_text_wrapped(ui.font.caption, x + pad, 0, w - 2 * pad, note, C_DIM, 1e9f, 1e9f); // measure only (nothing visible)
        Rect box = rect(x, y, x + w, y + nh + 2 * pad);
        r_rect(box, e->site_only == SITE_ONLY_PAGE ? ui_hex(0x4a3612, 0.45f) : ui_hex(0x2d333b, 0.6f), 8 * s, 0, 0.5f);
        ui_text_wrapped(ui.font.caption, x + pad, y + pad, w - 2 * pad, note, e->site_only == SITE_ONLY_PAGE ? ui_hex(0xffe3b0, 1) : C_DIM, p.y0, p.y1);
        y = box.y1 + 14 * s;
    }

    // Summary, screenshots, description, changelog.
    if (e->summary.size) y += ui_text_wrapped(ui.font.normal, x, y, w, e->summary, C_DIM, p.y0, p.y1) + 14 * s;
    Shot shots[SHOTS_MAX];
    U32 nshots = parse_shots(d && d->shots.size ? d->shots : e->shots, shots, SHOTS_MAX);
    if (nshots) y += draw_gallery(p, x, y, w, shots, nshots, &animating) + 14 * s;
    if (loading) {
        F32 cr = 7 * s;
        if (show_loading) {
            animating |= ui_spinner(x + cr, y + ui.font.caption->line_height * 0.5f, cr);
            f_draw_text(ui.font.caption, x + cr * 2 + 8 * s, y, str8_lit("Loading details..."), C_FAINT, w - cr * 2 - 8 * s);
        }
        y += ui.font.caption->line_height + 8 * s;
    } else if (!d->ok) {
        y += ui_text_wrapped(ui.font.caption, x, y, w, d->error, C_FAINT, p.y0, p.y1) + 8 * s;
    } else {
        F32 support_h = draw_support(p, x, y, w, d);
        if (support_h > 0) y += support_h + 16 * s;
        String8 picture = { 0 };
        if (!rich_empty(d->description)) {
            f_draw_text(ui.font.bold, x, y, str8_lit("Description"), ui.theme.heading, w);
            y += ui.font.bold->line_height + 6 * s;
            y += rich_draw(d->description, x, y, w, 0, p.y0, p.y1, &picture) + 16 * s;
        }
        if (!rich_empty(d->changelog)) {
            f_draw_text(ui.font.bold, x, y, str8_lit("Changelog"), ui.theme.heading, w);
            y += ui.font.bold->line_height + 6 * s;
            y += rich_draw(d->changelog, x, y, w, 1, p.y0, p.y1, &picture) + 16 * s;
        }
        if (picture.size) { Shot one = { picture, picture }; viewer_open(&one, 1, 0); } // a picture in the text, in wam
    }
    app.detail_content_h = y - y0 + 24 * s;
    ui_scrollbar(&app.detail_scroll, p, app.detail_content_h);

    // Close (clears the selection), fixed in the corner.
    Rect cb = rect(p.x1 - 38 * s, p.y0 + 12 * s, p.x1 - 12 * s, p.y0 + 38 * s);
    B32 chot = ui_mouse_in(cb);
    ui_plate(cb, chot ? C_BUTTON_HI : C_BUTTON, 6 * s);
    ui_text_centered(ui.font.normal, cb, str8_lit("✕"), chot ? C_TEXT : C_DIM);
    if (ui_clicked(cb)) { app.sel_id[0] = 0; app.sel_index = -1; wm_request_frame(); }
    return animating;
}

static B32 draw_search(Rect area)
{
    Arena *fa = ui.frame_arena;
    F32 s = ui.scale;
    F32 row_h = ADDON_ROW_H * s;
    B32 animating = 0;

    // Search field
    ui.clip = area;
    Rect field = rect(area.x0 + 12 * s, area.y0 + 12 * s, area.x1 - 12 * s, area.y0 + 48 * s);
    // A soft bar with a magnifying glass; its rim lights up while there's a query.
    r_rect(field, C_BG, 8 * s, 0, 0.5f);
    f_draw_text(ui.font.glyphs_big, field.x0 + 12 * s, (field.y0 + field.y1 - ui.font.glyphs_big->line_height) * 0.5f, str8_lit("\xEE\x9C\xA1"), C_FAINT, 0); // Search
    V4 no_border = C_BG;
    no_border.w = 0;
    ui_text_field(rect(field.x0 + 26 * s, field.y0, field.x1, field.y1), &app.query,
                  str8_lit("Filter by name, or paste a GitHub / WoWInterface / CurseForge / Wago link and press Enter"), 0, 1, no_border);
    if (app.query.len) r_rect(field, C_ACCENT_BG, 8 * s, ui_px(), 0.5f);

    // Left open for a long time: refresh the catalog once in the background.
    if (app.catalog_saved && cat_now() - app.catalog_saved > CAT_MAX_AGE && app.catalog_served >= app.catalog_asked) {
        app.catalog_saved = cat_now(); // asked; the fetch sets the real time
        catalog_request(1);
    }
    frame_timers();
    browse_order(row_h);
    paging_sync();
    draw_browse_filters(rect(field.x0, field.y1 + 8 * s, area.x1 - 12 * s, field.y1 + 40 * s));

    // The detail panel takes the right side while a row is selected (and the window is wide enough).
    SearchResult *sel_e = app.sel_index >= 0 && app.sel_index < (S64)app.order_count ? &app.cat.v[app.order[app.sel_index]] : 0;
    // Its width: a share of the view, so it keeps its proportion when the window is resized; set by
    // dragging the divider, else 42% (between 340 and 520 px). Kept at least 300 px, leaving the list 440 px.
    F32 aw = area.x1 - area.x0, pw_max = aw - 440 * s;
    F32 pw = app.detail_share > 0 ? Max(300 * s, aw * app.detail_share) : Clamp(340 * s, aw * 0.42f, 520 * s);
    pw = Min(pw, pw_max);
    B32 panel = sel_e && pw >= 300 * s;
    details_follow();
    Rect list = rect(area.x0, field.y1 + 48 * s, panel ? area.x1 - pw : area.x1, area.y1);
    r_rect(rect(area.x0, list.y0 - ui_px(), area.x1, list.y0), C_BORDER, 0, 0, 0);
    U64 count = app.order_count;
    B32 paging = app.suggest_served < app.more_asked;
    if (app.more_waiting && !paging) { // the page has come: did it add anything?
        app.more_waiting = 0;
        app.more_empty = count > app.more_rows ? 0 : app.more_empty + 1;
    }
    B32 more_shown = !app.more_done && !(app.src_hidden & (1u << SRC_CURSEFORGE)) && (!browse_query().size || app.more_index > 0) && app.more_category == browse_cf_category();
    B32 more_link = more_shown && !paging && app.more_empty >= MORE_EMPTY_MAX && app.suggest_served >= app.suggest_asked; // asking on is up to the user

    // Keep the selected row in view.
    if (app.sel_reveal && app.sel_index >= 0) {
        ui_scroll_reveal(&app.scroll[VIEW_SEARCH], (F32)app.sel_index * row_h, (F32)(app.sel_index + 1) * row_h, list.y1 - list.y0);
        app.sel_reveal = 0;
    }
    UI_List l = ui_list_begin(&app.scroll[VIEW_SEARCH], list, (S64)count, row_h, (paging || more_link) && count ? row_h : 0); // + a "Loading more..." row
    animating |= l.animating;

    String8 q = browse_query();
    F32 col_right = list.x1 - 16 * s - 130 * s;
    app.hover_index = -1;
    for (S64 i = l.first; i < l.last; i += 1) {
        SearchResult *r = &app.cat.v[app.order[i]];
        Rect rr = ui_list_row(&l, i, r->id);
        F32 ry = rr.y0;
        B32 hot = ui_mouse_in(rr) && ui_mouse_in(list), sel = i == app.sel_index;
        if (hot) app.hover_index = i;
        if (sel) draw_row_fill(rr, C_SELECTED);
        else if (hot) draw_row_fill(rr, C_HOVER);

        // Picture, name, the summary; author, downloads, last update and its sources (links to
        // their pages: where it installs from first, then the other one for a two-source addon).
        SearchResult *twin = cat_twin(&app.cat, r);
        RowFacts rf = row_facts(r);
        RowLook look = { 0 };
        look.rr = rr; look.list = list; look.text_right = col_right;
        look.icon_url = r->icon; look.icon_src = source_of_id(r->id) >= 0 ? source_of_id(r->id) : SRC_CURSEFORGE;
        look.title = r->name; look.title_color = rf.available ? C_TEXT : C_DIM;
        FuzzyMatch fm = q.size ? fuzzy_match(q, r->name) : (FuzzyMatch){ -1, 0, { 0 } };
        if (fm.score >= 0) { look.marks = fm.pos; look.mark_count = fm.count; }
        look.pill_count = 1; look.pill_src[0] = look.icon_src; look.pill_url[0] = r->website; look.pill_filled[0] = 1;
        if (twin && source_of_id(twin->id) >= 0) { look.pill_src[1] = source_of_id(twin->id); look.pill_url[1] = twin->website; look.pill_count = 2; }
        look.site_only = r->site_only;
        String8List meta = { 0 };
        if (r->author.size) str8_list_push(fa, &meta, r->author);
        if (rf.downloads > 0) str8_list_push(fa, &meta, str8_pushf(fa, "%.*s download%s", S8F(str8_count_short(fa, rf.downloads)), rf.downloads == 1 ? "" : "s")); // Tukui reports no counts
        if (rf.updated > 0) str8_list_push(fa, &meta, str8_pushf(fa, "updated %.*s", S8F(fmt_ago(fa, (U64)rf.updated))));
        look.meta = str8_list_join(fa, &meta, str8_lit("  \xC2\xB7  "));
        look.line2 = r->summary; look.line2_color = C_DIM;
        B32 pressed_pill = draw_row_look(&look);

        F32 bh = 28 * s, by = ry + (row_h - bh) * 0.5f, x = rr.x1 - 16 * s;
        Rect action = rect(x, by, x, by + bh);
        if (row_installed(r)) {
            String8 label = str8_lit("\xE2\x9C\x93 Installed");
            f_draw_text(ui.font.caption, x - f_text_width(ui.font.caption, label), ry + (row_h - ui.font.caption->line_height) * 0.5f, label, C_GREEN, 0);
        } else if (feedback_installing(r->id)) {
            // The Install button becomes the install's progress bar.
            action = rect(x - 118 * s, by, x, by + bh);
            animating |= draw_install_progress(action, feedback_installing(r->id));
        } else if (!r->available) {
            String8 label = str8_lit("Not for this version");
            f_draw_text(ui.font.caption, x - f_text_width(ui.font.caption, label), ry + (row_h - ui.font.caption->line_height) * 0.5f, label, C_FAINT, 0);
        } else {
            // Enter installs the selected row; the arrow: the file comes through CurseForge's website.
            B32 via_page = r->site_only == SITE_ONLY_PAGE;
            String8 label = sel ? (via_page ? str8_lit("Install \xE2\x86\x97  \xE2\x86\xB5###install") : str8_lit("Install  \xE2\x86\xB5###install"))
                                : (via_page ? str8_lit("Install \xE2\x86\x97###install") : str8_lit("Install###install"));
            action = rect(x - ui_button_width(label), by, x, by + bh);
            if (ui_button(action, label, C_TEXT, C_ACCENT_BG, C_ACCENT)) browse_install(r);
        }
        // A click elsewhere on the row selects it.
        if (hot && ui.in.released && point_in(rr, ui.in.press_x, ui.in.press_y) && !pressed_pill
            && !point_in(action, ui.in.press_x, ui.in.press_y)) {
            app.sel_index = i;
            copy_cstr(app.sel_id, sizeof(app.sel_id), r->id);
        }
    }
    if (paging && count) {
        F32 ry = ui_list_row(&l, (S64)count, str8_lit("loading more")).y0;
        ui_text_centered(ui.font.caption, rect(list.x0, ry, list.x1, ry + row_h), str8_lit("Loading more..."), C_FAINT);
    }
    // Near the end: the next page of this sort (CurseForge).
    if (more_link) {
        F32 ry = count ? ui_list_row(&l, (S64)count, str8_lit("look for more")).y0 : list.y0 + 124 * s; // (under the empty list's texts)
        String8 label = str8_lit("Look for more on CurseForge");
        F32 tw = f_text_width(ui.font.caption, label);
        Rect hit;
        ui_link_text(ui.font.caption, (list.x0 + list.x1 - tw) * 0.5f, ry + (row_h - ui.font.caption->line_height) * 0.5f, label, C_ACCENT_HI, 0, &hit);
        if (ui_mouse_in(ui.clip) && ui_clicked(hit)) { app.more_empty = 0; wm_request_frame(); }
    }
    if (more_shown && !paging && !app.more_waiting && app.more_empty < MORE_EMPTY_MAX && app.suggest_served >= app.suggest_asked && l.last + 10 >= (S64)count
        && app.catalog_served >= app.catalog_asked) {
        request_page(app.more_sort, q, app.more_index);
    }

    if (!count) {
        F32 cy = list.y0 + 36 * s;
        B32 loading = app.catalog_served < app.catalog_asked || app.suggest_served < app.suggest_asked;
        if (loading && !app.cat.count) {
            ui_text_centered(ui.font.normal, rect(list.x0, cy, list.x1, cy + 24 * s), str8_lit("Loading the addon list..."), C_DIM);
            animating |= ui_loading_bar(rect(list.x0, list.y0, list.x1, list.y0 + 2 * s));
        } else if (!app.cf_key[0] && !app.cat.count) {
            ui_text_centered(ui.font.normal, rect(list.x0, cy, list.x1, cy + 24 * s),
                               str8_lit("CurseForge search needs your own API key. Add it in Settings."), C_DIM);
            ui_text_centered(ui.font.caption, rect(list.x0, cy + 28 * s, list.x1, cy + 48 * s),
                               str8_lit("Paste a link instead, e.g.  BigWigsMods/BigWigs   or   https://www.wowinterface.com/downloads/info5108-Clique.html"), C_FAINT);
        } else if (app.catalog_failed && !app.cat.count) {
            ui_text_centered(ui.font.normal, rect(list.x0, cy, list.x1, cy + 24 * s), str8_lit("Couldn't load the addon list. Are you online?"), C_DIM);
            Rect b = rect((list.x0 + list.x1) * 0.5f - 50 * s, cy + 34 * s, (list.x0 + list.x1) * 0.5f + 50 * s, cy + 62 * s);
            if (ui_button(b, str8_lit("Try again"), C_TEXT, C_BUTTON, C_BUTTON_HI)) catalog_request(1);
        } else {
            String8 msg = q.size && loading ? str8_pushf(fa, "Searching for \"%.*s\"...", S8F(q))
                        : q.size ? str8_pushf(fa, "No addons match \"%.*s\".", S8F(q))
                        : str8_lit("Nothing to show with these filters.");
            ui_text_centered(ui.font.normal, rect(list.x0, cy, list.x1, cy + 24 * s), msg, C_DIM);
            if (q.size && !loading && (app.src_hidden || app.hide_installed))
                ui_text_centered(ui.font.caption, rect(list.x0, cy + 28 * s, list.x1, cy + 48 * s), str8_lit("Some sources or installed addons are filtered out."), C_FAINT);
        }
        ui_text_centered(ui.font.caption, rect(list.x0, cy + 72 * s, list.x1, cy + 92 * s),
                           str8_lit("Downloaded a .zip, .rar or .7z yourself? Drop it anywhere on this window to install it."), C_FAINT);
    } else if (app.suggest_served < app.suggest_asked && !paging) {
        animating |= ui_loading_bar(rect(list.x0, list.y0, list.x1, list.y0 + 2 * s)); // a search is on its way
    }
    ui_list_end(&l);
    if (panel) {
        animating |= draw_detail(rect(list.x1, list.y0, area.x1, area.y1), sel_e);
        // The divider: drag it to resize the panel.
        ui.clip = area;
        r_set_clip(area);
        Rect grip = rect(list.x1 - 1 * s, list.y0, list.x1 + 6 * s, list.y1); // on the panel side: the list's scrollbar is just left of the line
        UI_Signal g = ui_drag_handle(ui_id(str8_lit("detail divider")), grip);
        if (g.dragging) {
            F32 w = Clamp(300 * s, area.x1 - ui.in.mx, pw_max);
            app.detail_share = w / aw;
            animating = 1;
        }
        if (g.dragging || g.hovering) r_rect(rect(list.x1 - 1 * s, list.y0, list.x1 + 1 * s, list.y1), C_ACCENT, 0, 0, 0);
        if (g.released) wam_save_detail_share(app.detail_share); // remembered for next time
    }
    return animating;
}

//- Downloads view

// Watches the download folder(s) so a finished download shows up (and the "new" badge lights up)
// within a second, even while wam is in the background. Windows signals every change; a download
// in progress changes its file constantly, so it waits until the folder has been quiet for a
// moment (or at most a few seconds, so one huge unrelated download can't hold things up).
static U32 downloads_watch_main(void *p)
{
    (void)p;
    Arena *a = arena_alloc(MB(64));
    String8List dirs = downloads_dirs(a);
    OS_DirWatch w = os_dir_watch_open(dirs);
    if (!w.count) return 0;
    for (;;) {
        if (!os_dir_watch_wait(&w, 0xFFFFFFFF)) return 0;
        U64 start = os_now_ms();
        while (os_dir_watch_wait(&w, 400) && os_now_ms() - start <= 3000) {} // settle: 400 ms without changes, or 3 s at most
        wm_post(on_downloads_changed, 0, 0);
    }
}

static void request_downloads_scan(void)
{
    if (!app.wow_root.size) return;
    if (app.dl_scan_queued) { app.dl_rescan_wanted = 1; return; } // a file may have landed after that scan looked
    app.dl_scan_queued = 1;
    app.dl_last_scan_ms = os_now_ms();
    push_task(TASK_SCAN_DOWNLOADS, str8(0, 0));
}

// How long ago unix time mtime was ("5 min ago").
static String8 fmt_ago(Arena *a, U64 mtime)
{
    U64 now = (U64)cat_now();
    return str8_ago(a, now > mtime ? now - mtime : 0);
}

static B32 draw_downloads(Rect area)
{
    Arena *fa = ui.frame_arena;
    F32 s = ui.scale;
    DownloadsSnap *ds = app.downloads;
    U64 count = ds ? ds->list.count : 0;

    F32 head_h = 34 * s;
    String8 where = ds && ds->list.dirs.count ? str8_pushf(fa, "Addon archives in %.*s", S8F(str8_list_join(fa, &ds->list.dirs, str8_lit(", "))))
                  : str8_lit("Looking for your Downloads folder...");
    f_draw_text(ui.font.caption, area.x0 + 20 * s, area.y0 + (head_h - ui.font.caption->line_height) * 0.5f, where, C_FAINT, area.x1 - area.x0 - 40 * s);
    Rect list = rect(area.x0, area.y0 + head_h, area.x1, area.y1);
    r_rect(rect(list.x0, list.y0 - ui_px(), list.x1, list.y0), C_LINE, 0, 0, 0);

    F32 row_h = 56 * s;
    UI_List l = ui_list_begin(&app.scroll[VIEW_DOWNLOADS], list, (S64)count, row_h, 0);
    B32 animating = l.animating;
    F32 col_actions = list.x1 - 16 * s - 230 * s;
    F32 col_iface = col_actions - 70 * s;
    F32 col_version = col_iface - 190 * s;

    for (S64 i = l.first; i < l.last; i += 1) {
        DownloadItem *it = &ds->list.v[i];
        Rect rr = ui_list_row(&l, i, it->path);
        F32 ry = rr.y0;
        if (ui_mouse_in(rr) && ui_mouse_in(list)) r_rect(rr, C_HOVER, 0, 0, 0);
        if (i > 0) r_rect(rect(rr.x0 + 16 * s, ry, rr.x1 - 16 * s, ry + ui_px()), C_LINE, 0, 0, 0);

        F32 tx = rr.x0 + 20 * s;
        F32 top = ry + (row_h - ui.font.bold->line_height - ui.font.caption->line_height - 4 * s) * 0.5f;
        B32 is_new = it->is_new && it->state != DL_INSTALLED;
        F32 title_max = col_version - tx - 16 * s - (is_new ? 52 * s : 0);
        F32 title_end = f_draw_text(ui.font.bold, tx, top, it->title, it->state == DL_INSTALLED ? C_DIM : C_TEXT, title_max);
        if (is_new) {
            // Never installed anywhere yet: a green edge and a NEW tag until it is.
            r_rect(rect(rr.x0 + 4 * s, ry + 10 * s, rr.x0 + 7 * s, ry + row_h - 10 * s), C_GREEN, 1.5f * s, 0, 0.5f);
            String8 tag = str8_lit("NEW");
            F32 tw = f_text_width(ui.font.caption, tag) + 12 * s, th2 = ui.font.caption->line_height + 2 * s;
            F32 tyy = top + (ui.font.bold->line_height - th2) * 0.5f;
            ui_pill(rect(title_end + 10 * s, tyy, title_end + 10 * s + tw, tyy + th2), tag, ui.font.caption, ui_hex(0x0e1014, 1), C_GREEN);
        }
        String8 what = it->is_pack ? str8_list_join(fa, &it->titles, str8_lit(", ")) : str8_list_join(fa, &it->folders, str8_lit(", "));
        String8 sub = str8_pushf(fa, "%.*s  \xC2\xB7  %.*s  \xC2\xB7  %.*s", S8F(it->file_name), S8F(fmt_ago(fa, it->mtime)), S8F(what));
        f_draw_text(ui.font.caption, tx, top + ui.font.bold->line_height + 4 * s, sub, C_FAINT, col_version - tx - 16 * s);

        F32 mid = ry + (row_h - ui.font.normal->line_height) * 0.5f;
        if (it->state == DL_DIFFERENT && it->installed_version.size) {
            f_draw_text(ui.font.normal, col_version, top, it->version.size ? it->version : str8_lit("-"), C_TEXT, col_iface - col_version - 12 * s);
            f_draw_text(ui.font.caption, col_version, top + ui.font.bold->line_height + 4 * s,
                        str8_pushf(fa, "installed: %.*s", S8F(it->installed_version)), C_FAINT, col_iface - col_version - 12 * s);
        } else {
            f_draw_text(ui.font.normal, col_version, mid, it->version.size ? it->version : str8_lit("-"), C_DIM, col_iface - col_version - 12 * s);
        }
        if (it->interface_version.size) {
            // Built for another game version (e.g. a Retail zip while Forever is selected)?
            B32 matches = !ds->client.valid;
            for (String8 r = it->interface_version; r.size && !matches;) {
                U64 comma = str8_find(r, 0, str8_lit(","), 0);
                U32 v = (U32)str8_to_s64(str8_trim(str8_prefix(r, comma)));
                r = str8_skip(r, comma + 1);
                matches = v / 10000 == ds->client.major;
            }
            String8 iface = fmt_interface(fa, best_interface(it->interface_version, ds->client));
            if (matches) f_draw_text(ui.font.normal, col_iface, mid, iface, C_FAINT, 0);
            else {
                f_draw_text(ui.font.normal, col_iface, top, iface, C_ERROR, 0);
                f_draw_text(ui.font.caption, col_iface, top + ui.font.bold->line_height + 4 * s, str8_lit("other game version"), C_ERROR, 0);
            }
        }

        // Actions: Show (in Explorer), then Install / Reinstall, or "Installed".
        F32 bh = 28 * s, by = ry + (row_h - bh) * 0.5f, x = rr.x1 - 16 * s;
        String8 show = str8_lit("Show");
        Rect sb = rect(x - ui_button_width(show), by, x, by + bh);
        if (ui_button(sb, show, C_DIM, C_BUTTON, C_BUTTON_HI)) {
            os_shell_open_async(str8_lit("explorer.exe"), str8_pushf(fa, "/select,\"%.*s\"", S8F(it->path)));
        }
        x = sb.x0 - 8 * s;
        if (it->state == DL_INSTALLED) {
            String8 label = str8_lit("Installed");
            f_draw_text(ui.font.normal, x - f_text_width(ui.font.normal, label) - 8 * s, mid, label, C_GREEN, 0);
        } else {
            String8 label = it->state == DL_NEW ? str8_lit("Install")
                          : it->is_pack ? str8_pushf(fa, "Install (%u/%llu have)", it->installed_count, it->folders.count)
                          : str8_lit("Install this version");
            Rect ib = rect(x - ui_button_width(label), by, x, by + bh);
            if (ui_button(ib, label, C_TEXT, C_ACCENT_BG, C_ACCENT)) feedback_install(push_task(TASK_INSTALL_FILE, it->path), str8(0, 0), it->file_name, str8(0, 0));
        }
    }
    if (!count) {
        F32 cy = list.y0 + 36 * s;
        ui_text_centered(ui.font.normal, rect(list.x0, cy, list.x1, cy + 24 * s),
                           ds ? str8_lit("No addon archives in your Downloads folder.") : str8_lit("Scanning..."), C_DIM);
        ui_text_centered(ui.font.caption, rect(list.x0, cy + 28 * s, list.x1, cy + 48 * s),
                           str8_lit("Download an addon (.zip, .rar or .7z) in your browser and it shows up here."), C_FAINT);
    }
    ui_list_end(&l);
    return animating;
}

