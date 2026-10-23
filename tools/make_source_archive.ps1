# SPDX-License-Identifier: MIT
<#
.SYNOPSIS
  Packages the complete source, including the external/AppleWin submodule, into dist/applewin-harness-<version>-source.zip.
.DESCRIPTION
  This is the "corresponding source" that NOTICE.txt offers for the release binaries (GPL-2.0-or-later). GitHub's
  automatic source archives leave submodules out, so the release attaches this one as well. It holds the committed
  HEAD of this repository and the submodule commit the repository pins; uncommitted changes are not included.
  Needs git and tar (both are on Windows 10+ and on the GitHub runners). Runs on Windows PowerShell 5.1 and pwsh.
.PARAMETER OutDir
  Output directory, relative to the repository root unless absolute (default dist).
#>
[CmdletBinding()]
param(
    [string]$OutDir = 'dist'
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$utf8 = New-Object System.Text.UTF8Encoding($false)
$version = ([System.IO.File]::ReadAllText((Join-Path $repo 'VERSION'), $utf8)).Trim()
if ($version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+([-+][0-9A-Za-z.-]+)?$') { throw "VERSION does not hold a version: '$version'" }

function Invoke-Native {
    # Runs a native command and throws if it fails (Windows PowerShell 5.1 does not do this for exit codes).
    $exe, $rest = $args
    & $exe @rest
    if ($LASTEXITCODE -ne 0) { throw "$exe $($rest -join ' ') failed with exit code $LASTEXITCODE" }
}

$sub = Join-Path $repo 'external\AppleWin'
$pinned = (& git -C $repo rev-parse 'HEAD:external/AppleWin').Trim()
if ($LASTEXITCODE -ne 0 -or $pinned -notmatch '^[0-9a-f]{40}$') { throw 'Cannot read the pinned AppleWin commit (is this a git checkout?).' }
$have = (& git -C $sub rev-parse HEAD 2>$null)
if ($LASTEXITCODE -ne 0 -or $have.Trim() -ne $pinned) {
    throw "external/AppleWin is not checked out at the pinned commit $pinned. Run: git submodule update --init"
}

$name = "applewin-harness-$version-source"
if ([System.IO.Path]::IsPathRooted($OutDir)) { $outRoot = $OutDir } else { $outRoot = Join-Path $repo $OutDir }
New-Item -ItemType Directory -Force -Path $outRoot | Out-Null
$outRoot = (Resolve-Path $outRoot).Path
$zipPath = Join-Path $outRoot "$name.zip"
$stage = Join-Path ([System.IO.Path]::GetTempPath()) ("awh-source-" + [guid]::NewGuid().ToString('N'))
try {
    New-Item -ItemType Directory -Force -Path $stage | Out-Null
    # git archive writes the committed files with LF endings, as stored, whatever this checkout's autocrlf is.
    Invoke-Native git -C $repo archive --format=tar "--prefix=$name/" -o (Join-Path $stage 'root.tar') HEAD
    Invoke-Native git -C $sub archive --format=tar "--prefix=$name/external/AppleWin/" -o (Join-Path $stage 'applewin.tar') $pinned
    Invoke-Native tar -xf (Join-Path $stage 'root.tar') -C $stage
    Invoke-Native tar -xf (Join-Path $stage 'applewin.tar') -C $stage
    Remove-Item (Join-Path $stage 'root.tar'), (Join-Path $stage 'applewin.tar')

    $root = Join-Path $stage $name
    if (-not (Test-Path -LiteralPath (Join-Path $root 'external\AppleWin\source'))) { throw 'The AppleWin source is missing from the archive.' }

    # Zip with forward-slash entry names on both PowerShell editions.
    Add-Type -AssemblyName System.IO.Compression
    if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
    $fs = [System.IO.File]::Open($zipPath, [System.IO.FileMode]::CreateNew)
    try {
        $zip = New-Object System.IO.Compression.ZipArchive($fs, [System.IO.Compression.ZipArchiveMode]::Create)
        try {
            $prefix = $stage.TrimEnd('\') + '\'
            foreach ($f in (Get-ChildItem -LiteralPath $root -Recurse -File | Sort-Object FullName)) {
                $entry = $zip.CreateEntry($f.FullName.Substring($prefix.Length).Replace('\', '/'), [System.IO.Compression.CompressionLevel]::Optimal)
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

Write-Host "Wrote $zipPath (version $version, AppleWin $pinned)"
