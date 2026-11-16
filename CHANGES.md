# Changes

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
