# SPDX-License-Identifier: MIT
<#
.SYNOPSIS
  Packages a Release build into dist/applewin-harness-<version>-win64.zip (spec: docs/specs/m6.md, "Release bundle").
.DESCRIPTION
  Builds nothing itself. Build first:
    msbuild AppleWinHarness.sln -p:Configuration=Release -p:Platform=x64
  Runs on Windows PowerShell 5.1 and pwsh.
.PARAMETER Configuration
  Build configuration to package (default Release).
.PARAMETER OutDir
  Output directory, relative to the repository root unless absolute (default dist).
#>
[CmdletBinding()]
param(
    [string]$Configuration = 'Release',
    [string]$OutDir = 'dist'
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$utf8 = New-Object System.Text.UTF8Encoding($false)

function Read-Text([string]$path) { [System.IO.File]::ReadAllText($path, $utf8) }
function Write-Text([string]$path, [string]$text) { [System.IO.File]::WriteAllText($path, $text, $utf8) }

function Invoke-Git {
    # Returns the trimmed stdout, or $null when git fails. (Windows PowerShell 5.1 turns native stderr into a
    # terminating error under 'Stop', so relax it here.)
    $ErrorActionPreference = 'Continue'
    $out = & git -C $repo @args 2>$null
    if ($LASTEXITCODE -ne 0) { return $null }
    return (($out | Out-String).Trim())
}

# ---- Inputs ----
$exe = Join-Path $repo "bin\x64\$Configuration\AppleWinHarness.exe"
if (-not (Test-Path -LiteralPath $exe)) {
    throw "Missing $exe. Build first: msbuild AppleWinHarness.sln -p:Configuration=$Configuration -p:Platform=x64"
}
$version = (Read-Text (Join-Path $repo 'VERSION')).Trim()
if ($version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+([-+][0-9A-Za-z.-]+)?$') { throw "VERSION does not hold a version: '$version'" }

# The exe must have been built from the current VERSION (it prints the version it was built with).
$exeVersion = $null
$exeExit = -1
try {
    $exeVersion = (& $exe --version | Out-String).Trim()
    $exeExit = $LASTEXITCODE
} catch { $exeVersion = $null }
if ($exeExit -ne 0 -or $exeVersion -ne $version) {
    throw "$exe reports version '$exeVersion' (exit $exeExit) but VERSION is '$version'. Rebuild it from the current tree."
}

$commit = Invoke-Git rev-parse HEAD
if (-not $commit -or $commit -notmatch '^[0-9a-f]{40}$') { throw 'Cannot read the HEAD commit (is this a git checkout?).' }
$tag = Invoke-Git describe --exact-match --tags HEAD
$release = $commit
if ($tag) { $release = "$commit (tag $tag)" }

$dirty = Invoke-Git status --porcelain
if ($dirty) { Write-Warning 'The working tree is dirty: the bundle may not match the release commit recorded in NOTICE.txt.' }

# ---- Stage ----
$name = "applewin-harness-$version-win64"
if ([System.IO.Path]::IsPathRooted($OutDir)) { $outRoot = $OutDir } else { $outRoot = Join-Path $repo $OutDir }
New-Item -ItemType Directory -Force -Path $outRoot | Out-Null
$outRoot = (Resolve-Path $outRoot).Path
$zipPath = Join-Path $outRoot "$name.zip"
$stage = Join-Path ([System.IO.Path]::GetTempPath()) ("awh-release-" + [guid]::NewGuid().ToString('N'))
$root = Join-Path $stage $name
try {
    New-Item -ItemType Directory -Force -Path $root | Out-Null
    foreach ($d in 'adapter\applewin_mcp', 'adapter\examples', 'docs') { New-Item -ItemType Directory -Force -Path (Join-Path $root $d) | Out-Null }

    Copy-Item -LiteralPath $exe -Destination $root
    $py = @(Get-ChildItem -LiteralPath (Join-Path $repo 'adapter\applewin_mcp') -Filter '*.py' -File)
    if ($py.Count -eq 0) { throw 'No adapter Python files found.' }
    $py | Copy-Item -Destination (Join-Path $root 'adapter\applewin_mcp')
    Copy-Item (Join-Path $repo 'adapter\requirements.txt') (Join-Path $root 'adapter')
    Copy-Item (Join-Path $repo 'adapter\README.md') (Join-Path $root 'adapter')
    Copy-Item (Join-Path $repo 'VERSION') (Join-Path $root 'adapter\VERSION')   # applewin_mcp.__version__ reads ..\VERSION
    Copy-Item (Join-Path $repo 'docs\api_spec.md') (Join-Path $root 'docs')
    foreach ($f in 'README.md', 'LICENSE', 'LICENSE-GPL-2.0.txt', 'THIRD-PARTY-NOTICES.txt') { Copy-Item (Join-Path $repo $f) $root }

    # NOTICE.txt: record the release commit (the repository's copy keeps the placeholder).
    $notice = Read-Text (Join-Path $repo 'NOTICE.txt')
    if (-not $notice.Contains('{{RELEASE_COMMIT}}')) { throw 'NOTICE.txt has no {{RELEASE_COMMIT}} placeholder.' }
    $notice = $notice.Replace('{{RELEASE_COMMIT}}', $release)
    if ($notice.Contains('{{')) { throw 'A placeholder survived in the bundle NOTICE.txt.' }
    Write-Text (Join-Path $root 'NOTICE.txt') $notice

    # Example config: paths into the bundle instead of the repository. The JSON-escaped (doubled backslash) forms
    # come before the single-backslash forms.
    $bs = '\'
    $dos = 'DOS 3.3 System Master - 680-0210-A.dsk'
    $example = Read-Text (Join-Path $repo 'adapter\examples\basic_count.md')
    $rules = @(
        @(('<REPO>{0}{0}external{0}{0}AppleWin{0}{0}bin{0}{0}{1}' -f $bs, $dos), '<DISK>'),
        @(('<REPO>{0}external{0}AppleWin{0}bin{0}{1}' -f $bs, $dos), '<DISK>'),
        @(('<REPO>{0}{0}bin{0}{0}x64{0}{0}Release{0}{0}' -f $bs), ('<BUNDLE>{0}{0}' -f $bs)),
        @(('<REPO>{0}bin{0}x64{0}Release{0}' -f $bs), ('<BUNDLE>{0}' -f $bs)),
        @('- `<REPO>`: the repository root.', '- `<BUNDLE>`: the folder you extracted the release zip into.'),
        @('The disk is `<DISK>`. The harness works on a copy',
          ('`<DISK>` is the full path of your DOS 3.3 System Master image (`{0}`). The zip does not include it; see "Install from the release zip" in the README. The harness works on a copy' -f $dos)),
        @('<REPO>', '<BUNDLE>')
    )
    foreach ($r in $rules) { $example = $example.Replace($r[0], $r[1]) }
    foreach ($bad in '<REPO>', 'external', 'bin\x64', 'bin\\x64') {
        if ($example.Contains($bad)) { throw "The bundle example still mentions '$bad'." }
    }
    if (-not $example.Contains(('<BUNDLE>{0}{0}AppleWinHarness.exe' -f $bs))) { throw 'Example path rewrite failed.' }
    Write-Text (Join-Path $root 'adapter\examples\basic_count.md') $example

    # ---- Zip (forward-slash entry names on both PowerShell editions) ----
    Add-Type -AssemblyName System.IO.Compression
    if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
    $fs = [System.IO.File]::Open($zipPath, [System.IO.FileMode]::CreateNew)
    try {
        $zip = New-Object System.IO.Compression.ZipArchive($fs, [System.IO.Compression.ZipArchiveMode]::Create)
        try {
            $prefix = $stage.TrimEnd('\') + '\'
            foreach ($f in (Get-ChildItem -LiteralPath $root -Recurse -File | Sort-Object FullName)) {
                $entryName = $f.FullName.Substring($prefix.Length).Replace('\', '/')
                $entry = $zip.CreateEntry($entryName, [System.IO.Compression.CompressionLevel]::Optimal)
                $es = $entry.Open()
                try {
                    $in = [System.IO.File]::OpenRead($f.FullName)
                    try { $in.CopyTo($es) } finally { $in.Dispose() }
                } finally { $es.Dispose() }
            }
        } finally { $zip.Dispose() }
    } finally { $fs.Dispose() }
}
finally {
    if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue }
}

Write-Host "Wrote $zipPath (version $version, commit $release)"
