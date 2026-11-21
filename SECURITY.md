# Security

wam downloads archives from the internet and unpacks them into your World of Warcraft folder, and it
keeps API keys. If you find a way to make it write outside the AddOns folder, run something, leak a
key, or install something other than what was asked for, please report it privately rather than in a
public issue.

**How to report**: use GitHub's private vulnerability reporting on this repository (the
**Security** tab, **Report a vulnerability**). Say what you found, how to reproduce it, and which wam
version you used. You'll get an answer within a week; once a fix is released, the report can be made
public, with credit if you want it.

What wam already does, so you know what's intended:

- Archive entries with `..`, drive letters or absolute paths are refused; RAR and 7z archives with
  links, passwords or extraction errors are refused, and nothing is moved into AddOns until
  everything unpacked cleanly.
- Downloads and API calls use HTTPS only; links wam opens in your browser must be `https://`.
- The Wago key is stored encrypted for your Windows user (DPAPI); the CurseForge key built into
  releases is obscured, not secret, and CurseForge can revoke it.
- Removed addons go to the Recycle Bin; links (junctions) inside AddOns are removed, never followed.

Only the latest release gets security fixes.
