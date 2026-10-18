# Build and run the host test suites with TinyCC.
#
# NINETEEN counted suites, 1314 checks, 0 failures. Per suite, taken from an actual full run (the labels
# are the ones the script prints): crypto 21, signer 111, cenc 21, mp4 13, router 45, api 138,
# fixture 114, pmenu 133, index 27, ctl 64, grid 111, scrub 81, quality 49, httpcfg 98, shot 36,
# lang 32, hist 92, sb 66, playinfo 62. A twentieth RUN block has no check total of its own because it is
# a source grep rather than a compiled suite: "the liveness-token invariant", which proves every
# brls::sync callback that captures a screen also checks its token.
#
# Seven of the suites are the original ones (crypto, signer, cenc, mp4, router, api, fixture); the rest
# were added after them, each because something specific goes wrong silently without it: hg_pmenu (the
# player picture menu), hg_grid (poster and numbered-button grid geometry, including the up/down row
# step: the short last row, a heading's own row, and the two ends), hg_scrub (drag-to-seek), hg_quality
# (the quality ladder), hg_httpcfg (the HTTP client's tunables, where the REFUSALS are what is really
# being tested), hg_shot (the screenshot-raw probe, where the point is that a contradictory reply is NOT
# called a win), hg_lang (the interface language table), hg_history (local history and the watchlist,
# where a title with a newline must not split a record), hg_sidebar (the sidebar's movement rules, where
# focus must never land on a disabled item), and hg_play_info (build173: the play address route --
# player_access_key -> CENC key pinned to FOUR vectors captured live from the service, whose expected
# values come from the SITE'S OWN openBox() rather than from the C under test, plus the play_info
# envelope pinned to a real 2,505-byte response). The added suites run LAST so their summaries are easy
# to find.
#
# STALE TOTALS ARE A KNOWN HAZARD HERE: this comment has previously said 1049 (whose own breakdown summed
# to 1018) and 1052 (same problem). The figures above were re-measured from a full run rather than
# adjusted by hand, because the number gets quoted in the handover document as the baseline. The cenc
# suite reads 21 rather than the 15 it read before build173, because the openBox vectors were added to
# it; no existing assertion was weakened or removed to make room, and every count here is a floor -- a
# check that is no longer true is fixed, never deleted.
#
# TinyCC is used deliberately: it is i386 (ILP32), while the target is
# aarch64 (LP64). Compiling the same sources under both widths is the cheapest
# way to catch a stray `long` or an implicit int truncation that would work on
# the host and corrupt the signature on the Switch.
#
# ASCII-only file: PowerShell 5.1 parses BOM-less .ps1 using the system ANSI
# codepage (GBK/936 here), which corrupts non-ASCII source text. Non-ASCII
# paths arrive as parameters instead.
#
# Usage: powershell -File run.ps1 -SrcDir <dir> [-TccPath <tcc.exe>] [-Aarch64Gcc <gcc.exe>]

param(
    [Parameter(Mandatory = $true)][string]$SrcDir,
    [string]$TccPath = '<tcc.exe>',
    [string]$Aarch64Gcc = 'C:\devkitPro\devkitA64\bin\aarch64-none-elf-gcc.exe'
)

$ErrorActionPreference = 'Stop'

function Invoke-Tool {
    param([string]$Exe, [string[]]$ToolArgs)
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $code = 1
    try {
        & $Exe @ToolArgs 2>&1 | Out-Host
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $prev
    }
    return $code
}

if (-not (Test-Path $SrcDir)) { throw "SrcDir not found: $SrcDir" }
$SrcDir = (Resolve-Path $SrcDir).Path

# Build under the temp dir: the workspace path is non-ASCII and some tools
# mishandle it in arguments.
$scratch = Join-Path $env:TEMP 'hg_crypto_hosttest'
if (Test-Path $scratch) { Remove-Item -Recurse -Force $scratch }
New-Item -ItemType Directory -Force -Path $scratch | Out-Null
Write-Host "scratch: $scratch"

$sources = @(
    (Join-Path $SrcDir 'hg_crypto.c'),
    (Join-Path $SrcDir 'hg_aesv3.c'),
    (Join-Path $SrcDir 'tests\hg_crypto_test.c')
)
foreach ($s in $sources) { if (-not (Test-Path $s)) { throw "missing source: $s" } }

$signSources = @(
    (Join-Path $SrcDir 'hg_crypto.c'),
    (Join-Path $SrcDir 'hg_aes.c'),
    (Join-Path $SrcDir 'hg_aesv3.c'),
    (Join-Path $SrcDir 'hg_sign.c'),
    (Join-Path $SrcDir 'hg_medusa.c'),
    (Join-Path $SrcDir 'hg_request.c'),
    (Join-Path $SrcDir 'tests\hg_sign_test.c')
)
foreach ($s in $signSources) { if (-not (Test-Path $s)) { throw "missing source: $s" } }

$cencSources = @(
    (Join-Path $SrcDir 'hg_crypto.c'),
    (Join-Path $SrcDir 'hg_aes.c'),
    (Join-Path $SrcDir 'hg_cenc.c'),
    (Join-Path $SrcDir 'tests\hg_cenc_test.c')
)
foreach ($s in $cencSources) { if (-not (Test-Path $s)) { throw "missing source: $s" } }

$mp4Sources = @(
    (Join-Path $SrcDir 'hg_crypto.c'),
    (Join-Path $SrcDir 'hg_aes.c'),
    (Join-Path $SrcDir 'hg_cenc.c'),
    (Join-Path $SrcDir 'hg_mp4.c'),
    (Join-Path $SrcDir 'tests\hg_mp4_test.c')
)
foreach ($s in $mp4Sources) { if (-not (Test-Path $s)) { throw "missing source: $s" } }

$routerSources = @(
    (Join-Path $SrcDir 'hg_crypto.c'),
    (Join-Path $SrcDir 'hg_aes.c'),
    (Join-Path $SrcDir 'hg_cenc.c'),
    (Join-Path $SrcDir 'hg_router.c'),
    (Join-Path $SrcDir 'hg_play.c'),
    (Join-Path $SrcDir 'tests\hg_router_test.c')
)
foreach ($s in $routerSources) { if (-not (Test-Path $s)) { throw "missing source: $s" } }

$apiSources = @(
    (Join-Path $SrcDir 'hg_api.c'),
    (Join-Path $SrcDir 'hg_router.c'),
    (Join-Path $SrcDir 'tests\hg_api_test.c')
)
foreach ($s in $apiSources) { if (-not (Test-Path $s)) { throw "missing source: $s" } }

# The fixture suite runs against bytes captured from the LIVE upstream, so it
# links hg_play.c as well (the video-model envelope) and needs the fixtures on
# disk at run time -- the tests read them with fopen, hence the working directory
# has to be the repo root rather than the scratch build dir.
$fixtureSources = @(
    (Join-Path $SrcDir 'hg_api.c'),
    (Join-Path $SrcDir 'hg_router.c'),
    (Join-Path $SrcDir 'hg_crypto.c'),
    (Join-Path $SrcDir 'hg_aes.c'),
    (Join-Path $SrcDir 'hg_cenc.c'),
    (Join-Path $SrcDir 'hg_play.c'),
    (Join-Path $SrcDir 'tests\hg_fixture_test.c')
)
foreach ($s in $fixtureSources) { if (-not (Test-Path $s)) { throw "missing source: $s" } }

# The player picture menu (hg_pmenu) is pure arithmetic over its own header: it
# links nothing else, which is the whole point of having put it in C.
$pmenuSources = @(
    (Join-Path $SrcDir 'hg_pmenu.c'),
    (Join-Path $SrcDir 'hg_lang.c'),   # the row-label and value keys the panel draws
    (Join-Path $SrcDir 'tests\hg_pmenu_test.c')
)
foreach ($s in $pmenuSources) { if (-not (Test-Path $s)) { throw "missing source: $s" } }

# The home poster grid's geometry (hg_grid) is pure arithmetic over its own header,
# like hg_pmenu: where each card goes, how many columns a logical width yields, and
# the total content height. It links nothing else on purpose -- the whole reason it is
# in C is that the host suite can reach it.
$indexSources = @(
    (Join-Path $SrcDir 'hg_index.c'),
    (Join-Path $SrcDir 'tests\hg_index_test.c')
)

$ctlSources = @(
    (Join-Path $SrcDir 'hg_ctl.c'),
    (Join-Path $SrcDir 'tests\hg_ctl_test.c')
)

$gridSources = @(
    (Join-Path $SrcDir 'hg_grid.c'),
    (Join-Path $SrcDir 'tests\hg_grid_test.c')
)
foreach ($s in $gridSources) { if (-not (Test-Path $s)) { throw "missing source: $s" } }

# The drag-to-seek model (hg_scrub) is pure arithmetic over its own header, like hg_pmenu
# and hg_grid: the target position, its clamp, the preview rate limit, and the two bar
# geometry functions that must be inverses of each other.
$scrubSources = @(
    (Join-Path $SrcDir 'hg_scrub.c'),
    (Join-Path $SrcDir 'tests\hg_scrub_test.c')
)
foreach ($s in $scrubSources) { if (-not (Test-Path $s)) { throw "missing source: $s" } }

# The quality ladder (hg_quality) is pure ordering and labelling over its own header. Its
# checks are driven by the five tiers MEASURED in fixtures/video_model.json, because the
# trap it exists for -- quality_type 40 is 360p and 1 is 1080p, so a numeric sort offers the
# WORST picture as the default -- is invisible without those real numbers.
$qualitySources = @(
    (Join-Path $SrcDir 'hg_quality.c'),
    (Join-Path $SrcDir 'tests\hg_quality_test.c')
)
foreach ($s in $qualitySources) { if (-not (Test-Path $s)) { throw "missing source: $s" } }

# The HTTP client's tunables (hg_httpcfg). Pure parsing, ranges and refusals over its own header --
# and the refusals are the point: timeout 0 means "never" in libcurl, a connect timeout above the
# total can never be reached, and a proxy with no scheme is a string libcurl misreads.
$httpcfgSources = @(
    (Join-Path $SrcDir 'hg_httpcfg.c'),
    (Join-Path $SrcDir 'tests\hg_httpcfg_test.c')
)
foreach ($s in $httpcfgSources) { if (-not (Test-Path $s)) { throw "missing source: $s" } }

# The screenshot-raw probe's parsing (hg_shot). It needs hg_router.c for the JSON reader, and its
# important check is that a reply whose data contradicts its own w/h/stride is NOT called a win --
# a false pass would send the thumbnail feature down the wrong path.
$shotSources = @(
    (Join-Path $SrcDir 'hg_shot.c'),
    (Join-Path $SrcDir 'hg_router.c'),
    (Join-Path $SrcDir 'tests\hg_shot_test.c')
)
foreach ($s in $shotSources) { if (-not (Test-Path $s)) { throw "missing source: $s" } }

# The interface language table (hg_lang). Its checks are that every key has BOTH languages and that
# every string is well-formed UTF-8 -- the Chinese is stored as \xNN byte escapes to keep the source
# ASCII for this project's GBK build machine, and a mistyped escape is something no compiler reports.
$langSources = @(
    (Join-Path $SrcDir 'hg_lang.c'),
    (Join-Path $SrcDir 'tests\hg_lang_test.c')
)
foreach ($s in $langSources) { if (-not (Test-Path $s)) { throw "missing source: $s" } }

# Local watch history and the watchlist (hg_history). The invariant it exists to protect is that a
# title containing a newline or a tab must not split a record -- without it a corrupted file would
# present as "history shows the wrong drama" with no error message anywhere. It links hg_lang for the
# age strings, so that module is compiled in here too.
$histSources = @(
    (Join-Path $SrcDir 'hg_history.c'),
    (Join-Path $SrcDir 'hg_lang.c'),
    (Join-Path $SrcDir 'tests\hg_history_test.c')
)
foreach ($s in $histSources) { if (-not (Test-Path $s)) { throw "missing source: $s" } }

# The left sidebar model (hg_sidebar). Its checks are the MOVEMENT ones, because that is where a
# sidebar goes quietly wrong: never landing on a disabled item, wrapping at both ends, and staying
# inside its own group. It links hg_lang for the labels.
$sbSources = @(
    (Join-Path $SrcDir 'hg_sidebar.c'),
    (Join-Path $SrcDir 'hg_lang.c'),
    (Join-Path $SrcDir 'tests\hg_sidebar_test.c')
)
foreach ($s in $sbSources) { if (-not (Test-Path $s)) { throw "missing source: $s" } }

# The play-info route (build173): player_access_key -> CENC key, and the play_info envelope. It links
# hg_play.c (the envelope parser and the HgPlayPlan bridge) and hg_cenc.c (the key derivation, which now
# holds BOTH schemes side by side).
#
# WHY THESE CHECKS EXIST: both halves fail SILENTLY when wrong. A wrong content key does not raise an
# error -- it decrypts to garbage, mpv reports nothing, and the console shows a black screen with a clean
# log; and a mis-parsed envelope just leaves the URLs empty. So the key is pinned to FOUR vectors captured
# live from the service, whose expected values come from the SITE'S OWN openBox() rather than from the C
# under test (_probe/cap-test-openbox.mjs), and the envelope is pinned to a REAL 2,505-byte response
# stored as fixtures/play_info_720p.json. That fixture is read with fopen, so it needs the repository root
# as the working directory -- the same rule the fixture suite follows.
$playInfoSources = @(
    (Join-Path $SrcDir 'hg_play.c'),
    (Join-Path $SrcDir 'hg_cenc.c'),
    (Join-Path $SrcDir 'hg_crypto.c'),
    (Join-Path $SrcDir 'hg_aes.c'),
    (Join-Path $SrcDir 'hg_router.c'),
    (Join-Path $SrcDir 'tests\hg_play_info_test.c')
)
foreach ($s in $playInfoSources) { if (-not (Test-Path $s)) { throw "missing source: $s" } }

$exe = Join-Path $scratch 'hg_crypto_test.exe'
$signExe = Join-Path $scratch 'hg_sign_test.exe'
$cencExe = Join-Path $scratch 'hg_cenc_test.exe'
$mp4Exe = Join-Path $scratch 'hg_mp4_test.exe'
$routerExe = Join-Path $scratch 'hg_router_test.exe'
$apiExe = Join-Path $scratch 'hg_api_test.exe'
$fixtureExe = Join-Path $scratch 'hg_fixture_test.exe'
$pmenuExe = Join-Path $scratch 'hg_pmenu_test.exe'
$gridExe = Join-Path $scratch 'hg_grid_test.exe'
$ctlExe = Join-Path $scratch 'hg_ctl_test.exe'
$indexExe = Join-Path $scratch 'hg_index_test.exe'
$scrubExe = Join-Path $scratch 'hg_scrub_test.exe'
$qualityExe = Join-Path $scratch 'hg_quality_test.exe'
$httpcfgExe = Join-Path $scratch 'hg_httpcfg_test.exe'
$shotExe = Join-Path $scratch 'hg_shot_test.exe'
$langExe = Join-Path $scratch 'hg_lang_test.exe'
$histExe = Join-Path $scratch 'hg_history_test.exe'
$sbExe = Join-Path $scratch 'hg_sidebar_test.exe'
$playInfoExe = Join-Path $scratch 'hg_play_info_test.exe'

Write-Host ''
Write-Host '=== host build (TinyCC, i386) ==='
if (-not (Test-Path $TccPath)) { throw "TinyCC not found at $TccPath (pass -TccPath)" }
# Parenthesise the concatenation. In PowerShell `+` binds tighter than `,`, so
# @('-I' + $dir, '-o', $exe) is parsed as '-I' + ($dir, '-o', $exe) and silently
# collapses the whole argument list into ONE space-joined string.
$tccArgs = @(('-I' + $SrcDir), '-o', $exe) + $sources
$rc = Invoke-Tool -Exe $TccPath -ToolArgs $tccArgs
if ($rc -ne 0) { throw "TinyCC compile failed (rc=$rc)" }

$signArgs = @(('-I' + $SrcDir), '-I' + (Join-Path $SrcDir 'tests'), '-o', $signExe) + $signSources
$rc = Invoke-Tool -Exe $TccPath -ToolArgs $signArgs
if ($rc -ne 0) { throw "TinyCC compile failed for the signer tests (rc=$rc)" }

$cencArgs = @(('-I' + $SrcDir), '-o', $cencExe) + $cencSources
$rc = Invoke-Tool -Exe $TccPath -ToolArgs $cencArgs
if ($rc -ne 0) { throw "TinyCC compile failed for the CENC tests (rc=$rc)" }

$mp4Args = @(('-I' + $SrcDir), '-o', $mp4Exe) + $mp4Sources
$rc = Invoke-Tool -Exe $TccPath -ToolArgs $mp4Args
if ($rc -ne 0) { throw "TinyCC compile failed for the MP4 tests (rc=$rc)" }

$routerArgs = @(('-I' + $SrcDir), '-o', $routerExe) + $routerSources
$rc = Invoke-Tool -Exe $TccPath -ToolArgs $routerArgs
if ($rc -ne 0) { throw "TinyCC compile failed for the router tests (rc=$rc)" }

$apiArgs = @(('-I' + $SrcDir), '-o', $apiExe) + $apiSources
$rc = Invoke-Tool -Exe $TccPath -ToolArgs $apiArgs
if ($rc -ne 0) { throw "TinyCC compile failed for the API tests (rc=$rc)" }

$fixtureArgs = @(('-I' + $SrcDir), '-o', $fixtureExe) + $fixtureSources
$rc = Invoke-Tool -Exe $TccPath -ToolArgs $fixtureArgs
if ($rc -ne 0) { throw "TinyCC compile failed for the fixture tests (rc=$rc)" }

$pmenuArgs = @(('-I' + $SrcDir), '-o', $pmenuExe) + $pmenuSources
$rc = Invoke-Tool -Exe $TccPath -ToolArgs $pmenuArgs
if ($rc -ne 0) { throw "TinyCC compile failed for the pmenu tests (rc=$rc)" }

$indexArgs = @(('-I' + $SrcDir), '-o', $indexExe) + $indexSources
$rc = Invoke-Tool -Exe $TccPath -ToolArgs $indexArgs
if ($rc -ne 0) { throw "TinyCC compile failed for the index tests (rc=$rc)" }

$ctlArgs = @(('-I' + $SrcDir), '-o', $ctlExe) + $ctlSources
$rc = Invoke-Tool -Exe $TccPath -ToolArgs $ctlArgs
if ($rc -ne 0) { throw "TinyCC compile failed for the control-value tests (rc=$rc)" }
$gridArgs = @(('-I' + $SrcDir), '-o', $gridExe) + $gridSources
$rc = Invoke-Tool -Exe $TccPath -ToolArgs $gridArgs
if ($rc -ne 0) { throw "TinyCC compile failed for the grid tests (rc=$rc)" }

$scrubArgs = @(('-I' + $SrcDir), '-o', $scrubExe) + $scrubSources
$rc = Invoke-Tool -Exe $TccPath -ToolArgs $scrubArgs
if ($rc -ne 0) { throw "TinyCC compile failed for the scrub tests (rc=$rc)" }

$qualityArgs = @(('-I' + $SrcDir), '-o', $qualityExe) + $qualitySources
$rc = Invoke-Tool -Exe $TccPath -ToolArgs $qualityArgs
if ($rc -ne 0) { throw "TinyCC compile failed for the quality tests (rc=$rc)" }
$httpcfgArgs = @(('-I' + $SrcDir), '-o', $httpcfgExe) + $httpcfgSources
$rc = Invoke-Tool -Exe $TccPath -ToolArgs $httpcfgArgs
if ($rc -ne 0) { throw "TinyCC compile failed for the httpcfg tests (rc=$rc)" }
$shotArgs = @(('-I' + $SrcDir), '-o', $shotExe) + $shotSources
$rc = Invoke-Tool -Exe $TccPath -ToolArgs $shotArgs
if ($rc -ne 0) { throw "TinyCC compile failed for the shot tests (rc=$rc)" }
$langArgs = @(('-I' + $SrcDir), '-o', $langExe) + $langSources
$rc = Invoke-Tool -Exe $TccPath -ToolArgs $langArgs
if ($rc -ne 0) { throw "TinyCC compile failed for the lang tests (rc=$rc)" }
$histArgs = @(('-I' + $SrcDir), '-o', $histExe) + $histSources
$rc = Invoke-Tool -Exe $TccPath -ToolArgs $histArgs
if ($rc -ne 0) { throw "TinyCC compile failed for the history tests (rc=$rc)" }
$sbArgs = @(('-I' + $SrcDir), '-o', $sbExe) + $sbSources
$rc = Invoke-Tool -Exe $TccPath -ToolArgs $sbArgs
if ($rc -ne 0) { throw "TinyCC compile failed for the sidebar tests (rc=$rc)" }
$playInfoArgs = @(('-I' + $SrcDir), '-o', $playInfoExe) + $playInfoSources
$rc = Invoke-Tool -Exe $TccPath -ToolArgs $playInfoArgs
if ($rc -ne 0) { throw "TinyCC compile failed for the play-info tests (rc=$rc)" }

Write-Host ''
Write-Host '=== run: crypto primitives ==='
$rc = Invoke-Tool -Exe $exe -ToolArgs @()
Write-Host ''
Write-Host '=== run: signer against signing.json ==='
$rcSign = Invoke-Tool -Exe $signExe -ToolArgs @()
Write-Host ''
Write-Host '=== run: AES / CENC ==='
$rcCenc = Invoke-Tool -Exe $cencExe -ToolArgs @()
Write-Host ''
Write-Host '=== run: MP4 header parsing ==='
$rcMp4 = Invoke-Tool -Exe $mp4Exe -ToolArgs @()
Write-Host ''
Write-Host '=== run: _ROUTER_DATA scraping ==='
$rcRouter = Invoke-Tool -Exe $routerExe -ToolArgs @()
Write-Host ''
Write-Host '=== run: response parsing (search / detail / video model) ==='
$rcApi = Invoke-Tool -Exe $apiExe -ToolArgs @()
Write-Host ''
Write-Host '=== run: live-fixture parsing + playback plan ==='
# The fixtures are read relative to the repo root, so run from there.
$prevDir = Get-Location
try {
    Set-Location (Split-Path -Parent (Split-Path -Parent $SrcDir))
    $rcFixture = Invoke-Tool -Exe $fixtureExe -ToolArgs @()
} finally {
    Set-Location $prevDir
}
Write-Host ''
Write-Host '=== run: player picture menu arithmetic ==='
$rcPmenu = Invoke-Tool -Exe $pmenuExe -ToolArgs @()
Write-Host ''
# --- the liveness-token invariant (round 36) -------------------------------------
# SEE hg_alive.hpp. Every brls::sync callback that captures the screen pointer also captures the token, and of
# those, every one must CHECK the token before it touches anything. That rule was broken once already -- the
# search screen's result callback was written without the guard, and it is the callback most likely of all to
# arrive after the user has left. Counting the two things per file is what found it, so the count runs here
# rather than being a rule someone has to remember.
Write-Host ''
Write-Host '=== run: the liveness-token invariant (source check) ==='
$tokenViolations = @()
foreach ($src in Get-ChildItem (Join-Path $SrcDir '..\hongguo') -Filter '*.cpp' -ErrorAction SilentlyContinue) {
    $text = Get-Content $src.FullName -Raw
    $captures = ([regex]::Matches($text, 'brls::sync\(\[(self|this|st), alive')).Count
    if ($captures -eq 0) { continue }
    # TWO ACCEPTED SHAPES, because one of them cannot use the macro: `HG_GUARD_ALIVE` returns early, which is
    # right for a callback that only reads, and WRONG for the network probe -- it holds ptrLock on the rows it
    # reports into and must release them on every path, so it asks the same question (`alive->dead()`) into a
    # local and unlocks at the end. Counting both keeps the invariant useful instead of forcing a macro that
    # would leak a tab.
    $guards = ([regex]::Matches($text, 'HG_GUARD_ALIVE')).Count
    $guards += ([regex]::Matches($text, '(alive|token)->dead\(\)')).Count
    if ($guards -lt $captures) {
        $tokenViolations += "$($src.Name): $captures guarded callbacks but only $guards HG_GUARD_ALIVE checks"
    }
}
if ($tokenViolations.Count -gt 0) {
    foreach ($v in $tokenViolations) { Write-Host "  FAIL  $v" }
    $rcToken = 1
} else {
    Write-Host '  PASS  every brls::sync callback that captures a screen also checks its token'
    $rcToken = 0
}
Write-Host '=== run: the local index merge policy ==='
$rcIndex = Invoke-Tool -Exe $indexExe -ToolArgs @()
Write-Host ''
Write-Host '=== run: player control values (volume, speed, zoom) ==='
$rcCtl = Invoke-Tool -Exe $ctlExe -ToolArgs @()`r`nWrite-Host ''
Write-Host '=== run: home poster grid geometry ==='
$rcGrid = Invoke-Tool -Exe $gridExe -ToolArgs @()
Write-Host ''
Write-Host '=== run: drag-to-seek model and progress bar ==='
$rcScrub = Invoke-Tool -Exe $scrubExe -ToolArgs @()
Write-Host ''
Write-Host '=== run: the quality ladder ==='
$rcQuality = Invoke-Tool -Exe $qualityExe -ToolArgs @()
Write-Host ''
Write-Host '=== run: the HTTP client configuration ==='
$rcHttpcfg = Invoke-Tool -Exe $httpcfgExe -ToolArgs @()
Write-Host ''
Write-Host '=== run: the screenshot-raw probe ==='
$rcShot = Invoke-Tool -Exe $shotExe -ToolArgs @()
Write-Host ''
Write-Host '=== run: the interface language table ==='
$rcLang = Invoke-Tool -Exe $langExe -ToolArgs @()
Write-Host ''
Write-Host '=== run: local history and watchlist ==='
$rcHist = Invoke-Tool -Exe $histExe -ToolArgs @()
Write-Host ''
Write-Host '=== run: the left sidebar model ==='
$rcSb = Invoke-Tool -Exe $sbExe -ToolArgs @()
Write-Host ''
Write-Host '=== run: the play-info route (player_access_key -> CENC key, play_info envelope) ==='
$rcPlayInfo = Invoke-Tool -Exe $playInfoExe -ToolArgs @()
Write-Host ''
Write-Host "crypto: $rc   signer: $rcSign   cenc: $rcCenc   mp4: $rcMp4   router: $rcRouter   api: $rcApi   fixture: $rcFixture   pmenu: $rcPmenu   grid: $rcGrid   scrub: $rcScrub   quality: $rcQuality   httpcfg: $rcHttpcfg   shot: $rcShot   lang: $rcLang   hist: $rcHist   sb: $rcSb   playinfo: $rcPlayInfo   ctl: $rcCtl   index: $rcIndex"
if ($rcSign -ne 0) { $rc = $rcSign }
if ($rcCenc -ne 0) { $rc = $rcCenc }
if ($rcMp4 -ne 0) { $rc = $rcMp4 }
if ($rcRouter -ne 0) { $rc = $rcRouter }
if ($rcApi -ne 0) { $rc = $rcApi }
if ($rcFixture -ne 0) { $rc = $rcFixture }
if ($rcPmenu -ne 0) { $rc = $rcPmenu }
if ($rcGrid -ne 0) { $rc = $rcGrid }
if ($rcScrub -ne 0) { $rc = $rcScrub }
if ($rcQuality -ne 0) { $rc = $rcQuality }
if ($rcHttpcfg -ne 0) { $rc = $rcHttpcfg }
if ($rcShot -ne 0) { $rc = $rcShot }
if ($rcLang -ne 0) { $rc = $rcLang }
if ($rcHist -ne 0) { $rc = $rcHist }
if ($rcSb -ne 0) { $rc = $rcSb }
if ($rcPlayInfo -ne 0) { $rc = $rcPlayInfo }
if ($rcCtl -ne 0) { $rc = $rcCtl }
if ($rcIndex -ne 0) { $rc = $rcIndex }
if ($rcToken -ne 0) { $rc = $rcToken }

# ---------------------------------------------------------------------------
# THEME KEYS: every key the app asks for must exist in borealis' theme table.
#
# WHY THIS CHECK EXISTS, and it is measured rather than hypothetical: build34 was installed on the
# console and the app was GONE one frame in, with nothing on screen. The reason was one line in
# hg_sidebar_view.cpp -- a border colour looked up as "brls/separator", which this theme does not
# have. ThemeValues::getColor calls fatal() for an unknown key (theme.cpp:155), fatal() throws,
# nothing catches it, std::terminate aborts the process. A COLOUR TYPO KILLED THE APP, and the code
# carrying it had never been instantiated before (written in round 17, wired up in round 21).
#
# A C test cannot see this: the keys live in a C++ framework header the host suites do not build. A
# grep can, and it belongs here because this script is what "the tests" means for this project.
#
# ASCII-only file: this text is ASCII, and so are all the keys and paths involved.
Write-Host ''
Write-Host '=== theme keys used by the app exist in the theme ==='
$repoRoot = Split-Path (Split-Path $SrcDir -Parent) -Parent
$appDir   = Join-Path $repoRoot 'app\hongguo'
$themeCpp = Join-Path $repoRoot 'upstream\wiliwili\library\borealis\library\lib\core\theme.cpp'
if ((Test-Path $appDir) -and (Test-Path $themeCpp)) {
    $themeText = Get-Content $themeCpp -Raw
    $defined = @{}
    foreach ($m in [regex]::Matches($themeText, '\{\s*"(brls/[^"]+)"')) { $defined[$m.Groups[1].Value] = $true }

    $used = @{}
    foreach ($f in (Get-ChildItem $appDir -Filter *.cpp -File)) {
        $text = Get-Content $f.FullName -Raw
        # getColor("k")  |  themeColor("k", fallback)  |  theme["k"]
        foreach ($m in [regex]::Matches($text, '(?:getColor|themeColor)\(\s*"(brls/[^"]+)"')) {
            $used[$m.Groups[1].Value] = $f.Name
        }
        foreach ($m in [regex]::Matches($text, 'theme\[\s*"(brls/[^"]+)"\s*\]')) {
            $used[$m.Groups[1].Value] = $f.Name
        }
    }

    $missing = @()
    foreach ($k in ($used.Keys | Sort-Object)) {
        if (-not $defined.ContainsKey($k)) { $missing += "$k (used in $($used[$k]))" }
    }
    Write-Host "theme keys: $($defined.Count) defined, $($used.Count) used by the app"
    if ($missing.Count -eq 0) {
        Write-Host 'OK  every theme key the app uses exists (an unknown one would abort the app)'
    } else {
        foreach ($k in $missing) { Write-Host "MISSING theme key: $k" }
        Write-Host 'FAIL  an unknown theme key calls fatal() in ThemeValues::getColor and kills the app'
        $rc = 1
    }
} else {
    Write-Host "skipped: $appDir or $themeCpp not found"
}

# Cross-check that the same sources compile clean for the real target.
Write-Host ''
Write-Host '=== target build (devkitA64, aarch64, compile only) ==='
if (Test-Path $Aarch64Gcc) {
    $a64fail = 0
    foreach ($c in @('hg_crypto.c', 'hg_aesv3.c', 'hg_sign.c', 'hg_medusa.c', 'hg_aes.c', 'hg_cenc.c', 'hg_mp4.c', 'hg_router.c', 'hg_api.c', 'hg_http.c', 'hg_request.c', 'hg_play.c', 'hg_pmenu.c', 'hg_grid.c', 'hg_scrub.c', 'hg_quality.c', 'hg_httpcfg.c', 'hg_shot.c', 'hg_lang.c', 'hg_history.c', 'hg_sidebar.c', 'tests\hg_fixture_test.c', 'tests\hg_play_info_test.c')) {
        # The object name flattens the source's own directory: `tests\x.c` would
        # otherwise try to write into $scratch\tests, which does not exist.
        $obj = Join-Path $scratch ((($c -replace '[\\/]', '_') -replace '\.c$', '_a64.o'))
        $rc2 = Invoke-Tool -Exe $Aarch64Gcc -ToolArgs @(
            '-c', '-std=c99', '-Wall', '-Wextra',
            '-march=armv8-a+crc+crypto', '-mtune=cortex-a57', '-mtp=soft', '-D__SWITCH__',
            '-DCURL_STATICLIB',
            '-IC:/devkitPro/portlibs/switch/include',
            '-IC:/devkitPro/libnx/include',
            ('-I' + $SrcDir), (Join-Path $SrcDir $c), '-o', $obj
        )
        if ($rc2 -ne 0) {
            Write-Host "aarch64 compile FAILED: $c (rc=$rc2)"
            $a64fail = 1
        } else {
            Write-Host "aarch64 compile OK: $c -> $((Get-Item $obj).Length) bytes"
        }
    }
    if ($a64fail -ne 0) { $rc = 1 }
} else {
    Write-Host "skipped: devkitA64 gcc not found at $Aarch64Gcc"
}

Remove-Item -Recurse -Force $scratch -ErrorAction SilentlyContinue
exit $rc
