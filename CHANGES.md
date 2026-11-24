# Changes

## 1.5.0

- **CurseForge now needs your own API key**: wam no longer comes with one. Get yours at
  console.curseforge.com and paste it in Settings; it's tried before it's saved, and kept encrypted for your
  Windows user. Without it, your CurseForge addons stay installed but aren't checked for updates, and
  Get addons doesn't list CurseForge. The other sources work as before.
- **CurseForge downloads** send the API key, which CurseForge has announced it will require.
- **Settings scrolls** when the window is too low to show it all.
- **Support an addon's author**: a Support button near the top of the details when the addon's page links
  to Patreon, PayPal, Ko-fi, GitHub Sponsors or the like, and its support links further down. CurseForge
  addons link to their page too, which may have a Donate button.
- **Links that did nothing**: details links listed as `http://` (such as Donate on ElvUI and Tukui) open
  again, over https.
- **The details stay still** when an addon's version and favorites arrive a moment after the rest.

## 1.4.0

- **Descriptions and changelogs look right**: headings, bold and italic, lists, quotes, code, tables and
  clickable links, in the details and in What's new. A link shows where it goes when you point at it.
- **A screenshot gallery**: one large, uncropped picture with the others below it, up to 30 per addon
  instead of 6. Click the picture for full size.
- **Pictures in descriptions** show in place when they're on an addon site; others load when you click them.
- **GIFs play** in the gallery's large picture and at full size. They pause while wam isn't the active
  window, and stay still when Windows' animation effects are off.
- **Details come up sooner**: at once when you click an addon or step with the arrow keys, and the
  picture you're looking at loads before the others.
- **Faster start**: about 80 ms sooner, measured on one PC.
- **Searching within a category** finds more: wam asks CurseForge for that category. A search with few
  results no longer keeps loading forever; a link at the end looks further when you want it to.
- **Safer installs**: if an update fails and the old version can't be put back, wam keeps it and says where
  it is, instead of deleting it.
- **Damaged archives** are refused more carefully: a zip with a false file size could crash wam.
- **Settings backups**: wam only ever deletes the backups it made itself, never other zips you keep next
  to them.

## 1.3.0

- **Themes**: pick one of ten in Settings. Eight are new and drawn from WoW's magic and melee: Void,
  Frostbound, Inferno, Fel, Bloodrage, Radiance, Wildgrowth and Stormcaller, each with its own
  colors, emblem, corner pieces and spinner. Tinker is still the default, and Slate is plain.
- **A calmer window**: an addon's row has its name, what it does, and one faint line of facts ending
  in its sources (links to their pages). Tabs are plain text with a line under the active one, and
  buttons are flat with a thin rim.
- **Simpler filters**: Get addons has three menus, Sort, Category and Sources, instead of a row of
  buttons.
- **A status line in plain language**: how many addons are installed, how many updates are ready and
  when wam last checked. Frame and startup times are off unless you turn them on in Settings.
- **A new name and icon**: a drawn wordmark next to the theme's emblem, and a matching app icon.
- **Removing** asks in a popup, once for however many addons you've ticked, instead of in a bar above the
  list.
- **Keyboard**: Tab and Shift+Tab move between buttons, tabs and checkboxes, and Enter or Space clicks.

## 1.2.0

- **wam is open source** now, under the GPL-3.0 license: see the README for building it yourself.
- **Instant feedback**: removing addons takes them off the list the moment you confirm, updates
  show "Updating...", and an install shows its progress on the addon's Install button and as a row in
  Installed, even while wam is still busy with something else.
- **Faster installs**: installing a big addon takes about half as long.
- **Faster Check for updates**: addons that haven't changed aren't read again, so a check on a big
  install takes well under a second.
- **Get addons appears sooner**: its list shows as soon as CurseForge, WoWInterface and Tukui have
  answered; Wago's addons join when Wago does. If Wago is busy, last time's Wago addons stay.
- `wamcli.exe` is no longer included: wam is the app.
- `%LOCALAPPDATA%\wam\tasks.log` records how long each install, update and removal took, to help
  when something feels slow.

## 1.1.0

- **What's new**: click an update's new version to read the release notes of every version since
  yours, then update from there.
- **Pin** an addon to keep it at its version: Update all leaves it alone until you unpin it, and
  you still see what's available. `wamcli pin` / `unpin` do the same.
- **Settings**: the cog at the top opens them (or Ctrl+,): your Wago key, which CurseForge key is in
  use, the WoW folder, wam's folders and backups, and the version.
- **A cleaner window**: the header is now the title bar, with its own minimize, maximize and close
  buttons; dragging, snapping and Windows 11's snap layouts work as before.
- **Wago**: browse Wago's addons by category, with their pictures, descriptions, screenshots and
  changelogs.
- **One row per addon**: an addon on both CurseForge and Wago shows once, with both sources.
- Better changelogs: Markdown that authors paste into CurseForge now reads as text.
- When CurseForge or Wago says wam is asking too often, wam waits as long as they ask before trying
  again, and says so.
- `wamcli changes <addon>` prints what an update would bring; `wamcli version` prints the version.
- The exes now carry their version (Properties > Details).

## 1.0.0

The first public release.

- Install, update and remove addons from CurseForge, Wago, WoWInterface, GitHub and Tukui.
- Finds every WoW install and flavor; recognizes addons installed by hand or by other managers.
- Get addons: browse and search the catalog for your game version, with categories, screenshots,
  descriptions and changelogs.
- Update all, with a daily backup of your addon settings first; failed installs roll back.
- Downloads tab for archives you downloaded yourself; drop a .zip, .rar or .7z on the window.
- `wamcli.exe` for the command line.
