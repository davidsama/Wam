# Contributing to wam

Thanks for wanting to help. Bug reports and ideas go in [issues](../../issues); for anything bigger
than a small fix, open an issue first so we can agree on the approach before you spend time on it.

What a change should keep true:

- **Plain C, no dependencies beyond Windows.** wam has its own JSON parser, zip/DEFLATE reader,
  D3D11 renderer and UI library; RAR and 7z go through the `tar.exe` that ships with Windows. Please
  don't add libraries or a runtime: `wam.exe` stays one small file.
- **`build.bat test` passes**, and new behavior comes with a test where it can (`src/main_test.c`,
  offline: no network).
- **Respect the sites wam uses**: never work around CurseForge's per-addon download settings, build
  its file URLs or load its site out of sight (see Sources below), and keep requests within each
  API's limits.
- **The code explains itself**: comments say why, not what; names are plain. Look at the file you're
  changing and match it.

The rest of this file explains how wam is built, how it works and how the code is laid out.

## Build

```
vsenv               once per window: sets up the C/C++ tools (tools\vsenv.bat)
build.bat           debug
build.bat release   optimized
build.bat test      build + run offline unit tests (wamtest.exe)
```

`build.bat` doesn't set up the compiler itself (that took a couple of seconds on every build): run
`vsenv` first, once per window. `tools\vsenv.bat` finds Visual Studio or its Build Tools (vswhere)
and runs `vcvars64.bat` (x64), staying in the current folder; copy it to a folder on your PATH to
have it everywhere. In cmd it sets up that window. In PowerShell, which can't take a `.bat`'s
environment, it starts a set-up PowerShell in the same window, its prompt starting with `[vs]`;
`exit` goes back. `vsenv --here` only sets up the cmd it runs in (what `tools\release.ps1` uses).
Without a `cf_key.txt` the build works the same; only CurseForge needs a key (see below).

Produces `build\wam.exe`. Needs VS 2022 / Build Tools. The
first build fetches Microsoft's WebView2 SDK header and static loader (`tools\fetch_webview2.ps1`,
pinned version, SHA-256 checked) into `build\webview2\`; the loader is linked into `wam.exe`, so it
stays a single file.

The icon (`res\wam.ico`, built into `wam.exe` from `res\wam.rc`) and the README's logo
(`screenshots\logo.png`) are drawn by `tools\make_icon.c` from shapes (a gold W with a gem over its
middle), every size from the shapes themselves. The header's emblem and wordmark are drawn by wam
(`src\ui\ui.c`, `src\gui\wordmark.c`). To change them, edit the shapes and run it from the repo root:

```
cl /nologo /O2 tools\make_icon.c /Fe:build\make_icon.exe /Fo:build\ ole32.lib windowscodecs.lib
build\make_icon.exe
```

## Releasing

For maintainers. `tools\release.ps1` builds, checks and publishes a release on GitHub (it needs the
GitHub CLI: `winget install GitHub.cli`, then `gh auth login`).

The version is set in one place, `src\version.h` (`WAM_VERSION` and its three numbers). wam.exe's
file properties (`res\version.rc`), Settings > About and the user agent sent to the addon sites all
read it. To release a new version:

1. Set it in `src\version.h`, and write its `## 1.2.3` section in `CHANGES.md` (the release notes).
2. Commit both on `main` and push.
3. Check, then publish:

```
powershell -ExecutionPolicy Bypass -File tools\release.ps1 -DryRun     build, check, stage
powershell -ExecutionPolicy Bypass -File tools\release.ps1             publish
```

A release needs a clean `main` that's pushed, `cf_key.txt`, and the changelog section. `-Version
1.2.3`, if given, must match `version.h`. The script runs the tests and a release build, stages
`wam.exe`, `SHA256SUMS.txt` and `THIRD_PARTY_NOTICES.md` in `build\release\<version>`, and refuses if a `.pdb`, this folder's
path, the plain CurseForge key or this PC's git user name is in it, or if its version resource says
another version (`/PDBALTPATH` keeps the build path out of the exe). It then creates the GitHub
release `<version>` (no "v", like 1.0.0) on the commit it was built from, with the hashes in its
notes. The repository is the `origin` remote's (`-Repo owner/name` or `WAM_REPO` override it).

## Sources

| Source | Needs | Install with | Recognizes existing installs |
|---|---|---|---|
| GitHub releases | nothing | `owner/repo` or a github.com link | no (install once through wam) |
| WoWInterface | nothing | a wowinterface.com link or `wowi:5108` | yes, via `## X-WoWI-ID` in the .toc |
| CurseForge | API key | project id, curseforge.com link, or search | yes, via fingerprints |
| Wago Addons | your own Wago API key (Wago subscription) | addons.wago.io link, `wago:<id>`, or search | yes, via fingerprints (and `## X-Wago-ID`) |
| Tukui (ElvUI, Tukui) | nothing | search, a tukui.org link, or `tukui:elvui` | yes, by their folders |

**CurseForge projects whose author disabled third-party downloads** (`allowModDistribution: false`,
e.g. Details!, DBM): if the project's source link is a GitHub repo with a current release
(no more than 14 days older than the CurseForge file), wam installs from GitHub instead and tracks
it as `gh:…` from then on. Otherwise the file can only come from CurseForge's own site, so the
GUI opens that exact file's download page (`curseforge.com/wow/addons/<slug>/download/<fileId>`,
which starts the download by itself after a few seconds; no clicking on the site) in a small,
visible wam window (WebView2, Edge's engine in Windows 10/11). It catches the download, closes,
and wam installs the archive as that CurseForge file, so update checks keep working and the next
update goes the same way. Several such addons at once (Update all): the status line offers "Get N
from CurseForge" and the window fetches them one after another. Without WebView2 the page opens in
your browser instead and wam installs the archive when one with the expected name and size lands
in a download folder. The window's browser profile (cookies) is kept in
`%LOCALAPPDATA%\wam\webview2`; downloads go to `%TEMP%\wam-cf` and are deleted once installed.
So the window never comes as a surprise, the GUI says beforehand which addons go this way: in Get
addons they carry an amber **↗ website download** tag (and a grey **via GitHub** tag when a GitHub
source link exists, so the release usually comes from there), their Install button reads
**Install ↗** (**Update ↗** in Installed), and the detail panel explains what will happen. The
window itself starts with a strip saying why it opened, that it closes by itself, and that closing
it cancels.
wam never builds CurseForge's file URLs itself or loads its site out of sight: that would go
against the author's choice and CurseForge's terms, and could get the app's API key revoked for
everyone.

GitHub picks the zip for your game version from the BigWigs-packager `release.json` (exact
interface, e.g. Forever = 16001), falling back to zip names (`-classic`, `-mists`, ...).
Checks and installs don't use the GitHub API (60 requests per hour per IP without a token): the
newest tag comes from the redirect of `github.com/<repo>/releases/latest`, and `release.json` and
the zip from `releases/download/<tag>/`. Only repositories without a `release.json`, and the
CurseForge fallback (which compares release dates), use the API. For GitHub and Tukui addons an
update is a different version.

**Tukui** (`api.tukui.org`, no key) hosts ElvUI and Tukui, which aren't on CurseForge. They show up
in search, install for every flavor their `patch` list covers, and existing installs are recognized
by the folders Tukui lists for them.

**Wago** has no keyless API; WowUp's free Wago access comes from a partnership with Wago (an ad page
hands it a token). wam uses the key Wago gives its subscribers instead: paste it under
**Settings** (the cog at the top). It's stored in `%APPDATA%\wam\wago.key`,
encrypted with DPAPI so only your Windows user can read it; `WAGO_API_KEY` overrides it. With a key,
wam lists Wago's popular addons in Get addons (browse them by category like the rest), searches Wago
(results marked Wago / CurseForge), shows their details (description, screenshots, changelog), installs `addons.wago.io` links, recognizes
installed Wago addons first (Wago carries addons like Details! whose authors block other apps on
CurseForge), and checks them for updates. wam follows Wago's stable channel.

## CurseForge key

Users never need a key if the build embeds one:

1. Apply for a key for this app at https://console.curseforge.com.
2. Put it in `cf_key.txt` in the repo root (gitignored), then `build.bat release`.
   The build prints `embedded CurseForge key (N chars)`.

`tools\gen_key.ps1` XORs the key into `build\cf_key.h`, so it isn't a plain string in the exe.
That only deters casual extraction. If the key is ever abused, CurseForge can revoke it; the
robust fix is a small proxy server that holds the key. A user's `CURSEFORGE_API_KEY`
environment variable overrides the embedded key.

**Rate limits and a rejected key**: when CurseForge or Wago answers 429 (too many requests), wam
pauses that API for its `Retry-After` (else a minute, kept within 5 s to 5 min); requests during the
pause fail at once with a message saying how long, without touching the network, so wam never keeps
knocking. If CurseForge rejects the embedded key (revoked), wam says so and points to
`CURSEFORGE_API_KEY` for the user's own key; a rejected user key is named as theirs. Every request
identifies itself with the user agent `wam/<version> (WoW addon manager)`.

**Finding WoW**: `WAM_WOW_DIR` if set, else the folder you picked last (`%APPDATA%\wam\settings.json`), else Windows' list of installed programs (Battle.net records WoW's folder there), else `World of Warcraft` in the usual places on each local drive. Any folder inside an install (a flavor folder, `Interface`, `AddOns`) is accepted and resolved to the install's root.

## GUI

`wam.exe ["C:\Program Files (x86)\World of Warcraft"]`

- Header tabs pick the flavor (default Forever, else Retail). The game version comes from the flavor's `Wow*.exe`.
- **Look**: ten themes, picked in Settings > Appearance (`gui/themes.c`): Tinker (the default: midnight blue with gold and bronze trim, cut gems and cogs), Slate (flat, no ornaments), and Void, Frostbound, Inferno, Fel, Bloodrage, Radiance, Wildgrowth and Stormcaller. Each is data (`UI_Theme`: colors, trim metals, the gem, an `ornate` level, a second `spark` color, an `aura` glowing over the top of the window and panels) with an ornament style (`UI_STYLE_*`), which chooses the emblem in dividers and the header, the pieces on panel corners and the spinner. Buttons are flat with a thin rim in the theme's metal, tabs are text with a line under the active one. To add a theme, add its colors and a line to `themes`. The choice is kept as `theme` in `%APPDATA%\wam\settings.json`.
- **Header**: the theme's emblem and wam's name, drawn from strokes in the theme's metal with its gem for the A's crossbar (`gui/wordmark.c`; no font).
- **Status line**: what wam is doing or just did, for 12 to 27 seconds after it's done (errors stay until the next message); otherwise how many addons are installed, how many updates are ready and when wam last checked. Hover that for the AddOns folder. Settings can add frame and startup times (`showPerformance` in `settings.json`).
- **Title bar**: the header is the window's title bar (Windows' own is removed; the frame, shadow, snapping and Windows 11's snap layouts over maximize stay). Its empty parts drag the window, double-click maximizes, and minimize, maximize and close are at its right end. When the window is narrow it drops the name, then the game version, then shrinks the install button to a folder icon.
- **Which install**: the button at the top right shows the World of Warcraft folder in use; click it to switch to another install wam found or browse for one. The choice is remembered. If wam finds none, the window offers the folder picker.
- **Settings**: the cog next to it (or Ctrl+,) opens a popup in the middle of the window: the Wago API key (paste it and press Enter or Save; it's never shown, and stored encrypted for your Windows user; Remove deletes it), which CurseForge key is in use, the World of Warcraft folder (Change...), the theme and whether the status line shows performance numbers, and buttons that open wam's settings, caches and settings-backup folders. Esc, ✕ or a click beside it closes it.
- **Installed**: tracked addons (with Update when a newer file exists) and untracked folders, in rows that look like Get addons': a picture, then the name, the addon's summary (or its `.toc` notes, or its folders), and a faint line with author · version · game version · sources. Pictures and summaries come from the catalog, found under the addon's tracked id or the ids its `.toc` names (`X-Curse-Project-ID`, `X-WoWI-ID`, `X-Wago-ID`); installed CurseForge addons the catalog doesn't have are fetched when it loads (one request) and cached with it; GitHub addons outside it show their owner's GitHub picture. An available update shows as a green "→ new version" after the name. Removed folders go to the Recycle Bin.
- **Source links**: a row's faint line ends in where wam installs the addon from (a little brighter: CurseForge, Wago, WoWInterface or GitHub) and the other sites its `.toc` says it's published on (`X-Curse-Project-ID`, `X-Wago-ID`, `X-WoWI-ID`). Click one to open the addon's page in your browser. The same in Get addons.
- **Checkboxes**: tick rows (or click their names; Ctrl+A for all) to Update or Remove them together.
- **Removing** always asks first, in a popup (`gui/remove.c`): once for one addon or for all the checked ones (the Remove button above the list, or any checked row's), which it lists. It offers to also remove dependencies nothing else installed needs (from the `.toc` `## Dependencies` / `## RequiredDeps`), and warns when addons that stay installed need what you're removing. Enter confirms, Esc cancels. Closing wam waits for a confirmed removal to finish (30 s at most).
- **Instant feedback**: installs, updates and removals run on the worker one after another (a removal moves the folders to the Recycle Bin, about half a second for a big addon; any of them may wait behind a Check for updates), and the list only changes with the worker's next listing. So what you ask for shows at once (`gui/feedback.c`): removed rows disappear when you confirm, and come back with the error if the removal fails; Update buttons turn into "Updating..."; an install adds a row at the top of Installed ("Installing...", or "Waiting for wam's other work to finish first" while it's queued) and turns the addon's Install button in Get addons (and in its detail panel) into a progress bar: "Installing 45%" on the button's shape, filled as far as the install is (finding the file 0-5%, downloading 5-50%, unpacking file by file 50-80%, recording the installed files folder by folder 80-97%; a band sliding across while a download's size isn't known; "Waiting..." while it waits its turn). The worker publishes the running task's id and progress (`work_task`, `work_permille`) and asks for a frame when it moves by half a percent; a dependency's download doesn't move the bar. Recording an install's checksums (the baseline for *edited locally*) uses the MD5s computed while unpacking a zip instead of reading the files back from disk, which made installing ItemTree (44 MB) about twice as fast. Each lasts until the worker reports that task done (queued tasks are numbered, and `on_task_done` gets the number; the listing it made arrives just before). Each queued task also gets a line in `%LOCALAPPDATA%\wam\tasks.log`: when, its number and kind, how long it waited behind the tasks before it and how long it ran, and its addon ids or file name (no keys); kept under 256 KB, the previous one becomes `tasks.old.log`.
- **Edited addons**: if you changed an addon's files since wam installed it, its row says *edited locally* and **Update all** / batch updates leave it alone; its own **Overwrite edits** button replaces your changes.
- **Pinning**: the pin on a tracked row (it shows while the row is hovered, and stays lit once pinned) keeps the addon at its version: Update all and batch updates skip it and say so, and its row shows "pinned · *version* available" instead of an Update button. Updates are still checked, so you see what you're missing. Saved as `"pinned": true` in `wam.json`;
- **What's new**: an update's "→ *version*" is a link to a popup with the releases since the installed one, newest first, each with its changelog, and Update (or Unpin) at the bottom. CurseForge: the project's files for this game (the game's major version; releases only, unless the update is a beta), up to 10, each file's changelog fetched in parallel; up to 4 pages of 50 files are read to get back to the installed one, since addons that also ship Classic builds have many newer files for other games. GitHub: its releases (the API, without drafts and pre-releases) down to the installed tag. WoWInterface, Wago and Tukui keep one changelog per addon, shown down to the first line (after the first) that names the installed version as a whole version. The popup's heading names the newest release found, which may be newer than the last check's.
- **Settings backup**: before installing updates, wam zips the flavor's `WTF` folder (addon settings) to `WTF-Backup\WTF-YYYY-MM-DD.zip`, once a day, keeping the newest 7. If the backup fails, no updates are installed.
- **Check for updates**: recognizes untracked addons by fingerprint, then checks every tracked addon. Runs automatically at startup when a key is set. Fingerprints are cached in `%LOCALAPPDATA%\wam\fingerprints.tsv` with each folder's signature (its files' names, sizes and dates, from the directory listing), so only new or changed folders are read: a 189 MB guide addon WoW hadn't loaded lately took 3.7 s to fingerprint, and 0.4 s with the cache (the whole check).
- **Get addons** browses a catalog of addons for this flavor, so it shows something before you type and every keystroke, sort and filter answers at once (it all runs locally):
  - **Browse**: with an empty field, the addons that install on this game version, sorted by **Most downloaded**, **Updated** (most recently) or **New**. Scrolling near the end fetches the next page from CurseForge.
  - **Search**: typing filters the list as you type, fuzzy-matched (letters in order, e.g. `dbm` finds "DBM - Deadly Boss Mods") with the matched letters highlighted, sorted by **Best match** (match quality, then popularity) or any of the other sorts. Weak matches are left out. A moment after you stop typing (or at once on Enter), CurseForge (and Wago, with your key) are asked too; what they find joins the list. Addons with no file for this game version show at the end, dimmed.
  - **Filters**: the **category** menu (CurseForge's categories, such as Unit Frames, Bags & Inventory or Boss Encounters, each with how many addons it has under the other filters; categories with subcategories, such as Class › Mage, Combat › Healer, Professions › Alchemy or Plugins › Titan Panel, open a submenu that starts with the whole category; WoWInterface's categories are mapped onto them, its class, role and plugin-bar categories onto the subcategories), the **sources** menu (CurseForge, WoWInterface, Wago, Tukui), which turns sources on and off, and **Hide installed**, which hides what this flavor already has.
  - **One row per addon**: an addon on both CurseForge and Wago (Details!, DBM, WeakAuras, BigWigs, Plater...) is one row naming both sources: first, and brighter, where Install gets it from; then the other one (links to their pages). Install takes CurseForge, or Wago (with your key) when the CurseForge author lets only CurseForge's own website serve the files (the addons tagged "via GitHub" or "website download"), or whichever has a file for this game version. The detail panel adds a **From CurseForge** / **From Wago** button for the other source. The sources menu picks too: with CurseForge off, such a row is its Wago entry (and the other way round). The row's downloads add up both sources', and it's in the categories of either. On Installed, an addon the catalog knows on both names both even when its `.toc` names only one.
  - **Pictures**: each row shows the addon's icon (CurseForge's logo, Wago's thumbnail, Tukui's square logo, WoWInterface's first screenshot), or its initial on its source's color until one loads.
  - **Details**: selecting a row (click, or ↑/↓) opens a panel on the right with the addon's picture, downloads, last update, first release year, favorites and latest version; Install; links to its page, source code, issue tracker and donation page when it has them; its summary; screenshots (click one to view it full size in wam: as big as it is, or scaled down to fit the window; ← / → or the side buttons go through the addon's other screenshots, Esc, ✕ or a click beside the picture closes it, and "Open in browser" opens the picture's page); and its description and latest changelog. Details load a moment after the selection rests, and the last 16 stay loaded, so going back is instant. ✕ or Esc closes the panel. Drag the line between the list and the panel to resize it; it keeps that share of the window when the window is resized, and it's remembered (`detailShare` in `%APPDATA%\wam\settings.json`).
  - **Keyboard**: ↓ / ↑ move the selection (↑ from the first row goes back to the field), Page Up / Page Down jump, Ctrl+Home / Ctrl+End go to the ends, Enter installs the selected addon, Esc clears the selection, then the search. A click on a row selects it.
  - **Stable list**: when results arrive and the list re-sorts, the selected row (or the one under the mouse, or the top one when scrolled) stays where it was on screen, so nothing jumps under the cursor.
  - **Links**: paste a GitHub / WoWInterface / CurseForge / Wago link (Ctrl+V) and press Enter to install it. Required CurseForge dependencies are installed too.
- **Drop a .zip, .rar or .7z** anywhere on the window to install it (for addons you downloaded yourself).
- **Downloads**: lists addon archives found in your download folder(s), newest first, with their version, the game version they're built for (red when it's another flavor's), and whether they're new, a different version from what's installed, or installed. Install with one click; Show opens the file in Explorer. wam watches the download folder(s), so a finished download shows up within about a second, even while wam is in the background (it waits until the folder has been quiet for 0.4 s, so a download in progress doesn't trigger a scan for every chunk); it also rescans when wam starts, when you switch back to its window, and after installs. Archives that were never installed anywhere (any flavor or install, through wam or found installed) are marked **NEW**, and the Downloads tab shows a green "N new" badge until they are; installed archives are remembered by file name and size in `%APPDATA%\wam\downloads.json`.
- **Scrollbars**: every list (and the detail panel) scrolls with the wheel, by dragging its scrollbar thumb, or by pressing beside the thumb, which jumps there (keep holding to drag on from there). The scrollbar widens while the mouse is over it.
- F5 rescans, Ctrl+F opens search, Ctrl+, opens settings, Esc clears the selection / search.

## How it works

- **State**: `<flavor>\Interface\wam.json` maps addon id (`cf:…`, `gh:…`, `wowi:…`) → installed file → folders. Saved atomically; a file that can't be parsed is never overwritten. Folders deleted by hand are pruned automatically.
- **Game version**: read from the FileVersion string of the flavor's `Wow*.exe` (e.g. Forever 1.60.1).
- **Long paths**: file operations use `\\?\` paths, so deep `Libs\` trees past 260 characters work.
- **Choosing a file**: from the mod's `latestFilesIndexes`, same major game version as the client is required (1.x Era/Forever, 2.x Anniversary, 5.x Classic, 12.x Retail; Retail falls back to an older retail major, never to a Classic file). Then release > beta > alpha, then same minor, then newest.
- **Installing**: download → extract to `AddOns\.wam-staging-*` → move old folders aside → move new ones in → delete old. Any failure rolls back. Folder moves retry briefly because antivirus scanners hold new files open. Archive paths with `..`, drive letters or absolute paths are rejected.
- **Download folders**: the Windows Downloads known folder, plus custom download folders set in Chrome, Edge, Brave, Vivaldi, Opera (Preferences → `download.default_directory`) or Firefox (`prefs.js` → `browser.download.dir`). Only these local settings files are read. An archive counts as an addon if it has `Folder/Folder.toc` entries; zips are inspected by reading only their central directory and `.toc` entries (about 1 ms, even for huge unrelated zips), RAR/7z via `tar.exe`, cached per file.
- **RAR / 7z**: detected by content (downloads too; some old WoWInterface uploads are .rar) and extracted with `%SystemRoot%\System32\tar.exe` (bsdtar/libarchive, Windows 10 1803+). Entry names are validated first; archives with symbolic links, password protection or extraction errors are refused, and the extracted tree is checked for links before anything moves into AddOns.
- **Resizing**: each new window size is drawn immediately (in `WM_SIZE`, not on a later repaint), and the swapchain uses the bitblt ("discard") model rather than flip. While an edge is dragged, the compositor holds each new size back until the app presents a frame of that size; with flip-model swapchains that handshake cost about two refreshes per step in real drags (66–82 fps at 144 Hz, with content trailing the edge), with the bitblt model the window follows at the full refresh rate (138–139 fps). `WAM_TEST_FRAME_LOG=<file>` logs every frame's resize, draw and present times; the status line shows the frames drawn in the last second when Settings' performance numbers are on.
- **Catalog**: Get addons' list, per flavor, about 9,700 addons fetched in parallel (up to 24 requests at a time: they mostly wait on the network) in about half a second; with a Wago key, Wago's lists take about two seconds more, so they're fetched beside the rest, which goes to the list first (Wago's addons join it when they arrive; if Wago fails, last time's Wago entries are kept from the saved catalog): CurseForge's 1,000 most downloaded, 300 most recently updated and 200 newest (paged searches), WoWInterface's whole file list (its `filelist.json`; WoWInterface has no search API, so this is how its addons are found) and Tukui's list. Each entry records whether it has a file for this game version (CurseForge: the file picking above; WoWInterface: the game versions it lists as tested, the same major, or on retail the expansion before; Tukui: its patch list). With a Wago key, Wago's featured addons and each of its 29 categories' most popular (about 350; Wago has no browse API, so that's one search per category, 30 requests of the 60 a minute its API allows) join the catalog, with their pictures and categories (named like CurseForge's); other Wago addons join through searches. **Pairs**: Wago's lists name no CurseForge id and CurseForge's no Wago id, so wam pairs a Wago entry with a CurseForge entry that has the same name (ignoring case and punctuation: "Plater-Nameplates" = "Plater Nameplates"), whoever the authors are (people often use another user name on each site), or a shared author (ignoring case and punctuation, and a trailing suffix: "Sapu" = "Sapu94") and a name whose words are all in the other's ("OmniCD" / "OmniCD - Party Cooldown Tracker"; filler words like "core", "requires" and "the" don't count). Not when the longer name continues the shorter with `:` or `_` ("Masque: Caith", "BigWigs_Classic"), or adds a word like skin, sound, plugin or module; and only when each is the other's single best match (so "Vocal Raid Assistant" isn't paired with any of its sound packs). Ids an installed addon's `.toc` names (`X-Curse-Project-ID` and `X-Wago-ID`) are paired first, whatever the names. Of about 350 Wago addons, about 210 pair up; pairing the catalog takes a few milliseconds and is redone when it changes. A cached catalog made without Wago's lists is refetched once a key is added. The catalog is cached in `%LOCALAPPDATA%\wam\catalog\<flavor>.tsv` (`WAM_CATALOG_DIR` overrides the folder), shown at once on the next start, and refetched in the background when it's older than 12 hours or the game version changed. Every search and page wam fetches adds to it. Each entry also keeps its categories and subcategories, icon and screenshot links.
- **Details**: fetched when a row is selected. CurseForge: the mod (screenshots, links) and its description in parallel, then the changelog of the file wam would install. WoWInterface: its file details (description, changelog, favorites). Tukui: its list (description, links, gallery) and the changelog. Wago: the addon (`/addons/<id>`: description, gallery, source link) with its latest stable release's changelog. HTML (CurseForge), BBCode (WoWInterface) and Markdown (Tukui, Wago) become plain paragraphs and bullet lines; pictures inside them (including linked ones, like donate buttons) are left out. Details that failed (a rate limit, a network hiccup) are asked again when the addon is selected again a few seconds later.
- **Pictures**: fetched on 4 background threads, newest request first, skipping ones that scrolled off screen; decoded and scaled with Windows' WIC (icons 64×64, thumbnails 320×180, cropped to fill) into a 2048×2048 texture beside the glyph atlas, with the least recently drawn ones giving up their place. The downloaded files are cached in `%LOCALAPPDATA%\wam\images` (files older than 30 days are deleted at startup and fetched again when needed).
- **Testing hooks** (environment variables): `WAM_TEST_FRAME_LOG=<file>` logs frame times; `WAM_TEST_SHOT_DIR=<folder>` makes F12 save the frame wam renders as `shot-N.bmp` there (works while the screen is locked, when window captures come out blank); `WAM_TEST_OPEN_LOG=<file>` writes links to that file instead of opening them; `WAM_TEST_IMAGE_LOG=<file>` appends every picture that fails to load, with the reason; `WAM_TEST_NO_WEBVIEW=1` uses the browser instead of the download window; `WAM_TEST_DOWNLOADS_DIR` and `WAM_CATALOG_DIR` point the Downloads list and the catalog cache elsewhere.
- **Text**: DirectWrite with grayscale antialiasing (natural, symmetric rendering), fractional advances and glyphs cached at 4 sub-pixel positions, so stroke weights and spacing are even. Characters Segoe UI lacks (emoji, Chinese, Korean, symbols) come from Windows' font fallback, in one color. Glyphs are rasterized on first use into a 2048×2048 atlas; if it fills up, it's emptied and refilled on the next frame. Measuring text (for wrapping and truncating) uses cached advances and rasterizes nothing.
- **Speed**: listing a flavor reads only each folder's `.toc` (69 addons in about 20 ms); the file fingerprints used to recognize addons are computed only when syncing, in parallel. Listings run on their own thread, so switching flavors never waits behind a sync, install or update, and each flavor's last listing is kept so switching back shows it at once while a fresh one loads.
- **Local edits**: at install (or when first recognized) wam stores two MD5s per folder in `wam.json`: one over every file's path, size and modification time (cheap), and one over every file's path and contents. A folder whose cheap one still matches is unchanged; otherwise its contents are hashed again, and a different result means files were edited, added or deleted.
- **Wago fingerprints**: the same files as CurseForge's fingerprint, but MD5 of each file's raw bytes; the lowercase hex digests are sorted, concatenated and MD5'd again (WowUp's algorithm). Sent to `/addons/_match` with `## X-Wago-ID` when present. A Wago addon's file id is its release time, so it grows with each release.
- **Distribution**: files with no `downloadUrl` (author disabled third-party downloads) are never fetched; wam points to the website.
- **Removing**: folders go to the Recycle Bin. Junctions/symlinks are unlinked, never followed.

## Layout

| Path | What |
|---|---|
| `src/base/` | types, arenas, strings, paths, JSON, numbers and ages for people to read (`12,345`, `1.2M`, `5 min ago`) |
| `src/os/` | Win32 layer: memory, files, dirs, move/delete/recycle, threads, parallel-for, locks, semaphores and events, time, folder watching, the clipboard, shell open, picture decoding (WIC) |
| `src/core/fingerprint.c` | MurmurHash2 CurseForge fingerprint |
| `src/core/addons.c` | WoW detection, client version, `.toc` parsing, folder fingerprint, scanning |
| `src/core/zip.c` | zip central directory, DEFLATE decoder, CRC32 |
| `src/core/curseforge.c` | WinHTTP client (progress, redirects), CurseForge API calls |
| `src/core/wago.c` | Wago API calls, key storage (DPAPI) |
| `src/core/manager.c` | state file, file picking, install / update / remove / sync / search |
| `src/core/catalog.c` | the catalog Get addons browses: hashed entries, CurseForge / Wago pairs, categories, parallel fetch, disk cache, an addon's details |
| `src/core/markup.c` | descriptions and changelogs (HTML, BBCode, Markdown) as plain paragraphs and bullets |
| `src/render/render_d3d11.c` | instanced SDF renderer: rounded rects, cog wheels, diamonds, stars, crescents, leaves, tapered strokes and soft glows, solid or with a gradient; clip batches |
| `src/font/font_dwrite.c`, `font_dwrite.cpp` | text with DirectWrite: glyphs rasterized on demand into an R8 atlas (natural symmetric antialiasing, fractional advances, 4 sub-pixel positions, Windows font fallback for emoji / CJK / symbols). DirectWrite is C++-only, so `font_dwrite.cpp` is the one C++ file (a small C API, compiled on its own, no exceptions or RTTI) |
| `src/font/font_gdi.c` | the older GDI backend, same interface (compile with `/DWAM_FONT_GDI`) |
| `src/ui/` | reusable immediate-mode UI library, independent of wam: `ui.h` (API), `ui.c` (themes, fonts, hit testing, widget identity (`UI_Id`: a label hashed within a scope, with `##` / `###` like Dear ImGui and the RAD Debugger), `ui_signal` (hover, press, the active widget that keeps a drag, click), keyboard focus (Tab / Shift+Tab, Enter or Space clicks, a ring when it came from the keyboard, kept inside a popup while one is up), `ui_anim` (per-widget easing, e.g. hover fades), ornaments that follow the theme and its style: flat plates with a metal rim, framed panels with corner pieces, dividers, emblems, auras, gems, cogs, spinners, rivets; widgets: button, small button, glass button, checkbox, pill, tabs (with a badge) and title-bar tabs, text field, link text, busy label (spinner and text), scrolling list with a draggable scrollbar and a virtual list that draws only the visible rows, popups over a dimmed backdrop with a close button, highlighted text, word-wrapped text, drag handle with a resize pointer, overlay layer for dropdowns, loading bar), `ui_image.c` (pictures from URLs: background fetch through an app callback, or `res:NAME` from the .exe's resources; an image atlas with LRU slots; an avatar: a picture, or a name's initial until it loads). No platform code: it uses `wm/` and `os/` |
| `src/wm/` | the window layer: `wm.h` (API, no Win32 types), `wm_win32.c` (the window, DPI, the app-drawn title bar, mouse state and key/text events, frame scheduling with a re-entrancy guard, `wm_post` from other threads, timers, deferred work, popup menus with submenus, mouse pointer, folder picker), optional `wm_webview.c` (a visible WebView2 window that shows a page and hands back the file it downloads) |
| `src/gui/` | wam's GUI on the library (no Win32: it uses `wm/` and `os/`): `state.c` (snapshots, tasks, app state), `tasks.c` (worker and lane threads, building snapshots), `views.c` (Installed, Get addons with its catalog list, Downloads), `themes.c` (the themes), `settings.c` (the settings popup), `remove.c` (the removal popup), `wordmark.c` (the name in the header), `frame.c` (header, toolbar, status line, switching flavors/installs) |
| `res/`, `tools/make_icon.c` | the icon, and the program that draws it and the README's logo |
| `src/main_gui.c`, `src/main_test.c` | unity-build entry points (`main_gui.c`: keyboard, worker results, startup) |

**Using the UI library elsewhere**: include `base/`, `os/`, `render/render_d3d11.c`, `font/font_dwrite.c` (link `font_dwrite.obj` and `dwrite.lib`; or `font/font_gdi.c`), then
`wm/wm.h`, `wm/wm_win32.c`, `ui/ui.h`, `ui/ui.c`; fill a `WM_App` (class name, title, sizes, a `frame`
callback, optional `file_dropped` / `activated` callbacks), call `wm_window_open`, then `ui_init`, then
`wm_window_run`. Make `frame` return `ui_frame(f, build)`; in `build`, lay out with `rect()` and call
widgets, which read `ui.in` (the mouse) and `ui.events` (keys and text) and draw with `ui.theme` / `ui.font`.
Give repeated parts their own id scope (`ui_scope_push`, or the key you pass `ui_list_row`) so their widgets don't share an identity. Other threads hand results to the UI thread with `wm_post`. Pick a look by setting `ui.theme` (`ui_theme_dark()`, `ui_theme_tinker()` or your own `UI_Theme`); `WM_App.icon` names the window's icon resource.

## Known gaps

- GitHub, WoWInterface and CurseForge are tested live (fingerprints matched 22/22 real addons exactly).
- Wago is tested live with a subscriber key: browsing, search, details and installs. Update checks and fingerprint matching follow WowUp's open-source Wago provider and have had less use. An addon on both Wago and CurseForge is paired by name and author (see Catalog), which can miss a pair whose names differ a lot (it then shows as two rows) and, rarely, pair two addons that only look alike.
- WoWInterface has no search API; wam searches its whole file list locally instead. Its list has no summaries.
- Tukui reports no download counts, so ElvUI sorts last under Most downloaded (it's first under Best match when you search for it).
- Descriptions are plain text: headings, links and images inside them aren't drawn as such (links keep their text). Emoji and other characters Segoe UI lacks show as boxes or question marks.
- The renderer compiles its shaders at startup (about 30 ms); compiling them when building, with `fxc`, would save that.
