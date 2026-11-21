# fetch_webview2.ps1 <out_dir>
# Puts Microsoft's WebView2 SDK header and static loader (x64) in <out_dir>, once. Pinned version,
# checked against its SHA-256, from nuget.org. Only these three files are kept:
#   WebView2.h, WebView2LoaderStatic.lib, LICENSE.txt
# The static loader is linked into wam.exe, so nothing extra ships next to it; the WebView2 runtime
# itself is part of Windows 10/11 (Edge).
param([string]$OutDir)
$ErrorActionPreference = 'Stop'
$version = '1.0.4191.47'
$sha256  = 'f492bbf547d0da329553b6727435b677579b1e9f91cc9e4a1ad029366d5f23d0'
$need = @('WebView2.h', 'WebView2LoaderStatic.lib', 'LICENSE.txt')
if (($need | Where-Object { -not (Test-Path (Join-Path $OutDir $_)) }).Count -eq 0) { exit 0 }

New-Item -ItemType Directory -Force $OutDir | Out-Null
$tmp = Join-Path ([IO.Path]::GetTempPath()) ("wam-webview2-" + [Guid]::NewGuid())
New-Item -ItemType Directory $tmp | Out-Null
try {
    $pkg = Join-Path $tmp 'webview2.zip'
    $url = "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/$version/microsoft.web.webview2.$version.nupkg"
    Write-Host "fetching WebView2 SDK $version"
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $pkg
    $got = (Get-FileHash -Algorithm SHA256 $pkg).Hash.ToLower()
    if ($got -ne $sha256) { throw "WebView2 SDK checksum mismatch: $got" }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($pkg)
    try {
        $map = @{ 'build/native/include/WebView2.h' = 'WebView2.h'; 'build/native/x64/WebView2LoaderStatic.lib' = 'WebView2LoaderStatic.lib'; 'LICENSE.txt' = 'LICENSE.txt' }
        foreach ($e in $zip.Entries) {
            if ($map.ContainsKey($e.FullName)) {
                [IO.Compression.ZipFileExtensions]::ExtractToFile($e, (Join-Path $OutDir $map[$e.FullName]), $true)
            }
        }
    } finally { $zip.Dispose() }
    foreach ($f in $need) { if (-not (Test-Path (Join-Path $OutDir $f))) { throw "WebView2 SDK: $f missing from the package" } }
} finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
