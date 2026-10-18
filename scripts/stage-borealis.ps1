# Stage the borealis tree (and the native app sources) into an ASCII scratch dir.
#
# Two environment limits make this necessary, both already established here:
#   1. aarch64-none-elf-gcc/g++ mangle non-ASCII arguments, and the workspace
#      lives under a non-ASCII path ("D:\...\..."). All compiler work therefore
#      happens under $env:TEMP and only artifacts are copied back.
#   2. Same reason as build-switch-test.ps1: it keeps the build reproducible
#      regardless of where the repo is checked out.
#
# The huge platform trees borealis never uses on Switch (SDL, glfw, android,
# ps4, psv, winrt) are excluded; copying them costs minutes and they are not in
# any source list this build consumes.
#
# ASCII-only file.
#
# Usage: powershell -File stage-borealis.ps1 [-Scratch <dir>] [-Force]

param(
    [string]$Scratch = (Join-Path $env:TEMP 'hgbrls'),
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
$borealis = Join-Path $repo 'upstream\wiliwili\library\borealis'
if (-not (Test-Path $borealis)) { throw "borealis not found: $borealis" }

if ($Force -and (Test-Path $Scratch)) { Remove-Item -Recurse -Force $Scratch -EA SilentlyContinue }
New-Item -ItemType Directory -Force -Path $Scratch | Out-Null

$stamp = Join-Path $Scratch '.staged'
if (Test-Path $stamp) {
    Write-Host "already staged: $Scratch"
    exit 0
}

$exclude = @()
foreach ($rel in @('library\lib\extern\SDL', 'library\lib\extern\glfw',
                   'android-project', 'ps4', 'psv', 'winrt', '.github')) {
    $exclude += (Join-Path $borealis $rel)
}

$args = @($borealis, $Scratch, '/E', '/NFL', '/NDL', '/NJH', '/NJS', '/NP', '/MT:16', '/XD') + $exclude
& robocopy @args | Out-Null
# robocopy: 0-7 are success codes.
if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE)" }

foreach ($need in @('library\include\borealis', 'library\lib\extern\yoga\yoga',
                    'library\lib\extern\switch-libpulsar\include',
                    'library\lib\extern\tweeny\include', 'library\lib\extern\fmt\include',
                    'library\lib\extern\tinyxml2\tinyxml2.cpp',
                    'library\lib\extern\nanovg\nanovg.c',
                    'library\lib\extern\nanovg\deko3d\dk_renderer.cpp',
                    'library\lib\extern\nanovg\deko3d\shaders\fill_vsh.glsl')) {
    if (-not (Test-Path (Join-Path $Scratch $need))) {
        throw "staged tree is incomplete, missing: $need"
    }
}

Set-Content -Path $stamp -Value (Get-Date -Format o) -Encoding ASCII
Write-Host "staged borealis -> $Scratch ($((Get-ChildItem $Scratch -Recurse -File).Count) files)"
