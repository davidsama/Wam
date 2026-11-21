// gui/feedback.c - what the user just asked for shows at once. Installs, updates and removals run
// on the worker one after another (an install downloads, a removal moves the folders to the Recycle
// Bin, and either may wait behind a Check for updates), and the Installed list only changes when
// the worker's next listing arrives. Until then:
//   removing: the rows are hidden right away; they come back if the removal failed
//   updating: the rows say "Updating..." instead of showing their Update button
//   installing: a row at the top of Installed ("Installing <name>..."), and "Installing..." on the
//               addon's Install button in Get addons
// Each lasts until the worker says that task is done (WM_APP_TASK_DONE carries its id; the listing
// it made arrives just before). Part of main_gui.c's unity build.

static void feedback_install(U32 task, String8 id, String8 name, String8 icon)
{
    if (!task) return;
    if (app.installing_count == ArrayCount(app.installing)) { // the oldest goes
        memmove(&app.installing[0], &app.installing[1], sizeof(app.installing[0]) * (ArrayCount(app.installing) - 1));
        app.installing_count -= 1;
    }
    PendingInstall *p = &app.installing[app.installing_count++];
    memset(p, 0, sizeof(*p));
    p->task = task;
    p->flavor = app.flavor;
    copy_cstr(p->id, sizeof(p->id), id);
    copy_cstr(p->name, sizeof(p->name), name.size ? name : str8_lit("addon"));
    copy_cstr(p->icon, sizeof(p->icon), icon);
    p->src = id.size ? source_of_id(id) : -1;
    ui_request_frame();
}

static void feedback_remove(U32 task, String8List *keys)
{
    if (!task) return;
    for (String8Node *n = keys->first; n; n = n->next) {
        keyset_add(&app.gone, n->string);
        keyset_remove(&app.selected, n->string);
    }
    app.gone_task = task;
}

static void feedback_update(U32 task, String8List *keys)
{
    if (!task) return;
    for (String8Node *n = keys->first; n; n = n->next) keyset_add(&app.updating, n->string);
    app.updating_task = task;
}

// The worker finished task `id` (tasks finish in order, so all before it are done too).
static void feedback_task_done(U32 id)
{
    if (!id) return;
    app.tasks_done = Max(app.tasks_done, id);
    if (app.gone.count && app.tasks_done >= app.gone_task) keyset_clear(&app.gone);
    if (app.updating.count && app.tasks_done >= app.updating_task) keyset_clear(&app.updating);
    U32 kept = 0;
    for (U32 i = 0; i < app.installing_count; i += 1) if (app.installing[i].task > app.tasks_done) app.installing[kept++] = app.installing[i];
    app.installing_count = kept;
}

// An install of this addon is on its way.
static PendingInstall *feedback_installing(String8 id)
{
    for (U32 i = 0; i < app.installing_count; i += 1) {
        if (app.installing[i].flavor == app.flavor && str8_match(str8_cstring(app.installing[i].id), id, 1)) return &app.installing[i];
    }
    return 0;
}

//- Progress bars for installs (the worker publishes the running task's progress: work_task, work_permille)

// How far install p is: 0..1, -1 under way but how far isn't known, -2 still waiting its turn.
static F32 feedback_install_progress(PendingInstall *p)
{
    if ((LONG)p->task != work_task) return p->task > app.tasks_done ? -2.0f : 1.0f;
    LONG v = work_permille;
    return v < 0 ? -1.0f : (F32)v / 1000.0f;
}

// "Installing 45%", "Installing..." (how far isn't known) or "Waiting..." (its turn hasn't come).
static String8 feedback_install_label(PendingInstall *p)
{
    F32 f = feedback_install_progress(p);
    if (f < -1.5f) return str8_lit("Waiting...");
    if (f < 0) return str8_lit("Installing...");
    return str8_pushf(ui.frame_arena, "Installing %d%%", (int)(f * 100.0f + 0.5f));
}

// p's progress where its Install button was (r): the button's shape, filled from the left as far as
// the install is, with "Installing 45%" on it; a band sliding across while how far isn't known;
// "Waiting..." while its turn hasn't come. Returns 1 while it should keep animating.
static B32 draw_install_progress(Rect r, PendingInstall *p)
{
    F32 s = ui.scale, rad = 6 * s, w = r.x1 - r.x0;
    F32 f = feedback_install_progress(p);
    B32 animating = 0;
    r_rect(r, ui.theme.button, rad, 0, 0.5f);
    r_rect(r, ui.theme.border, rad, ui_px(), 0.5f);
    if (f >= 0) {
        if (f < p->shown) p->shown = f;            // (follows at once should it ever go back)
        p->shown += (f - p->shown) * 0.25f;        // eases toward it, frame by frame
        if (f - p->shown < 0.002f) p->shown = f;
        animating = p->shown < f;
        F32 fw = w * p->shown;
        if (fw >= 1) r_rect_gradient(rect(r.x0, r.y0, r.x0 + fw, r.y1), ui.theme.accent_bg, ui.theme.accent, Min(rad, fw * 0.5f), 0, 0.5f, 1);
    } else if (f > -1.5f) {
        F32 t = (F32)fmod((F64)GetTickCount64() / 1100.0, 1.0), seg = w * 0.35f, x0 = r.x0 + (w + seg) * t - seg;
        Rect band = rect(Max(x0, r.x0), r.y0, Min(x0 + seg, r.x1), r.y1);
        if (band.x1 > band.x0) r_rect(band, ui_mix(ui.theme.accent_bg, ui.theme.button, 0.3f), rad, 0, 0.5f);
        animating = 1;
    }
    ui_text_centered(ui.font.caption, r, feedback_install_label(p), f < -1.5f ? ui.theme.dim : ui.theme.text);
    return animating;
}

// This flavor's installs on their way (placeholder rows), into out; returns how many.
static U32 feedback_installs(PendingInstall **out, U32 cap)
{
    U32 n = 0;
    for (U32 i = 0; i < app.installing_count && n < cap; i += 1) if (app.installing[i].flavor == app.flavor) out[n++] = &app.installing[i];
    return n;
}
