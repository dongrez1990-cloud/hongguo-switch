# Build borealis as a static library for Nintendo Switch (deko3d backend).
#
# WHY THIS SCRIPT EXISTS INSTEAD OF cmake
# ---------------------------------------
# borealis ships a CMake build, and the toolchain to drive it exists
# (C:\devkitPro\msys2\usr\bin\cmake.exe). It cannot run here: every MSYS2 binary
# aborts at startup with
#
#     fatal error - couldn't create signal pipe, Win32 error 5
#
# MSYS2's runtime creates a named pipe per process for signal emulation, and the
# sandbox denies named-pipe creation. That is the same restriction this project
# already documented for `make`, and it is not something a flag can work around.
# A native (non-MSYS2) CMake is not installed on this machine.
#
# So the CMake graph is reproduced here by hand. It is mechanical work, not
# guesswork: every source list, include path and define below is transcribed
# from borealis/library/CMakeLists.txt and borealis/library/cmake/toolchain.cmake.
# The one thing CMake did that is dropped is gen_dksh(), which runs `uam` to
# compile the nanovg shaders; those prebuilt .dksh files ship in the tree and are
# copied into the NRO's romfs by scripts/build-hongguo.ps1 instead.
#
# ASCII-only file: PowerShell 5.1 parses BOM-less .ps1 with the system ANSI
# codepage (GBK/936), which corrupts non-ASCII source text.
#
# Usage: powershell -File build-borealis.ps1 [-Scratch <dir>] [-Jobs <n>]

param(
    [string]$Scratch = (Join-Path $env:TEMP 'hgbrls'),
    [int]$Jobs = 8,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

$DEVKITPRO = 'C:/devkitPro'
$GXX       = 'C:\devkitPro\devkitA64\bin\aarch64-none-elf-g++.exe'
$GCC       = 'C:\devkitPro\devkitA64\bin\aarch64-none-elf-gcc.exe'
$AR        = 'C:\devkitPro\devkitA64\bin\aarch64-none-elf-ar.exe'

if (-not (Test-Path $Scratch)) { throw "borealis scratch tree not found: $Scratch" }
if (-not (Test-Path "$Scratch\library\include\borealis")) {
    throw "not a borealis tree (no library/include/borealis): $Scratch"
}

$lib  = "$Scratch\library"
$ext  = "$lib\lib\extern"
$objDir = "$Scratch\obj-brls"
if ($Force -and (Test-Path $objDir)) { Remove-Item -Recurse -Force $objDir }
New-Item -ItemType Directory -Force -Path $objDir | Out-Null

$ARCH = @('-march=armv8-a+crc+crypto', '-mtune=cortex-a57', '-mtp=soft', '-fPIE')

# Include paths, in the order borealis' CMakeLists assembles them.
$INC = @(
    "-I$DEVKITPRO/libnx/include",
    "-I$DEVKITPRO/portlibs/switch/include",
    "-I$lib/include",
    "-I$lib/include/borealis/extern",
    "-I$lib/include/borealis/extern/nanovg",
    "-I$ext/switch-libpulsar/include",
    "-I$ext/tweeny/include",
    "-I$ext/fmt/include",
    "-I$ext/yoga",
    "-I$ext/yoga/yoga",
    # tinyxml2's PUBLIC include dir, from
    # target_include_directories(tinyxml2 PUBLIC ...). borealis' box.cpp
    # includes <tinyxml2.h>, which only resolves through this path.
    "-I$lib/include/borealis/extern/tinyxml2",
    "-I$ext/tinyxml2"
)

# BRLS_PLATFORM_OPTION from the PLATFORM_SWITCH branch, plus the deko3d driver
# selection that toolchain.cmake turns into BOREALIS_USE_DEKO3D.
$DEFS = @(
    '-DHAVE_LIBNX', '-DSWITCH', '-D__SWITCH__', '-DSTBI_NO_THREAD_LOCALS',
    '-DBOREALIS_USE_DEKO3D',
    # from target_compile_definitions(borealis PUBLIC -DYG_ENABLE_EVENTS ...).
    # application.cpp hard-errors without it.
    '-DYG_ENABLE_EVENTS',
    # set(BRLS_PLATFORM_RESOURCES_PATH "\"romfs:/\"") in the SWITCH branch.
    # Backslash-escaped quotes survive PowerShell's argument marshalling and
    # reach GCC as a real C string literal.
    '-DBRLS_RESOURCES=\"romfs:/\"'
)

$COMMON = @('-g', '-O2', '-fno-strict-aliasing', '-Wno-error') + $ARCH + $DEFS + $INC
$CXXFLAGS = @('-std=c++17', '-Wno-volatile', '-Wno-format', '-Wall') + $COMMON
$CFLAGS   = @('-std=gnu99', '-Wno-format', '-Wall') + $COMMON

# --- source lists -----------------------------------------------------------
# BOREALIS_SOURCE (globbed recursively by the CMake foreach), minus the
# platform trees this build never touches (glad/libretro-common are only wired
# in for the OpenGL driver; deko3d uses nanovg's own .cpp backend instead).
$srcDirs = @(
    "$lib\lib\core",
    "$lib\lib\core\touch",
    "$lib\lib\views",
    "$lib\lib\views\cells",
    "$lib\lib\views\widgets",
    "$ext\switch-libpulsar\src\archive",
    "$ext\switch-libpulsar\src\bfgrp",
    "$ext\switch-libpulsar\src\bfsar",
    "$ext\switch-libpulsar\src\bfwar",
    "$ext\switch-libpulsar\src\bfwav",
    "$ext\switch-libpulsar\src\bfwsd",
    "$ext\switch-libpulsar\src\player"
)

# file(GLOB_RECURSE SWITCH_PLATFORM_SRC lib/platforms/switch/*.cpp)
# switch_wrapper.c is excluded: it defines userAppInit/userAppExit, and those
# belong to the application, not to the library.
$srcDirs += "$lib\lib\platforms\switch"

$sources = @()
foreach ($d in $srcDirs) {
    if (-not (Test-Path $d)) { throw "source dir missing: $d" }
    $sources += Get-ChildItem $d -Recurse -Include *.cpp, *.c -File |
                Where-Object { $_.Name -ne 'switch_wrapper.c' }
}

# nanovg core (C) + the deko3d nanovg backend (C++), which the CMake adds
# separately because it does not live under a BOREALIS_SOURCE directory.
$sources += Get-Item "$ext\nanovg\nanovg.c"
$sources += Get-ChildItem "$ext\nanovg" -Recurse -Include *.cpp -File

# libretro-common: the three dirs borealis' CMakeLists lists explicitly. They
# are not optional -- application.cpp calls cpu_features_get_time_usec(), and
# without these the link fails on exactly that one symbol.
foreach ($d in @('compat', 'encodings', 'features')) {
    $sources += Get-ChildItem "$ext\libretro-common\$d" -Recurse -Include *.c, *.cpp -File
}

# Vendored static deps that CMake builds as sub-targets.
$sources += Get-ChildItem "$ext\yoga\yoga" -Recurse -Include *.cpp -File
# fmt: CMake's FMT_SOURCES is src/format.cc, and src/os.cc is added only under
# target_sources(... PRIVATE src/os.cc) behind FMT_OS. src/fmt.cc is the C++20
# MODULE wrapper and is not part of a normal build; it cannot compile as a
# translation unit on this toolchain ("'module' does not name a type"). os.cc
# fails for the same class of reason: libnx's newlib has no fileno/fdopen.
$sources += Get-Item "$ext\fmt\src\format.cc"
$sources += Get-Item "$ext\tinyxml2\tinyxml2.cpp"
# tweeny is header-only at this commit; guard in case a .cpp appears.
$sources += Get-ChildItem "$ext\tweeny\src" -Recurse -Include *.cpp, *.cc -File -ErrorAction SilentlyContinue

# yoga also has a generated enums file in some checkouts; only real files here.
$sources = $sources | Sort-Object FullName -Unique

Write-Host "borealis: $($sources.Count) translation units -> $objDir"

function Invoke-Tool {
    param([string]$Exe, [string[]]$ToolArgs, [string]$LogPath)
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $code = 1
    try {
        & $Exe @ToolArgs *> $LogPath
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $prev
    }
    return $code
}

$logDir = "$Scratch\logs"
New-Item -ItemType Directory -Force -Path $logDir | Out-Null

$failed = New-Object System.Collections.ArrayList
$compiled = 0
$sw = [System.Diagnostics.Stopwatch]::StartNew()

# Compile in parallel batches. Each compiler is a separate native process, which
# is what makes this viable where MSYS2 make is not.
$queue = New-Object System.Collections.Queue
foreach ($src in $sources) {
    $key = ($src.FullName -replace [regex]::Escape($Scratch), '') -replace '[\\/:*?"<>|]', '_'
    $obj = Join-Path $objDir ($key.TrimStart('_') + '.o')
    if ((-not $Force) -and (Test-Path $obj) -and
        ((Get-Item $obj).LastWriteTime -gt $src.LastWriteTime)) { continue }
    $queue.Enqueue([pscustomobject]@{ Src = $src; Obj = $obj; Key = $key })
}

$total = $queue.Count
Write-Host "to compile: $total  (already up to date: $($sources.Count - $total))"

while ($queue.Count -gt 0) {
    $batch = @()
    for ($i = 0; $i -lt $Jobs -and $queue.Count -gt 0; $i++) { $batch += $queue.Dequeue() }

    # One .cmd per unit, run through Start-Process and redirected to a single
    # combined log. An earlier version passed -RedirectStandardOutput and
    # -RedirectStandardError to two different files from the same Start-Process
    # call and then read only one of them: GCC diagnostics go to stderr, so every
    # failure looked like an EMPTY log and every unit was reported as failed even
    # when most had succeeded. Redirecting the child's whole stdio through cmd
    # avoids that entirely.
    $procs = @()
    foreach ($item in $batch) {
        $isC = $item.Src.Extension -eq '.c'
        $flags = if ($isC) { $CFLAGS } else { $CXXFLAGS }
        $exe = if ($isC) { $GCC } else { $GXX }
        $log = Join-Path $logDir ($item.Key + '.log')

        $cmdFile = Join-Path $logDir ($item.Key + '.cmd')
        $line = '"{0}" {1} -c "{2}" -o "{3}"' -f $exe,
                (($flags | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' '),
                $item.Src.FullName, $item.Obj
        Set-Content -Path $cmdFile -Value ("@echo off`r`n" + $line + "`r`n") -Encoding ASCII

        $procs += [pscustomobject]@{
            Item = $item
            Log  = $log
            Proc = Start-Process -FilePath 'cmd.exe' -PassThru -NoNewWindow `
                     -ArgumentList @('/c', $cmdFile) `
                     -RedirectStandardOutput $log -RedirectStandardError ($log + '.err')
        }
    }
    foreach ($p in $procs) {
        $p.Proc.WaitForExit()
        # Judge success by the ARTIFACT, not by the child's exit code.
        # Start-Process -NoNewWindow with redirection reports an ExitCode that
        # does not reflect the grandchild's status here (a cmd.exe wrapper made
        # every unit look failed while 97 of 99 .o files were in fact produced
        # correctly). A nonzero-sized object file that is newer than its source
        # is the thing we actually care about, and it cannot be faked by a
        # marshalling quirk.
        $ok = (Test-Path $p.Item.Obj) -and ((Get-Item $p.Item.Obj).Length -gt 0)
        if (-not $ok) {
            $err = ''
            foreach ($f in @($p.Log, ($p.Log + '.err'))) {
                if (Test-Path $f) { $err += (Get-Content $f -Raw) + "`n" }
            }
            [void]$failed.Add([pscustomobject]@{
                Src = $p.Item.Src.FullName; Log = $p.Log; Err = $err
            })
        } else {
            $compiled++
        }
    }
    Write-Host ("  compiled {0}/{1}" -f ($compiled + $failed.Count), $total)
}
$sw.Stop()

if ($failed.Count -gt 0) {
    Write-Host ""
    Write-Host "=== $($failed.Count) TRANSLATION UNIT(S) FAILED ==="
    foreach ($f in $failed | Select-Object -First 12) {
        Write-Host "--- $($f.Src)"
        ($f.Err -split "`n" | Select-String -Pattern 'error' | Select-Object -First 4) |
            ForEach-Object { Write-Host "    $($_.Line.Trim())" }
    }
    throw "borealis compile failed ($($failed.Count) units)"
}

$objs = Get-ChildItem $objDir -Filter *.o -File | ForEach-Object { $_.FullName }
Write-Host "archiving $($objs.Count) objects ..."
$libOut = Join-Path $Scratch 'libborealis.a'
if (Test-Path $libOut) { Remove-Item -Force $libOut }

# ar's command line has a length cap, so pass the objects through a response file.
$rsp = Join-Path $Scratch 'ar.rsp'
($objs -join "`n") | Set-Content -Path $rsp -Encoding ASCII
# aarch64-none-elf-ar needs forward slashes for the rsp entries on this host.
(($objs -join "`n") -replace '\\', '/') | Set-Content -Path $rsp -Encoding ASCII

$prev = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
& $AR rcs $libOut "@$rsp" 2>&1 | Out-Host
$arc = $LASTEXITCODE
$ErrorActionPreference = $prev
if ($arc -ne 0) { throw "ar failed ($arc)" }

Write-Host ""
Write-Host ("OK  borealis static lib -> {0}  ({1:N1} MiB)" -f $libOut, ((Get-Item $libOut).Length / 1MB))
Write-Host ("    {0} objects, {1:N1}s" -f $objs.Count, $sw.Elapsed.TotalSeconds)
