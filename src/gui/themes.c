// gui/themes.c - the looks the user can pick in Settings > Appearance: the UI library's two
// (Tinker, Slate) and a set drawn from WoW's schools of magic and ways of fighting. Each has its
// own ornaments (ui.h's UI_STYLE_*: the divider's emblem, the panel corners, the spinner) and two
// colors played against each other: the gem's and the spark's. The choice is saved in
// settings.json ("theme") and applied at startup. Part of main_gui.c's unity build.

// A theme's colors as 0xRRGGBB. ok/danger/danger_hi/error/warn left 0 take Tinker's.
typedef struct ThemeHex {
    U32 bg, panel, hover, button, button_hi, border, border_hi, selected, line;
    U32 text, dim, faint, heading;
    U32 accent, accent_hi, accent_bg;
    U32 trim_hi, trim, trim_dark, gem;
    U32 ok, danger, danger_hi, error, warn;
    int style;
    U32 spark, aura;
    F32 aura_alpha;
} ThemeHex;

static UI_Theme theme_from_hex(ThemeHex h)
{
    UI_Theme t = ui_theme_tinker(); // the status colors when not given, and ornate 1
    t.bg = ui_hex(h.bg, 1); t.panel = ui_hex(h.panel, 1); t.hover = ui_hex(h.hover, 1);
    t.button = ui_hex(h.button, 1); t.button_hi = ui_hex(h.button_hi, 1);
    t.border = ui_hex(h.border, 1); t.border_hi = ui_hex(h.border_hi, 1);
    t.selected = ui_hex(h.selected, 1); t.line = ui_hex(h.line, 1);
    t.text = ui_hex(h.text, 1); t.dim = ui_hex(h.dim, 1); t.faint = ui_hex(h.faint, 1); t.heading = ui_hex(h.heading, 1);
    t.accent = ui_hex(h.accent, 1); t.accent_hi = ui_hex(h.accent_hi, 1); t.accent_bg = ui_hex(h.accent_bg, 1);
    t.trim_hi = ui_hex(h.trim_hi, 1); t.trim = ui_hex(h.trim, 1); t.trim_dark = ui_hex(h.trim_dark, 1); t.gem = ui_hex(h.gem, 1);
    if (h.ok) t.ok = ui_hex(h.ok, 1);
    if (h.danger) t.danger = ui_hex(h.danger, 1);
    if (h.danger_hi) t.danger_hi = ui_hex(h.danger_hi, 1);
    if (h.error) t.error = ui_hex(h.error, 1);
    if (h.warn) t.warn = ui_hex(h.warn, 1);
    t.style = h.style;
    t.spark = ui_hex(h.spark, 1);
    t.aura = ui_hex(h.aura, h.aura_alpha);
    return t;
}

// Shadow magic: violet void in silver crescents, teal stars, a violet glow.
static UI_Theme theme_void(void)
{
    return theme_from_hex((ThemeHex){
        .bg = 0x0a0910, .panel = 0x120f1b, .hover = 0x1b1628, .button = 0x261e38, .button_hi = 0x322849,
        .border = 0x29213e, .border_hi = 0x423560, .selected = 0x231a3a, .line = 0x191426,
        .text = 0xece8f5, .dim = 0xa8a1ba, .faint = 0x6b647b, .heading = 0xb4efe6,
        .accent = 0x8a4fd8, .accent_hi = 0xa775ee, .accent_bg = 0x4a2580,
        .trim_hi = 0xd8ccf0, .trim = 0x7c6aa2, .trim_dark = 0x211836, .gem = 0x9b5cf0,
        .ok = 0x5fe3c8,
        .style = UI_STYLE_VOID, .spark = 0x6ff0dc, .aura = 0x6a2fc0, .aura_alpha = 0.16f,
    });
}

// Frost magic: ice crystals in silver, an arcane violet glint.
static UI_Theme theme_frost(void)
{
    return theme_from_hex((ThemeHex){
        .bg = 0x08101a, .panel = 0x0d1824, .hover = 0x142232, .button = 0x1b2d41, .button_hi = 0x253b54,
        .border = 0x1e3146, .border_hi = 0x344f6c, .selected = 0x162c44, .line = 0x121f2d,
        .text = 0xedf6fc, .dim = 0xa2b5c5, .faint = 0x637586, .heading = 0xe6f3ff,
        .accent = 0x3d9be0, .accent_hi = 0x66b8f2, .accent_bg = 0x1b4f7d,
        .trim_hi = 0xeef6ff, .trim = 0x8aa2ba, .trim_dark = 0x1c2b3a, .gem = 0x8fdcff,
        .ok = 0x5fd0a0,
        .style = UI_STYLE_FROST, .spark = 0xb7a0ff, .aura = 0x3a8fd0, .aura_alpha = 0.14f,
    });
}

// Fire magic: flames and embers in blackened iron, a red glow, gold headings.
static UI_Theme theme_inferno(void)
{
    return theme_from_hex((ThemeHex){
        .bg = 0x0f0907, .panel = 0x170f0b, .hover = 0x221610, .button = 0x2e1e15, .button_hi = 0x3c281b,
        .border = 0x32211a, .border_hi = 0x51362a, .selected = 0x331f0f, .line = 0x1f140f,
        .text = 0xf7ece1, .dim = 0xbda896, .faint = 0x7c6858, .heading = 0xffc857,
        .accent = 0xd9641a, .accent_hi = 0xf0802e, .accent_bg = 0x7a330c,
        .trim_hi = 0xb09888, .trim = 0x5c4a40, .trim_dark = 0x1c1411, .gem = 0xff6a1a,
        .style = UI_STYLE_FLAME, .spark = 0xffd75a, .aura = 0xc0300a, .aura_alpha = 0.16f,
    });
}

// Fel magic: fel green on demon-hunter purple, horned eyes, magenta sigils.
static UI_Theme theme_fel(void)
{
    return theme_from_hex((ThemeHex){
        .bg = 0x0c0910, .panel = 0x140f19, .hover = 0x1d1624, .button = 0x281e31, .button_hi = 0x33273f,
        .border = 0x2a2034, .border_hi = 0x433452, .selected = 0x241c30, .line = 0x1a1420,
        .text = 0xeef0e8, .dim = 0xa9a3b0, .faint = 0x6c6676, .heading = 0xa8ff3c,
        .accent = 0x4f9a12, .accent_hi = 0x6cbf22, .accent_bg = 0x2c560b,
        .trim_hi = 0xbfa3d8, .trim = 0x5e4a74, .trim_dark = 0x1e1528, .gem = 0x8cff2e,
        .ok = 0x4fd1b5, // (green is the accent here)
        .style = UI_STYLE_FEL, .spark = 0xd04ae0, .aura = 0x4fd01a, .aura_alpha = 0.10f,
    });
}

// Brutal melee: crossed swords and spikes in battered iron, blood red, bronze and gold.
static UI_Theme theme_bloodrage(void)
{
    return theme_from_hex((ThemeHex){
        .bg = 0x110c0a, .panel = 0x191210, .hover = 0x231a17, .button = 0x2f2320, .button_hi = 0x3c2d29,
        .border = 0x31241f, .border_hi = 0x4d3932, .selected = 0x2f1b18, .line = 0x201715,
        .text = 0xf2e8e2, .dim = 0xb3a39b, .faint = 0x76675f, .heading = 0xe9b65c,
        .accent = 0xb3261e, .accent_hi = 0xd23b2e, .accent_bg = 0x6a1712,
        .trim_hi = 0xd0c9c1, .trim = 0x6e6660, .trim_dark = 0x221d1a, .gem = 0xe0322a,
        .danger = 0xa8421a, .danger_hi = 0xc95527, // (red is the accent here: danger leans to rust)
        .style = UI_STYLE_BLOOD, .spark = 0xd9a441, .aura = 0x9a1a10, .aura_alpha = 0.16f,
    });
}

// Holy light: gold sunbursts around pearl, a sky-blue shimmer.
static UI_Theme theme_radiance(void)
{
    return theme_from_hex((ThemeHex){
        .bg = 0x12100c, .panel = 0x1b1711, .hover = 0x252017, .button = 0x322b1f, .button_hi = 0x413829,
        .border = 0x372f21, .border_hi = 0x5a4c34, .selected = 0x222838, .line = 0x221d15,
        .text = 0xfcf7ea, .dim = 0xc5baa2, .faint = 0x857b64, .heading = 0xfff3d0,
        .accent = 0xc4961a, .accent_hi = 0xe0b43c, .accent_bg = 0x6e520e,
        .trim_hi = 0xfff1c4, .trim = 0xc9a24a, .trim_dark = 0x3d2f12, .gem = 0xfff6e0,
        .ok = 0x7fd6b0,
        .style = UI_STYLE_HOLY, .spark = 0x8cc8ff, .aura = 0xffd27a, .aura_alpha = 0.10f,
    });
}

// Nature: leaves on oak, amber headings, autumn berries.
static UI_Theme theme_wildgrowth(void)
{
    return theme_from_hex((ThemeHex){
        .bg = 0x0a0e0a, .panel = 0x10170f, .hover = 0x172116, .button = 0x202d1d, .button_hi = 0x2a3a26,
        .border = 0x243221, .border_hi = 0x3b5035, .selected = 0x1c2c1a, .line = 0x152013,
        .text = 0xeff0e3, .dim = 0xaaae98, .faint = 0x6b705e, .heading = 0xf0c060,
        .accent = 0x3f8f4a, .accent_hi = 0x58ad62, .accent_bg = 0x1f5028,
        .trim_hi = 0xdcbd8c, .trim = 0x8a6a3e, .trim_dark = 0x2e2213, .gem = 0x5fd06a,
        .ok = 0x7fd0e0,
        .style = UI_STYLE_NATURE, .spark = 0xe0703a, .aura = 0x3f9a6a, .aura_alpha = 0.12f,
    });
}

// The elements: storm teal in copper rings, lightning yellow.
static UI_Theme theme_storm(void)
{
    return theme_from_hex((ThemeHex){
        .bg = 0x071012, .panel = 0x0c181b, .hover = 0x122226, .button = 0x182d32, .button_hi = 0x213b41,
        .border = 0x1b3035, .border_hi = 0x2e4d54, .selected = 0x13303a, .line = 0x10201f,
        .text = 0xe8f6f5, .dim = 0x9db6b5, .faint = 0x607777, .heading = 0xfff0a8,
        .accent = 0x1f9ea6, .accent_hi = 0x3cc0c6, .accent_bg = 0x0f555b,
        .trim_hi = 0xf0b98a, .trim = 0xa8663a, .trim_dark = 0x35200f, .gem = 0x5fe8ff,
        .style = UI_STYLE_STORM, .spark = 0xfff27a, .aura = 0x1fb0c0, .aura_alpha = 0.12f,
    });
}

typedef struct ThemeInfo {
    char *key;     // what settings.json keeps
    char *name;    // what the picker shows
    char *about;   // its tooltip
    UI_Theme (*make)(void);
} ThemeInfo;

static ThemeInfo themes[] = {
    { "tinker",      "Tinker",      "Gnomish engineering: cogs, rivets and cut sapphires in gold and bronze",  ui_theme_tinker },
    { "slate",       "Slate",       "Plain and quiet: flat gray-blue, no ornaments",                           ui_theme_dark },
    { "void",        "Void",        "Shadow magic: void orbs in silver crescents, teal stars",                 theme_void },
    { "frostbound",  "Frostbound",  "Frost magic: ice crystals and shards in silver, an arcane glint",         theme_frost },
    { "inferno",     "Inferno",     "Fire magic: flames and embers in blackened iron",                         theme_inferno },
    { "fel",         "Fel",         "Fel magic: horned fel eyes and magenta sigils on demon-hunter purple",    theme_fel },
    { "bloodrage",   "Bloodrage",   "Brutal melee: crossed swords, iron spikes, blood and bronze",             theme_bloodrage },
    { "radiance",    "Radiance",    "Holy light: golden sunbursts around pearl, a sky-blue shimmer",           theme_radiance },
    { "wildgrowth",  "Wildgrowth",  "Nature: leaves on oak, amber and autumn berries",                         theme_wildgrowth },
    { "stormcaller", "Stormcaller", "The elements: lightning over storm orbs in copper rings",                 theme_storm },
};
#define THEME_COUNT ((int)ArrayCount(themes))

static int theme_current; // index in themes

static int theme_index(String8 key)
{
    for (int i = 0; i < THEME_COUNT; i += 1) if (str8_match(key, str8_cstring(themes[i].key), 1)) return i;
    return 0; // unknown or none saved: Tinker
}

static void theme_apply(int i)
{
    theme_current = Clamp(0, i, THEME_COUNT - 1);
    ui.theme = themes[theme_current].make();
    wm_request_frame();
}
