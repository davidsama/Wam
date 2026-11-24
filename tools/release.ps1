# Builds, checks and publishes a wam release on GitHub: wam.exe (with its hash and notices), with the version's
# CHANGES.md section as the notes.
#
#   powershell -ExecutionPolicy Bypass -File tools\release.ps1 -DryRun
#   powershell -ExecutionPolicy Bypass -File tools\release.ps1
#
# The version is src\version.h's (WAM_VERSION); -Version 1.2.3, if given, must match it. The release
# and its tag are named after it (1.2.3, no "v"), and CHANGES.md needs a "## 1.2.3" section. To
# release a new version: set it in version.h, write its CHANGES section, commit both on main and push,
# then run this.
# -Repo    the GitHub repository, owner/name (default: the origin remote's; or set WAM_REPO).
# -DryRun  build, check and stage everything in build\release\<version>, publish nothing.
#
# Needs the GitHub CLI (winget install GitHub.cli, then gh auth login) for anything but -DryRun.
# Uploaded: wam.exe, SHA256SUMS.txt and THIRD_PARTY_NOTICES.md; never .pdb files, which describe the code.
param(
    [string]$Version,
    [string]$Repo = $env:WAM_REPO,
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

function Fail([string]$msg) { Write-Host "error: $msg" -ForegroundColor Red; exit 1 }
function Step([string]$msg) { Write-Host "== $msg" -ForegroundColor Cyan }
function RunGit { $out = & git.exe @args 2>&1; if ($LASTEXITCODE -ne 0) { Fail "git $($args -join ' '): $out" }; $out }

if (-not $Repo) {
    # owner/name from the origin remote (https://github.com/owner/name(.git) or git@github.com:owner/name.git)
    $url = (& git.exe remote get-url origin 2>$null)
    if ($url -match 'github\.com[:/]+([\w.-]+/[\w.-]+?)(\.git)?/?$') { $Repo = $Matches[1] }
}
if (-not $DryRun) {
    if (-not $Repo) { Fail 'no GitHub repository: give -Repo owner/name (or set WAM_REPO)' }
    if ($Repo -notmatch '^[\w.-]+/[\w.-]+$') { Fail "-Repo should look like owner/name, not '$Repo'" }
    if (-not (Get-Command gh -ErrorAction SilentlyContinue)) { Fail 'the GitHub CLI is missing: winget install GitHub.cli, then gh auth login' }
}

# --- checks before building -------------------------------------------------------------------------
# The version: src\version.h's, whose string and numbers must agree; -Version only confirms it.
$vh = Get-Content -Raw (Join-Path $root 'src\version.h')
$vs = [regex]::Match($vh, '#define\s+WAM_VERSION\s+"(\d+\.\d+\.\d+)"').Groups[1].Value
$vn = foreach ($part in 'MAJOR', 'MINOR', 'PATCH') { [regex]::Match($vh, "#define\s+WAM_VERSION_$part\s+(\d+)").Groups[1].Value }
if (-not $vs -or ($vn -join '.') -ne $vs) { Fail "src\version.h: WAM_VERSION ('$vs') and its numbers ($($vn -join '.')) don't agree" }
if ($Version -and $Version -ne $vs) { Fail "src\version.h says $vs, not ${Version}: set WAM_VERSION there (and commit it) first" }
$Version = $vs
$tag = $Version
Step "wam $Version"

$dirty = & git status --porcelain
$branch = (RunGit rev-parse --abbrev-ref HEAD).Trim()
$commit = (RunGit rev-parse HEAD).Trim()
# The release is made from this commit on GitHub, so GitHub must have it: main, pushed.
& git.exe fetch -q origin 2>$null
$pushed = (& git.exe branch -r --contains $commit 2>$null) -match '^\s*origin/main$'
$why = if ($dirty) { 'there are uncommitted changes' } elseif ($branch -ne 'main') { "the branch is $branch, not main" } elseif (-not $pushed) { 'this commit isn''t pushed to origin/main' } else { '' }
if ($why) {
    if ($DryRun) { Write-Host "warning: $why (fine for a dry run, not for a release)" -ForegroundColor Yellow }
    else { Fail "$why; a release is built from a clean, pushed main" }
}
& git rev-parse -q --verify "refs/tags/$tag" *> $null
if ($LASTEXITCODE -eq 0) { Fail "tag $tag already exists" }

$changes = Get-Content -Raw (Join-Path $root 'CHANGES.md')
$m = [regex]::Match($changes, "(?ms)^## $([regex]::Escape($Version))\b[^\r\n]*\r?\n(.*?)(?=^## |\z)")
if (-not $m.Success -or -not $m.Groups[1].Value.Trim()) { Fail "CHANGES.md has no '## $Version' section with notes" }
$notes = $m.Groups[1].Value.Trim()

# --- build -------------------------------------------------------------------------------------------
foreach ($mode in 'test', 'release') {
    Step "build.bat $mode"
    $bat = Join-Path $root 'build.bat'
    $vsenv = Join-Path $root 'tools\vsenv.bat' # sets up the compiler in that cmd (build.bat doesn't)
    & cmd /c "cd /d `"$root`" && call `"$vsenv`" --here && `"$bat`" $mode"
    if ($LASTEXITCODE -ne 0) { Fail "build.bat $mode failed" }
}

# --- stage and check what ships ----------------------------------------------------------------------
$stage = Join-Path $root "build\release\$tag"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory $stage | Out-Null
$exes = @('wam.exe')
foreach ($e in $exes) { Copy-Item (Join-Path $root "build\$e") (Join-Path $stage $e) }
# Shipped beside it: the WebView2 loader's license asks binary releases to carry its notice.
Copy-Item (Join-Path $root 'THIRD_PARTY_NOTICES.md') (Join-Path $stage 'THIRD_PARTY_NOTICES.md')

Step 'checking the files'
if (Get-ChildItem $stage -Filter *.pdb) { Fail 'a .pdb ended up in the release' }
$latin1 = [Text.Encoding]::GetEncoding(28591)   # bytes as chars, one to one, for searching binaries
$forbidden = @(@{ what = 'the build folder path'; text = $root })
# A release has no CurseForge key: not the one in cf_key.txt (a leftover, or kept for testing), plain or
# XORed the way wam up to 1.4.0 carried it.
$keyFile = Join-Path $root 'cf_key.txt'
$key = if (Test-Path $keyFile) { "$(Get-Content -Raw $keyFile)".Trim() } else { '' }
if ($key) {
    $i = 0
    $xored = [byte[]]@(foreach ($b in [Text.Encoding]::UTF8.GetBytes($key)) { $b -bxor ((0x5A + $i * 7) -band 0xFF); $i++ })
    $forbidden += @{ what = 'the CurseForge key'; text = $key }
    $forbidden += @{ what = 'the CurseForge key (UTF-16)'; text = $latin1.GetString([Text.Encoding]::Unicode.GetBytes($key)) }
    $forbidden += @{ what = 'the CurseForge key (XORed)'; text = $latin1.GetString($xored) }
}
# This PC's own git name (the global one: a repository may use a public name, which wam.exe names
# on purpose as its author), so a real name never ships inside the exe.
$gitName = (& git.exe config --global --get user.name)
$publicName = (& git.exe config --get user.name)
if ($gitName -and $gitName -ne $publicName) {
    # As a whole word: a first name alone ("David") is part of plenty of others ("davidsama").
    $forbidden += @{ what = "this PC's git user name"; text = $gitName; word = 1 }
    $forbidden += @{ what = "this PC's git user name (UTF-16)"; text = $latin1.GetString([Text.Encoding]::Unicode.GetBytes($gitName)); word = 1 }
}
foreach ($e in $exes) {
    $bin = $latin1.GetString([IO.File]::ReadAllBytes((Join-Path $stage $e)))
    foreach ($f in $forbidden) {
        $found = if ($f.word) { [regex]::IsMatch($bin, '(?<![A-Za-z])' + [regex]::Escape($f.text) + '(?![A-Za-z])', 'IgnoreCase') }
                 else { $bin.IndexOf($f.text, [StringComparison]::OrdinalIgnoreCase) -ge 0 }
        if ($found) { Fail "$e contains $($f.what)" }
    }
}
foreach ($e in $exes) {
    $pv = (Get-Item (Join-Path $stage $e)).VersionInfo.ProductVersion
    if ($pv -ne $Version) { Fail "$e says it's version '$pv', not $Version (its version resource)" }
}
Write-Host "no .pdb, no build paths, no CurseForge key, no git user name; wam.exe is $Version"

$sums = foreach ($e in $exes) { '{0}  {1}' -f (Get-FileHash -Algorithm SHA256 (Join-Path $stage $e)).Hash.ToLower(), $e }
Set-Content -Path (Join-Path $stage 'SHA256SUMS.txt') -Value $sums -Encoding ascii
$fence = '```'
$body = $notes + "`n`n### SHA-256`n`n$fence`n" + ($sums -join "`n") + "`n$fence`n"
$notesFile = Join-Path $root "build\release\$tag-notes.md"
[IO.File]::WriteAllText($notesFile, $body, (New-Object Text.UTF8Encoding $false))

Get-ChildItem $stage | ForEach-Object { '{0,-16} {1,10:N0} bytes' -f $_.Name, $_.Length }
Write-Host ''
Write-Host $body

if ($DryRun) { Write-Host "dry run: staged in $stage, nothing published" -ForegroundColor Green; exit 0 }

# --- publish -----------------------------------------------------------------------------------------
Step "publishing $tag to $Repo"
$files = (Get-ChildItem $stage).FullName
& gh release create $tag @files --repo $Repo --target $commit --title "wam $Version" --notes-file $notesFile
if ($LASTEXITCODE -ne 0) { Fail 'gh release create failed' }
& git.exe fetch -q --tags origin 2>$null # the tag GitHub made on that commit
Write-Host "released: https://github.com/$Repo/releases/tag/$tag" -ForegroundColor Green
