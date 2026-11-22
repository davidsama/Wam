// ui_image.c - pictures from URLs (icons, screenshot thumbnails), loaded in the background.
//
// ui_image(dst, url, kind, ...) draws the picture if it's loaded and otherwise asks for it, so a
// view just calls it every frame for what's on screen. ui_image_full(area, url, ...) does the same
// for one picture at full size (a screenshot being viewed), fitted inside area. Loader threads fetch the bytes through the
// app's ui_image_fetch callback (which can cache them on disk), decode them (os_image_decode), scale them to
// the kind's slot size (icons: square, cropped to fill; thumbnails: 16:9, cropped to fill) and post
// the pixels to the UI thread, which copies them into the renderer's image atlas.
//
// A url "res:NAME" is the program's own RCDATA resource NAME (e.g. a logo built into the .exe),
// read directly instead of through ui_image_fetch.
//
// The atlas holds a fixed number of slots per kind; the least recently drawn picture gives up its
// slot. Requests are served newest first, and a request whose picture went off screen meanwhile is
// skipped, so scrolling through a long list loads what's visible now, not what was visible before.
// Pictures that fail to load are remembered and not asked for again.
//
// Needs: render (r_image, r_images_update, R_IMAGES_SIZE), wm_post, and os_image_decode.

typedef enum UI_ImageKind { UI_IMG_ICON, UI_IMG_THUMB, UI_IMG_FULL, UI_IMG_KIND_COUNT } UI_ImageKind;

// Returns the bytes at url (in a), or empty. Called on loader threads.
typedef String8 UI_ImageFetch(Arena *a, String8 url);
static UI_ImageFetch *ui_image_fetch;
// Testing: when set, every picture that fails to load is appended to this file with the reason.
static String8 ui_image_log;

#define UI_IMG_ICON_PX   64             // icon slots: 64x64, the top half of the atlas
#define UI_IMG_THUMB_W   320            // thumbnail slots: 320x180, the bottom half
#define UI_IMG_THUMB_H   180
#define UI_IMG_FULL_MAX  2560           // the full-size picture: at most this wide or tall (its own texture)
#define UI_IMG_THREADS   4
#define UI_IMG_QUEUE     256
#define UI_IMG_FAILED    1024
#define UI_IMG_MAX_BYTES (12 << 20)

typedef struct UI_ImageSlot {
    U64 key;                // hash of the URL, 0 = free
    U8 state;               // 1 loading, 2 ready
    volatile U64 used_ms;   // last drawn (loader threads read it to skip stale requests)
    U16 x, y;               // in the atlas
    U16 w, h;               // the full-size picture: its size
} UI_ImageSlot;

typedef struct UI_ImageRequest { U64 key; int kind; U32 slot; char url[1024]; } UI_ImageRequest;
typedef struct UI_ImageResult { U64 key; int kind; U32 slot; U32 w, h; B32 ok, skipped; U8 pixels[1]; } UI_ImageResult;

#define UI_IMG_ICON_SLOTS  ((R_IMAGES_SIZE / UI_IMG_ICON_PX) * ((R_IMAGES_SIZE / 2) / UI_IMG_ICON_PX))
#define UI_IMG_THUMB_SLOTS ((R_IMAGES_SIZE / UI_IMG_THUMB_W) * ((R_IMAGES_SIZE / 2) / UI_IMG_THUMB_H))

static struct {
    B32 started;
    UI_ImageSlot icons[UI_IMG_ICON_SLOTS];
    UI_ImageSlot thumbs[UI_IMG_THUMB_SLOTS];
    UI_ImageSlot full;
    U64 failed[UI_IMG_FAILED];
    U32 failed_next;
    OS_Mutex lock;          // the queue
    OS_Semaphore sem;
    UI_ImageRequest queue[UI_IMG_QUEUE];
    U32 queue_count;
} ui_img;

static U64 ui_image_key(String8 url)
{
    U64 h = 14695981039346656037ull;
    for (U64 i = 0; i < url.size; i += 1) { h ^= url.str[i]; h *= 1099511628211ull; }
    return h ? h : 1;
}

static UI_ImageSlot *ui_image_slots(int kind, U32 *count)
{
    if (kind == UI_IMG_ICON) { *count = UI_IMG_ICON_SLOTS; return ui_img.icons; }
    if (kind == UI_IMG_FULL) { *count = 1; return &ui_img.full; }
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
    res->w = img.w; res->h = img.h;
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
    (void)p;
    Arena *a = arena_alloc(GB(1));
    for (;;) {
        os_semaphore_wait(ui_img.sem);
        os_mutex_lock(&ui_img.lock);
        B32 have = ui_img.queue_count > 0;
        UI_ImageRequest req = { 0 };
        if (have) req = ui_img.queue[--ui_img.queue_count]; // newest first
        os_mutex_unlock(&ui_img.lock);
        if (!have) continue;

        U32 w = req.kind == UI_IMG_ICON ? UI_IMG_ICON_PX : req.kind == UI_IMG_FULL ? UI_IMG_FULL_MAX : UI_IMG_THUMB_W;
        U32 h = req.kind == UI_IMG_ICON ? UI_IMG_ICON_PX : req.kind == UI_IMG_FULL ? UI_IMG_FULL_MAX : UI_IMG_THUMB_H;
        U32 count;
        UI_ImageSlot *slot = &ui_image_slots(req.kind, &count)[req.slot];
        UI_ImageResult *res = 0;
        if (os_now_ms() - slot->used_ms <= 1500 && ui_image_fetch) { // else: off screen while it waited
            arena_clear(a);
            String8 url = str8_cstring(req.url);
            String8 data = str8_starts_with(url, str8_lit("res:"), 0) ? os_resource(str8_skip(url, 4)) : ui_image_fetch(a, url);
            if (data.size && data.size <= UI_IMG_MAX_BYTES * (req.kind == UI_IMG_FULL ? 4 : 1)) res = ui_image_decode(a, data, w, h, req.kind == UI_IMG_FULL);
            if (res) res->ok = 1;
            else ui_image_log_failure(url, !data.size ? "fetch failed" : data.size > UI_IMG_MAX_BYTES * (req.kind == UI_IMG_FULL ? 4 : 1) ? "too big" : "decode failed", data.size);
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
        if (res->ok && res->kind == UI_IMG_FULL) {
            slot->state = r_picture_set(res->w, res->h, res->pixels) ? 2 : 0;
            slot->w = (U16)res->w; slot->h = (U16)res->h;
            if (!slot->state) slot->key = 0;
        } else if (res->ok) {
            r_images_update(slot->x, slot->y, res->w, res->h, res->pixels, res->w * 4);
            slot->state = 2;
        } else {
            if (!res->skipped) { ui_img.failed[ui_img.failed_next++ % UI_IMG_FAILED] = res->key; }
            slot->key = 0; slot->state = 0;
        }
        wm_request_frame();
    }
    os_free(res);
}

// Draws url's picture into dst if it's loaded (returns 1), else asks for it and returns 0 (the
// caller draws a placeholder). radius rounds the corners; tint multiplies (white = as is).
static void ui_image_request(UI_ImageSlot *slot, U32 index, U64 key, int kind, String8 url);

static B32 ui_image(Rect dst, String8 url, UI_ImageKind kind, F32 radius, V4 tint)
{
    if (!url.size || !r_state->images) return 0;
    ui_image_start();
    U64 key = ui_image_key(url), now = os_now_ms();
    for (U32 i = 0; i < UI_IMG_FAILED; i += 1) if (ui_img.failed[i] == key) return 0;
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
    ui_image_request(victim, (U32)(victim - slots), key, kind, url);
    return 0;
}

// Draws the full-size picture at url, as big as it is or scaled down to fit inside area, centered
// (*drawn: where). Returns 0 while it loads (or if it can't). One such picture is kept at a time.
static B32 ui_image_full(Rect area, String8 url, F32 radius, Rect *drawn)
{
    if (!url.size || !str8_starts_with(url, str8_lit("https://"), 1) || url.size >= sizeof(((UI_ImageRequest *)0)->url)) return 0;
    ui_image_start();
    U64 key = ui_image_key(url), now = os_now_ms();
    for (U32 i = 0; i < UI_IMG_FAILED; i += 1) if (ui_img.failed[i] == key) return 0;
    UI_ImageSlot *slot = &ui_img.full;
    if (slot->key != key) { ui_image_request(slot, 0, key, UI_IMG_FULL, url); return 0; }
    slot->used_ms = now;
    if (slot->state != 2) return 0;
    F32 aw = area.x1 - area.x0, ah = area.y1 - area.y0;
    F32 k = Min(1.0f, Min(aw / slot->w, ah / slot->h));
    F32 w = slot->w * k, h = slot->h * k;
    F32 x = (F32)(int)(area.x0 + (aw - w) * 0.5f), y = (F32)(int)(area.y0 + (ah - h) * 0.5f);
    Rect r = rect(x, y, x + w, y + h);
    r_picture(r, ui_hex(0xffffff, 1), radius);
    if (drawn) *drawn = r;
    return 1;
}

// Whether url failed to load (so the caller can say so instead of "loading").
static B32 ui_image_failed(String8 url)
{
    U64 key = ui_image_key(url);
    for (U32 i = 0; i < UI_IMG_FAILED; i += 1) if (ui_img.failed[i] == key) return 1;
    return 0;
}

static void ui_image_request(UI_ImageSlot *victim, U32 index, U64 key, int kind, String8 url)
{
    U64 now = os_now_ms();
    victim->key = key;
    victim->state = 1;
    victim->used_ms = now;
    UI_ImageRequest req = { key, kind, index, { 0 } };
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
}
