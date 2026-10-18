# Fetch wiliwili's git submodules at their pinned SHAs.
#
# Why not `git submodule update`:
#   1. It shells out to sh.exe, and this sandbox forbids creating named pipes
#      ("couldn't create signal pipe, Win32 error 5"), so sh cannot start.
#   2. This machine's schannel is broken (SEC_E_NO_CREDENTIALS), so git must be
#      told to use its openssl backend explicitly.
# So we read the gitlink SHAs with `ls-tree` and clone/checkout each by hand.
#
# ASCII-ONLY ON PURPOSE: PowerShell 5.1 parses BOM-less .ps1 files using the
# system ANSI codepage (GBK/936 here), which corrupts non-ASCII source text.
# Keep this file pure ASCII and pass non-ASCII paths in as parameters.
#
# Usage: powershell -File fetch-submodules.ps1 -RepoRoot <path-to-wiliwili>

param(
    [Parameter(Mandatory = $true)][string]$RepoRoot
)

$ErrorActionPreference = 'Stop'
$env:GIT_TERMINAL_PROMPT = '0'

if (-not (Test-Path $RepoRoot)) { throw "RepoRoot not found: $RepoRoot" }
$RepoRoot = (Resolve-Path $RepoRoot).Path
$lib = Join-Path $RepoRoot 'library'
if (-not (Test-Path $lib)) { throw "no library/ under $RepoRoot" }

$subs = @(
    @{ name = 'MemoryModule';      url = 'https://github.com/xfangfang/MemoryModule.git';   sha = '7739ba4b2d87395446bbdcad6ae8bf9131b4250b' },
    @{ name = 'OpenCC';            url = 'https://github.com/xfangfang/OpenCC.git';         sha = 'ccae908834c2fe41ba02141fa5d0eef178a45080' },
    @{ name = 'QR-Code-generator'; url = 'https://github.com/nayuki/QR-Code-generator.git'; sha = '720f62bddb7226106071d4728c292cb1df519ceb' },
    @{ name = 'borealis';          url = 'https://github.com/xfangfang/borealis.git';       sha = '5f08b286f3df737f3321d2247a6fe633fcead03c' },
    @{ name = 'cpr';               url = 'https://github.com/xfangfang/cpr.git';            sha = 'ef35a614f1feb6ba0d4de13b1950bcaf7faad060' },
    @{ name = 'libpdr';            url = 'https://github.com/xfangfang/libpdr.git';         sha = '9dd3ab920940adac013a3cf40e3a5805d7e193e1' },
    @{ name = 'lunasvg';           url = 'https://github.com/sammycage/lunasvg.git';        sha = 'f924651b85cac47dbe15f51a4aa320461fc1d07b' },
    @{ name = 'mongoose';          url = 'https://github.com/xfangfang/mongoose.git';       sha = '4328842400370f62d00b6a6b23f1cbe0cded4073' },
    @{ name = 'pystring';          url = 'https://github.com/imageworks/pystring.git';      sha = '7d16bc814ccb4cad03c300dcb77440034caa84f7' }
)

$gitArgs = @('-c', 'http.sslBackend=openssl')

foreach ($s in $subs) {
    $dest = Join-Path $lib $s.name
    $marker = Join-Path $dest '.git'

    if (Test-Path $marker) {
        Write-Host "SKIP $($s.name) (already a git repo)"
        continue
    }

    # A zip-extracted tree leaves empty placeholder dirs; clear them so clone can land.
    if (Test-Path $dest) {
        $n = (Get-ChildItem $dest -Recurse -Force -ErrorAction SilentlyContinue | Measure-Object).Count
        if ($n -gt 0) { Write-Host "SKIP $($s.name) (non-empty, $n entries)"; continue }
        Remove-Item -Recurse -Force $dest
    }

    Write-Host "=== clone $($s.name) ==="
    & git @gitArgs clone --quiet $s.url $dest 2>&1 | Out-Host
    if ($LASTEXITCODE -ne 0) { Write-Host "FAIL clone $($s.name)"; continue }

    & git -C $dest @gitArgs checkout --quiet $s.sha 2>&1 | Out-Host
    if ($LASTEXITCODE -ne 0) {
        Write-Host "FAIL checkout $($s.name) @ $($s.sha)"
        continue
    }

    # Confirm the checkout actually landed on the pinned SHA.
    $head = ((& git -C $dest rev-parse HEAD 2>&1) -join '').Trim()
    $mark = 'MISMATCH'
    if ($head -eq $s.sha) { $mark = 'OK      ' }
    Write-Host ("{0} {1} head={2} want={3}" -f $mark, $s.name, $head.Substring(0, 12), $s.sha.Substring(0, 12))
}

Write-Host ""
Write-Host "=== final state ==="
foreach ($s in $subs) {
    $dest = Join-Path $lib $s.name
    $files = (Get-ChildItem $dest -Recurse -File -Force -ErrorAction SilentlyContinue | Measure-Object).Count
    Write-Host ("{0,-18} files={1}" -f $s.name, $files)
}
