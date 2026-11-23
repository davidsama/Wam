// ui_image.c - pictures from URLs (icons, screenshot thumbnails), loaded in the background.
//
// ui_image(dst, url, kind, ...) draws the picture if it's loaded and otherwise asks for it, so a
// view just calls it every frame for what's on screen. ui_image_full(area, url, ...) does the same
// for one picture at full size (a screenshot being viewed), fitted inside area, and ui_picture for whole
// pictures shown smaller (a gallery's, those in a text). Loader threads fetch the bytes through the
// app's ui_image_fetch callback (which can cache them on disk), decode them (os_image_decode), scale them to
// the kind's slot size (icons: square, cropped to fill; thumbnails: 16:9, cropped to fill) and post
// the pixels to the UI thread, which copies them into the renderer's image atlas.
//
// A url "res:NAME" is the program's own RCDATA resource NAME (e.g. a logo built into the .exe),
// read directly instead of through ui_image_fetch.
//
// The atlas holds a fixed number of slots per kind; the least recently drawn picture gives up its
// slot. Requests are served newest first (the picture being looked at before any other, with a loader of its own), and a request whose picture went off screen meanwhile is
// skipped, so scrolling through a long list loads what's visible now, not what was visible before.
// Pictures that fail to load are remembered and not asked for again.
//
// Needs: render (r_image, r_images_update, R_IMAGES_SIZE), wm_post, and os_image_decode.

typedef enum UI_ImageKind { UI_IMG_ICON, UI_IMG_THUMB, UI_IMG_FULL, UI_IMG_FIT, UI_IMG_KIND_COUNT } UI_ImageKind;

// Returns the bytes at url (in a), or empty. Called on loader threads.
typedef String8 UI_ImageFetch(Arena *a, String8 url);
static UI_ImageFetch *ui_image_fetch;
// Testing: when set, every picture that fails to load is appended to this file with the reason.
static String8 ui_image_log;

#define UI_IMG_ICON_PX   64             // icon slots: 64x64, the top half of the atlas
#define UI_IMG_THUMB_W   320            // thumbnail slots: 320x180, the bottom half
#define UI_IMG_THUMB_H   180
#define UI_IMG_FULL_MAX  2560           // the full-size picture: at most this wide or tall (its own texture)
#define UI_IMG_FIT_MAX   1280           // ui_picture's: at most this wide or tall (a texture each)
#define UI_IMG_FITS      48             // how many are kept, and at most UI_IMG_FIT_BYTES of them
#define UI_IMG_FIT_BYTES MB(128)
#define UI_IMG_SIZES     1024           // pictures whose size is remembered, loaded or not
#define UI_IMG_THREADS   4
#define UI_IMG_QUEUE     256
#define UI_IMG_FAILED    1024
#define UI_IMG_MAX_BYTES (12 << 20)

typedef struct UI_ImageSlot {
    U64 key;                // hash of the URL, 0 = free
    U8 state;               // 1 loading, 2 ready
    volatile U64 used_ms;   // last drawn (loader threads read it to skip stale requests)
    U16 x, y;               // in the atlas
    R_Picture picture;      // full size and fitted ones: the texture
    B32 animated;           // a GIF that moves (ui_gif_play)
} UI_ImageSlot;

typedef struct UI_ImageRequest { U64 key; int kind; U32 slot; B32 urgent; char url[1024]; } UI_ImageRequest; // urgent: what's being looked at
typedef struct UI_ImageResult { U64 key; int kind; U32 slot; U32 w, h; B32 ok, skipped, animated; U8 pixels[1]; } UI_ImageResult;

#define UI_IMG_ICON_SLOTS  ((R_IMAGES_SIZE / UI_IMG_ICON_PX) * ((R_IMAGES_SIZE / 2) / UI_IMG_ICON_PX))
#define UI_IMG_THUMB_SLOTS ((R_IMAGES_SIZE / UI_IMG_THUMB_W) * ((R_IMAGES_SIZE / 2) / UI_IMG_THUMB_H))

static struct {
    B32 started;
    UI_ImageSlot icons[UI_IMG_ICON_SLOTS];
    UI_ImageSlot thumbs[UI_IMG_THUMB_SLOTS];
    UI_ImageSlot full, fits[UI_IMG_FITS];
    struct { U64 key; U16 w, h; } sizes[UI_IMG_SIZES]; // of fitted pictures
    U32 sizes_next;
    U32 changes;            // goes up when a fitted picture's size becomes known, or it fails
    U64 failed[UI_IMG_FAILED];
    U32 failed_next;
    OS_Mutex lock;          // the queue
    OS_Semaphore sem, urgent_sem; // any request / an urgent one (for the loader that only takes those)
    UI_ImageRequest queue[UI_IMG_QUEUE];
    U32 queue_count;
} ui_img;

static U64 ui_image_key(String8 url)
{
    U64 h = 14695981039346656037ull;
    for (U64 i = 0; i < url.size; i += 1) { h ^= url.str[i]; h *= 1099511628211ull; }
    return h ? h : 1;
}

// Failures are kept apart for the small kinds and the big ones (which take bigger files).
static B32 ui_image_big(int kind) { return kind == UI_IMG_FULL || kind == UI_IMG_FIT; }
static U64 ui_image_fail_key(U64 key, int kind) { return ui_image_big(kind) ? ~key : key; }

static B32 ui_image_has_failed(U64 key, int kind)
{
    key = ui_image_fail_key(key, kind);
    for (U32 i = 0; i < UI_IMG_FAILED; i += 1) if (ui_img.failed[i] == key) return 1;
    return 0;
}

static UI_ImageSlot *ui_image_slots(int kind, U32 *count)
{
    if (kind == UI_IMG_ICON) { *count = UI_IMG_ICON_SLOTS; return ui_img.icons; }
    if (kind == UI_IMG_FULL) { *count = 1; return &ui_img.full; }
    if (kind == UI_IMG_FIT) { *count = UI_IMG_FITS; return ui_img.fits; }
    *count = UI_IMG_THUMB_SLOTS; return ui_img.thumbs;
}

//- Loader threads: fetch, decode, scale

// Decodes data into a new result (os_alloc) of BGRA pixels: scaled to exactly w x h, cropped to
// fill, or with fit, scaled down (never up) to fit inside w x h. Returns 0 if it can't.
static UI_ImageResult *ui_image_decode(Arena *a, String8 data, U32 w, U32 h, B32 fit)
{
    OS_Image img = os_image_decode(a, data, w, h, fit);
    if (!img.w) return 0;
    UI_ImageResult *res = (UI_ImageResult *)os_alloc(sizeof(UI_ImageResult) + (U64)img.w * img.h * 4);
    if (!res) return 0;
    res->w = img.w; res->h = img.h; res->animated = img.animated;
    memcpy(res->pixels, img.pixels, (U64)img.w * img.h * 4);
    return res;
}

static void ui_image_log_failure(String8 url, char *why, U64 bytes)
{
    if (!ui_image_log.size) return;
    Temp t = scratch_begin(0);
    os_append_file(ui_image_log, str8_pushf(t.arena, "%s (%llu bytes) %.*s\r\n", why, bytes, S8F(url)));
    scratch_end(t);
}

static void ui_image_arrived(U64 param, void *ptr);

static U32 ui_image_thread(void *p)
{
    B32 urgent_only = p != 0;
    Arena *a = arena_alloc(GB(1));
    for (;;) {
        os_semaphore_wait(urgent_only ? ui_img.urgent_sem : ui_img.sem);
        os_mutex_lock(&ui_img.lock);
        // Newest first, urgent ones before the others.
        S32 pick = -1;
        for (S32 i = (S32)ui_img.queue_count - 1; i >= 0 && pick < 0; i -= 1) if (ui_img.queue[i].urgent) pick = i;
        if (pick < 0 && !urgent_only) pick = (S32)ui_img.queue_count - 1;
        B32 have = pick >= 0;
        UI_ImageRequest req = { 0 };
        if (have) {
            req = ui_img.queue[pick];
            ui_img.queue_count -= 1;
            memmove(&ui_img.queue[pick], &ui_img.queue[pick + 1], sizeof(UI_ImageRequest) * (ui_img.queue_count - (U32)pick));
        }
        os_mutex_unlock(&ui_img.lock);
        if (!have) continue;

        U32 w = req.kind == UI_IMG_ICON ? UI_IMG_ICON_PX : req.kind == UI_IMG_FULL ? UI_IMG_FULL_MAX : req.kind == UI_IMG_FIT ? UI_IMG_FIT_MAX : UI_IMG_THUMB_W;
        U32 h = req.kind == UI_IMG_ICON ? UI_IMG_ICON_PX : req.kind == UI_IMG_FULL ? UI_IMG_FULL_MAX : req.kind == UI_IMG_FIT ? UI_IMG_FIT_MAX : UI_IMG_THUMB_H;
        U32 count;
        UI_ImageSlot *slot = &ui_image_slots(req.kind, &count)[req.slot];
        UI_ImageResult *res = 0;
        if (os_now_ms() - slot->used_ms <= 1500 && ui_image_fetch) { // else: off screen while it waited
            arena_clear(a);
            String8 url = str8_cstring(req.url);
            String8 data = str8_starts_with(url, str8_lit("res:"), 0) ? os_resource(str8_skip(url, 4)) : ui_image_fetch(a, url);
            if (data.size && data.size <= UI_IMG_MAX_BYTES * (ui_image_big(req.kind) ? 4 : 1)) res = ui_image_decode(a, data, w, h, ui_image_big(req.kind));
            if (res) res->ok = 1;
            else ui_image_log_failure(url, !data.size ? "fetch failed" : data.size > UI_IMG_MAX_BYTES * (ui_image_big(req.kind) ? 4 : 1) ? "too big" : "decode failed", data.size);
        } else {
            res = (UI_ImageResult *)os_alloc(sizeof(UI_ImageResult));
            if (res) res->skipped = 1;
        }
        if (!res) res = (UI_ImageResult *)os_alloc(sizeof(UI_ImageResult)); // failed
        if (!res) continue;
        res->key = req.key; res->kind = req.kind; res->slot = req.slot;
        if (!wm_post(ui_image_arrived, 0, res)) os_free(res);
    }
}

//- UI thread

static void ui_image_start(void)
{
    if (ui_img.started) return;
    ui_img.started = 1;
    ui_img.sem = os_semaphore_alloc(1 << 20);
    ui_img.urgent_sem = os_semaphore_alloc(1 << 20);
    for (U32 i = 0; i < UI_IMG_ICON_SLOTS; i += 1) {
        U32 per_row = R_IMAGES_SIZE / UI_IMG_ICON_PX;
        ui_img.icons[i].x = (U16)((i % per_row) * UI_IMG_ICON_PX);
        ui_img.icons[i].y = (U16)((i / per_row) * UI_IMG_ICON_PX);
    }
    for (U32 i = 0; i < UI_IMG_THUMB_SLOTS; i += 1) {
        U32 per_row = R_IMAGES_SIZE / UI_IMG_THUMB_W;
        ui_img.thumbs[i].x = (U16)((i % per_row) * UI_IMG_THUMB_W);
        ui_img.thumbs[i].y = (U16)(R_IMAGES_SIZE / 2 + (i / per_row) * UI_IMG_THUMB_H);
    }
    for (U32 t = 0; t < UI_IMG_THREADS; t += 1) os_thread_start(ui_image_thread, 0);
    os_thread_start(ui_image_thread, &ui_img.urgent_sem);
}

static B32 ui_image_url_ok(String8 url)
{
    return str8_starts_with(url, str8_lit("https://"), 1) && url.size < sizeof(((UI_ImageRequest *)0)->url);
}

// w x h as big as fits inside area, at most max_scale times its size, centered, on whole pixels.
static Rect ui_image_fit(Rect area, U32 pw, U32 ph, F32 max_scale)
{
    F32 aw = area.x1 - area.x0, ah = area.y1 - area.y0;
    F32 k = Min(max_scale, Min(aw / (F32)pw, ah / (F32)ph));
    F32 w = (F32)pw * k, h = (F32)ph * k;
    F32 x = (F32)(int)(area.x0 + (aw - w) * 0.5f), y = (F32)(int)(area.y0 + (ah - h) * 0.5f);
    return rect(x, y, x + w, y + h);
}

static void ui_image_size_set(U64 key, U32 w, U32 h)
{
    for (U32 i = 0; i < UI_IMG_SIZES; i += 1) if (ui_img.sizes[i].key == key) return;
    U32 i = ui_img.sizes_next++ % UI_IMG_SIZES;
    ui_img.sizes[i].key = key; ui_img.sizes[i].w = (U16)w; ui_img.sizes[i].h = (U16)h;
    ui_img.changes += 1;
}

// Keeps the fitted pictures within UI_IMG_FIT_BYTES: the least recently drawn ones go, but none on screen.
static void ui_image_fits_trim(void)
{
    U64 now = os_now_ms();
    for (;;) {
        U64 bytes = 0;
        UI_ImageSlot *oldest = 0;
        for (U32 i = 0; i < UI_IMG_FITS; i += 1) {
            UI_ImageSlot *sl = &ui_img.fits[i];
            if (sl->state != 2) continue;
            bytes += (U64)sl->picture.w * sl->picture.h * 4 * 4 / 3; // with its mipmaps
            if (now - sl->used_ms > 2000 && (!oldest || sl->used_ms < oldest->used_ms)) oldest = sl;
        }
        if (bytes <= UI_IMG_FIT_BYTES || !oldest) return;
        r_picture_free(&oldest->picture);
        oldest->key = 0; oldest->state = 0;
    }
}

// A loader thread's result, on the UI thread (wm_post).
static void ui_image_arrived(U64 param, void *ptr)
{
    (void)param;
    UI_ImageResult *res = (UI_ImageResult *)ptr;
    U32 count;
    UI_ImageSlot *slots = ui_image_slots(res->kind, &count);
    UI_ImageSlot *slot = res->slot < count ? &slots[res->slot] : 0;
    if (slot && slot->key == res->key && slot->state == 1) {
        if (res->ok && ui_image_big(res->kind)) {
            r_picture_free(&slot->picture);
            slot->picture = r_picture_make(res->w, res->h, res->pixels);
            slot->state = slot->picture.srv ? 2 : 0;
            slot->animated = res->animated;
            if (!slot->state) { slot->key = 0; ui_img.failed[ui_img.failed_next++ % UI_IMG_FAILED] = ui_image_fail_key(res->key, res->kind); }
            if (res->kind == UI_IMG_FIT) {
                ui_image_size_set(res->key, res->w, res->h);
                ui_image_fits_trim();
            }
        } else if (res->ok) {
            r_images_update(slot->x, slot->y, res->w, res->h, res->pixels, res->w * 4);
            slot->state = 2;
        } else {
            if (!res->skipped) { ui_img.failed[ui_img.failed_next++ % UI_IMG_FAILED] = ui_image_fail_key(res->key, res->kind); ui_img.changes += 1; }
            slot->key = 0; slot->state = 0;
        }
        wm_request_frame();
    }
    os_free(res);
}

// Draws url's picture into dst if it's loaded (returns 1), else asks for it and returns 0 (the
// caller draws a placeholder). radius rounds the corners; tint multiplies (white = as is).
static void ui_image_request(UI_ImageSlot *slot, U32 index, U64 key, int kind, String8 url, B32 urgent);

static B32 ui_image(Rect dst, String8 url, UI_ImageKind kind, F32 radius, V4 tint)
{
    if (!url.size || !r_state->images) return 0;
    ui_image_start();
    U64 key = ui_image_key(url), now = os_now_ms();
    if (ui_image_has_failed(key, kind)) return 0;
    if (!(str8_starts_with(url, str8_lit("https://"), 1) || str8_starts_with(url, str8_lit("res:"), 0)) || url.size >= sizeof(((UI_ImageRequest *)0)->url)) {
        ui_img.failed[ui_img.failed_next++ % UI_IMG_FAILED] = key; // never loadable: don't look again
        ui_image_log_failure(url, "refused (not https:// or too long)", 0);
        return 0;
    }
    U32 count;
    UI_ImageSlot *slots = ui_image_slots(kind, &count);
    UI_ImageSlot *slot = 0, *victim = 0;
    for (U32 i = 0; i < count; i += 1) {
        UI_ImageSlot *sl = &slots[i];
        if (sl->key == key) { slot = sl; break; }
        // A free slot, else the least recently drawn picture that isn't on screen.
        if (!sl->key) { if (!victim || victim->key) victim = sl; }
        else if (sl->state == 2 && now - sl->used_ms > 250 && (!victim || (victim->key && sl->used_ms < victim->used_ms))) victim = sl;
    }
    if (slot) {
        slot->used_ms = now;
        if (slot->state != 2) return 0;
        U32 w = kind == UI_IMG_ICON ? UI_IMG_ICON_PX : UI_IMG_THUMB_W, h = kind == UI_IMG_ICON ? UI_IMG_ICON_PX : UI_IMG_THUMB_H;
        r_image(dst, rect(slot->x + 0.5f, slot->y + 0.5f, slot->x + w - 0.5f, slot->y + h - 0.5f), tint, radius);
        return 1;
    }
    if (!victim) return 0; // every slot is on screen
    ui_image_request(victim, (U32)(victim - slots), key, kind, url, 0);
    return 0;
}

//- GIFs that move
//
// One plays at a time: the one shown large (ui_gif_play, called each frame it's drawn). Its thread makes one
// frame ahead, when the one before has been taken; the UI thread puts it in the picture's texture when it's
// due and asks for a frame. It stops when it isn't drawn any more, or the window isn't the active one, and
// goes on from there when it is again.

typedef struct UI_GifFrame { U32 gen, w, h, delay_ms; B32 failed; U8 pixels[1]; } UI_GifFrame;

static struct {
    B32 started;
    OS_Mutex lock;           // gen, url, w, h, asked: what the thread should make
    OS_Semaphore sem;
    U32 gen, w, h, asked;    // gen: goes up with each other picture; asked: frames asked for of this one
    char url[1024];
    // UI thread:
    U64 key; int kind;       // the picture that plays
    UI_ImageSlot *slot;
    UI_GifFrame *next;       // made, waiting until it's due
    U64 due_ms;
    B32 drawn, paused;
} ui_gif;

static void ui_gif_arrived(U64 param, void *ptr);

static U32 ui_gif_thread(void *p)
{
    (void)p;
    Arena *a = arena_alloc(GB(1)), *frame = arena_alloc(MB(256));
    OS_Gif *g = 0;
    U32 gen = 0, made = 0;
    for (;;) {
        os_semaphore_wait(ui_gif.sem);
        os_mutex_lock(&ui_gif.lock);
        U32 want = ui_gif.gen, asked = ui_gif.asked, w = ui_gif.w, h = ui_gif.h;
        char url[1024];
        memcpy(url, ui_gif.url, sizeof(url));
        os_mutex_unlock(&ui_gif.lock);
        if (want != gen) {
            gen = want;
            made = 0;
            os_gif_close(g);
            g = 0;
            arena_clear(a);
            String8 data = ui_image_fetch ? ui_image_fetch(a, str8_cstring(url)) : str8(0, 0);
            if (data.size && data.size <= UI_IMG_MAX_BYTES * 4) g = os_gif_open(a, data);
        }
        if (made >= asked) continue;
        made += 1;
        arena_clear(frame);
        U32 delay = 0;
        OS_Image img = { 0 };
        if (g && os_gif_next(g, &delay)) img = os_gif_pixels(frame, g, w, h);
        UI_GifFrame *f = (UI_GifFrame *)os_alloc(sizeof(UI_GifFrame) + (img.w ? (U64)img.w * img.h * 4 : 0));
        if (!f) continue;
        f->gen = gen; f->w = img.w; f->h = img.h; f->delay_ms = delay;
        f->failed = !img.w;
        if (img.w) memcpy(f->pixels, img.pixels, (U64)img.w * img.h * 4);
        if (!wm_post(ui_gif_arrived, 0, f)) os_free(f);
    }
}

static void ui_gif_ask(void)
{
    os_mutex_lock(&ui_gif.lock);
    ui_gif.asked += 1;
    os_mutex_unlock(&ui_gif.lock);
    os_semaphore_signal(ui_gif.sem);
}

static void ui_gif_stop(void)
{
    if (ui_gif.next) { os_free(ui_gif.next); ui_gif.next = 0; }
    ui_gif.key = 0;
    ui_gif.slot = 0;
}

static void ui_gif_tick(void);

// Puts the next frame up if it's there and due; else comes back when it is.
static void ui_gif_advance(void)
{
    if (!ui_gif.key || !ui_gif.next) return;
    UI_ImageSlot *slot = ui_gif.slot;
    if (slot->key != ui_gif.key || slot->state != 2 || slot->picture.w != ui_gif.next->w || slot->picture.h != ui_gif.next->h) { ui_gif_stop(); return; }
    U64 now = os_now_ms();
    if (now < ui_gif.due_ms) { wm_timer_start(ui_gif_tick, (U32)(ui_gif.due_ms - now)); return; }
    if (!ui_gif.drawn || !wm_window_is_active()) { ui_gif.paused = 1; return; } // nobody sees it: ui_gif_play goes on
    r_picture_update(&slot->picture, ui_gif.next->pixels);
    // On time from frame to frame, unless it fell behind.
    ui_gif.due_ms = (now - ui_gif.due_ms > 200 ? now : ui_gif.due_ms) + ui_gif.next->delay_ms;
    os_free(ui_gif.next);
    ui_gif.next = 0;
    ui_gif.drawn = 0;
    ui_gif_ask();
    wm_request_frame();
}

static void ui_gif_tick(void)
{
    wm_timer_stop(ui_gif_tick);
    ui_gif_advance();
}

static void ui_gif_arrived(U64 param, void *ptr)
{
    (void)param;
    UI_GifFrame *f = (UI_GifFrame *)ptr;
    if (f->gen != ui_gif.gen || !ui_gif.key || ui_gif.next) { os_free(f); return; }
    if (f->failed) { if (ui_gif.slot && ui_gif.slot->key == ui_gif.key) ui_gif.slot->animated = 0; os_free(f); ui_gif_stop(); return; } // it stays as it is
    ui_gif.next = f;
    ui_gif_advance();
}

// The picture in slot was drawn, and should move.
static void ui_gif_play(UI_ImageSlot *slot, int kind, String8 url)
{
    if (!slot->animated || url.size >= sizeof(ui_gif.url) || !os_animations_on()) return;
    if (ui_gif.key != slot->key || ui_gif.kind != kind || ui_gif.slot != slot) {
        if (!ui_gif.started) {
            ui_gif.started = 1;
            ui_gif.sem = os_semaphore_alloc(1 << 20);
            os_thread_start(ui_gif_thread, 0);
        }
        ui_gif_stop();
        ui_gif.key = slot->key; ui_gif.kind = kind; ui_gif.slot = slot;
        ui_gif.due_ms = os_now_ms();
        ui_gif.paused = 0;
        os_mutex_lock(&ui_gif.lock);
        ui_gif.gen += 1;
        ui_gif.asked = 0;
        ui_gif.w = slot->picture.w; ui_gif.h = slot->picture.h;
        memcpy(ui_gif.url, url.str, url.size);
        ui_gif.url[url.size] = 0;
        os_mutex_unlock(&ui_gif.lock);
        ui_gif_ask();
    }
    ui_gif.drawn = 1;
    if (ui_gif.paused && wm_window_is_active()) { ui_gif.paused = 0; ui_gif.due_ms = os_now_ms(); wm_timer_start(ui_gif_tick, 10); }
    else ui_gif_advance(); // (timers wait while something else animates; the texture is read when the frame ends)
}

// Whether a picture's link says it's a GIF.
static B32 ui_image_url_gif(String8 url)
{
    U64 n = 0;
    while (n < url.size && url.str[n] != '?' && url.str[n] != '#') n += 1;
    return str8_ends_with(str8_prefix(url, n), str8_lit(".gif"), 1);
}

// Draws the full-size picture at url, as big as it is or scaled down to fit inside area, centered
// (*drawn: where). Returns 0 while it loads (or if it can't). One such picture is kept at a time.
static B32 ui_image_full(Rect area, String8 url, F32 radius, Rect *drawn)
{
    if (!ui_image_url_ok(url)) return 0;
    ui_image_start();
    U64 key = ui_image_key(url), now = os_now_ms();
    if (ui_image_has_failed(key, UI_IMG_FULL)) return 0;
    UI_ImageSlot *slot = &ui_img.full;
    if (slot->key != key) { ui_image_request(slot, 0, key, UI_IMG_FULL, url, 1); return 0; }
    slot->used_ms = now;
    if (slot->state != 2) return 0;
    Rect r = ui_image_fit(area, slot->picture.w, slot->picture.h, 1);
    r_picture(&slot->picture, r, ui_hex(0xffffff, 1), radius);
    ui_gif_play(slot, UI_IMG_FULL, url);
    if (drawn) *drawn = r;
    return 1;
}

// The fitted picture at url once it's loaded, else 0 (and it's asked for).
static UI_ImageSlot *ui_picture_slot(String8 url, B32 urgent)
{
    if (!ui_image_url_ok(url)) return 0;
    ui_image_start();
    U64 key = ui_image_key(url), now = os_now_ms();
    if (ui_image_has_failed(key, UI_IMG_FIT)) return 0;
    UI_ImageSlot *slot = 0, *victim = 0;
    for (U32 i = 0; i < UI_IMG_FITS; i += 1) {
        UI_ImageSlot *sl = &ui_img.fits[i];
        if (sl->key == key) { slot = sl; break; }
        if (!sl->key) { if (!victim || victim->key) victim = sl; }
        else if (sl->state == 2 && now - sl->used_ms > 250 && (!victim || (victim->key && sl->used_ms < victim->used_ms))) victim = sl;
    }
    if (!slot) {
        // (A victim's texture may be in this frame's batches only if it was drawn just now: then it isn't one.)
        if (victim) { r_picture_free(&victim->picture); ui_image_request(victim, (U32)(victim - ui_img.fits), key, UI_IMG_FIT, url, urgent); }
        return 0;
    }
    slot->used_ms = now;
    return slot->state == 2 ? slot : 0;
}

// A whole picture, kept small enough to have several (UI_IMG_FIT_MAX): draws it into area like
// ui_image_full, but scaled up at most max_scale times; play: it's the one being looked at (at most one
// picture drawn): it loads before others, and a GIF moves. Returns 0 while it loads (or if it can't).
static B32 ui_picture(Rect area, String8 url, F32 max_scale, F32 radius, B32 play, Rect *drawn)
{
    UI_ImageSlot *slot = ui_picture_slot(url, play);
    if (!slot) return 0;
    Rect r = ui_image_fit(area, slot->picture.w, slot->picture.h, max_scale);
    r_picture(&slot->picture, r, ui_hex(0xffffff, 1), radius);
    if (play) ui_gif_play(slot, UI_IMG_FIT, url);
    if (drawn) *drawn = r;
    return 1;
}

// Whether ui_picture would draw it right away (doesn't ask for it).
static B32 ui_picture_ready(String8 url)
{
    U64 key = ui_image_key(url);
    for (U32 i = 0; i < UI_IMG_FITS; i += 1) if (ui_img.fits[i].key == key) return ui_img.fits[i].state == 2;
    return 0;
}

// Loads a picture ui_picture will likely be asked for next (call it each frame, like ui_picture).
static void ui_picture_ahead(String8 url) { ui_picture_slot(url, 0); }

// The size of a picture ui_picture has loaded (it may be gone again). 0 if it isn't known.
static B32 ui_picture_size(String8 url, U32 *w, U32 *h)
{
    U64 key = ui_image_key(url);
    for (U32 i = 0; i < UI_IMG_SIZES; i += 1) if (ui_img.sizes[i].key == key) { *w = ui_img.sizes[i].w; *h = ui_img.sizes[i].h; return 1; }
    return 0;
}

static B32 ui_picture_failed(String8 url) { return !ui_image_url_ok(url) || ui_image_has_failed(ui_image_key(url), UI_IMG_FIT); }

// Goes up whenever ui_picture_size or ui_picture_failed may answer differently (to lay out again).
static U32 ui_picture_changes(void) { return ui_img.changes; }

// Whether ui_image_full failed to load url (so the caller can say so instead of "loading").
static B32 ui_image_failed(String8 url)
{
    return ui_image_has_failed(ui_image_key(url), UI_IMG_FULL);
}

static void ui_image_request(UI_ImageSlot *victim, U32 index, U64 key, int kind, String8 url, B32 urgent)
{
    U64 now = os_now_ms();
    victim->key = key;
    victim->state = 1;
    victim->used_ms = now;
    UI_ImageRequest req = { key, kind, index, urgent, { 0 } };
    memcpy(req.url, url.str, url.size);
    os_mutex_lock(&ui_img.lock);
    if (ui_img.queue_count == UI_IMG_QUEUE) {
        // Full: drop the oldest request (its slot frees up when nothing answers it... so free it now).
        UI_ImageRequest *old = &ui_img.queue[0];
        U32 oc;
        UI_ImageSlot *os_ = &ui_image_slots(old->kind, &oc)[old->slot];
        if (os_->key == old->key && os_->state == 1) { os_->key = 0; os_->state = 0; }
        memmove(&ui_img.queue[0], &ui_img.queue[1], sizeof(UI_ImageRequest) * (UI_IMG_QUEUE - 1));
        ui_img.queue_count -= 1;
    }
    ui_img.queue[ui_img.queue_count++] = req;
    os_mutex_unlock(&ui_img.lock);
    os_semaphore_signal(ui_img.sem);
    if (urgent) os_semaphore_signal(ui_img.urgent_sem);
}

// A picture, or the first letter of name on bg (in fg) until (or unless) one loads.
static void ui_avatar(Rect r, String8 url, String8 name, V4 fg, V4 bg, F32 radius)
{
    if (url.size && ui_image(r, url, UI_IMG_ICON, radius, ui_hex(0xffffff, 1))) return;
    r_rect(r, bg, radius, 0, 0.5f);
    name = str8_trim(name);
    U64 n = 1;
    while (n < name.size && (name.str[n] & 0xC0) == 0x80) n += 1;
    U8 letter[8] = { 0 };
    memcpy(letter, name.str, Min(n, 7));
    if (letter[0] >= 'a' && letter[0] <= 'z') letter[0] -= 32;
    ui_text_centered((r.y1 - r.y0) > 44 * ui.scale ? ui.font.title : ui.font.bold, r, str8(letter, name.size ? Min(n, 7) : 0), fg);
}
