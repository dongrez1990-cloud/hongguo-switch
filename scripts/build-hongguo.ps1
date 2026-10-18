# Build the hongguo application NRO: borealis UI + the verified C data layer.
#
# WHY NOT CMake, like build-borealis.ps1: the MSYS2 CMake/make/ninja binaries
# cannot start under this sandbox at all ("couldn't create signal pipe, Win32
# error 5"), and no native Windows CMake is installed. So the link is driven
# directly with aarch64-none-elf-g++, exactly as build-switch-test.ps1 already
# does for the pure-C test NROs. This keeps the project's existing property: all
# compiler work happens in an ASCII scratch directory, because the toolchain
# mangles non-ASCII arguments and the workspace path is non-ASCII.
#
# ASCII-only file.
#
# Usage: powershell -File build-hongguo.ps1 [-OutNro <file>] [-SmokeTest] [-Diag]
#
#   (default)    the shipping app: main.cpp
#   -Diag        diag_main.cpp: the staged boot entry point. Same startup
#                sequence (hg_boot.cpp), then stage A (bare label, 60 frames)
#                and stage B (the real HomeActivity). One console run then says
#                which stage dies. See app/hongguo/diag_main.cpp.
#   -SmokeTest   smoke_main.cpp: minimal borealis boot, no app code.

param(
    [string]$OutNro,
    [switch]$SmokeTest,
    [switch]$Diag,
    [switch]$WithMpv = $true
)

$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
if (-not $OutNro) {
    $name = if ($Diag) { 'hongguo-diag.nro' } else { 'hongguo.nro' }
    $OutNro = Join-Path $repo "out\$name"
}

$DEVKITPRO = 'C:/devkitPro'
$CXX       = 'C:\devkitPro\devkitA64\bin\aarch64-none-elf-g++.exe'
$GCC       = 'C:\devkitPro\devkitA64\bin\aarch64-none-elf-gcc.exe'
$ELF2NRO   = 'C:\devkitPro\tools\bin\elf2nro.exe'
$NACPTOOL  = 'C:\devkitPro\tools\bin\nacptool.exe'

$scratch = Join-Path $env:TEMP 'hgbrls'

# Self-healing: libborealis.a used to live only in the session temp directory,
# so a new session found it gone and the build stopped with "run
# build-borealis.ps1 first". That cost ~2 minutes of diagnosis every time and
# was already recorded as a hazard in docs/交接.md. Close the loop here instead:
# if the library is absent, produce it. A full build is ~2 minutes; the
# alternative is a dead end.
if (-not (Test-Path "$scratch\libborealis.a")) {
    Write-Host "libborealis.a missing from $scratch -- staging and building borealis (~2 min)"
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'stage-borealis.ps1') -Scratch $scratch
    if ($LASTEXITCODE -ne 0) { throw "stage-borealis.ps1 failed ($LASTEXITCODE)" }
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'build-borealis.ps1') -Scratch $scratch -Jobs 8
    if ($LASTEXITCODE -ne 0) { throw "build-borealis.ps1 failed ($LASTEXITCODE)" }
}
if (-not (Test-Path "$scratch\libborealis.a")) {
    throw "libborealis.a still missing after an automatic build: $scratch"
}
$brlsLib = "$scratch\library"

$appSrc = Join-Path $repo 'app\hongguo'
$coreSrc = Join-Path $repo 'app\src'

# --- stage the app sources flat, next to borealis, under the ASCII scratch ---
# The object dir is wiped every run: build-hongguo.ps1 compiles a small number
# of files quickly, and a stale smoke_main.o from a previous -SmokeTest run
# once turned into a "multiple definition of main" error that ld reported
# against fmt/chrono.h, hiding the actual cause.
$stage = "$scratch\app"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force -Path "$stage\obj" | Out-Null

Copy-Item (Join-Path $coreSrc '*.c') $stage -Force
Copy-Item (Join-Path $coreSrc '*.h') $stage -Force
Copy-Item (Join-Path $coreSrc 'tests\*.h') $stage -Force -ErrorAction SilentlyContinue
Copy-Item (Join-Path $appSrc '*.cpp') $stage -Force -ErrorAction SilentlyContinue
Copy-Item (Join-Path $appSrc '*.hpp') $stage -Force -ErrorAction SilentlyContinue

# --- NOT vendoring wiliwili's AutoTabFrame, and the reason is measured -----------------
# wiliwili's left sidebar is `AutoTabFrame`, which is NOT in the borealis fork we build (that tree
# has only tab_frame.hpp). The obvious move is to copy it in, so that was tried: it pulls in
# `view/svg_image.hpp`, which includes <lunasvg.h>, and lunasvg is NOT in this devkitPro
# (C:\devkitPro\portlibs\switch has no lunasvg.h). Its icon model is SVG, so it cannot be adopted
# without adding that library to the toolchain.
#
# The sidebar is therefore written in our own code using borealis primitives we already ship --
# a Box of focusable icon+label rows. The thing the user asked for is the STRUCTURE and the
# behaviour (left column, one item per section, bottom group for inbox/settings, touch and pad),
# and that is reproducible without an SVG rasteriser. See docs/wiliwili界面结构复刻规格.md.

# borealis' Switch bootstrap. It defines userAppInit/userAppExit, which the
# platform calls: service init (socket with raised BSD session count, nifm,
# romfs, pl, setsys, lbl) plus the appletLockExit pair. build-borealis.ps1
# deliberately excludes it from the static library because it belongs to the
# application, not the library.
Copy-Item (Join-Path $brlsLib 'lib\platforms\switch\switch_wrapper.c') $stage -Force

$ARCH = @('-march=armv8-a+crc+crypto', '-mtune=cortex-a57', '-mtp=soft', '-fPIE')

$INC = @(
    "-I$DEVKITPRO/libnx/include",
    "-I$DEVKITPRO/portlibs/switch/include",
    "-I$brlsLib/include",
    "-I$brlsLib/include/borealis/extern",
    "-I$brlsLib/include/borealis/extern/nanovg",
    "-I$brlsLib/lib/extern/switch-libpulsar/include",
    "-I$brlsLib/lib/extern/tweeny/include",
    "-I$brlsLib/lib/extern/fmt/include",
    "-I$brlsLib/lib/extern/yoga",
    "-I$brlsLib/lib/extern/yoga/yoga",
    "-I$brlsLib/include/borealis/extern/tinyxml2",
    "-I$stage"
)

$DEFS = @(
    '-DHAVE_LIBNX', '-DSWITCH', '-D__SWITCH__', '-DSTBI_NO_THREAD_LOCALS',
    '-DBOREALIS_USE_DEKO3D', '-DYG_ENABLE_EVENTS',
    '-DBRLS_RESOURCES=\"romfs:/\"',
    '-DCURL_STATICLIB',
    '-D_GNU_SOURCE'
)

$COMMON = @('-g', '-O2', '-fno-strict-aliasing', '-Wno-error') + $ARCH + $DEFS + $INC
$CXXFLAGS = @('-std=c++17', '-Wno-volatile', '-Wno-format', '-Wall') + $COMMON
$CFLAGS   = @('-std=c99', '-Wno-format', '-Wall') + $COMMON

function Invoke-Tool {
    param([string]$Exe, [string[]]$ToolArgs)
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $code = 1
    try { & $Exe @ToolArgs 2>&1 | Out-Host; $code = $LASTEXITCODE }
    finally { $ErrorActionPreference = $prev }
    return $code
}

# --- compile the C core and the C++ app -------------------------------------
$sources = @()
$sources += Get-ChildItem $stage -Filter *.c -File
$sources += Get-ChildItem $stage -Filter *.cpp -File
if ($SmokeTest) {
    # The smoke test supplies its own main(); drop the real one.
    $sources = $sources | Where-Object { $_.Name -ne 'main.cpp' }
    $sources = $sources | Where-Object {
        $_.Name -in @('smoke_main.cpp') -or $_.Extension -eq '.c' -or $_.Name -eq 'hg_client.cpp'
    }
} else {
    # Likewise drop the smoke test from a real build. Both define main(), and a
    # leftover smoke_main.o in the object dir produced a "multiple definition
    # of main" link error that pointed at fmt/chrono.h and hid the real cause.
    # Exactly ONE entry point is linked. There are now three of them
    # (main.cpp, diag_main.cpp, smoke_main.cpp); linking two is a "multiple
    # definition of main" error that ld once reported against fmt/chrono.h,
    # hiding the cause. So the choice is explicit rather than by exclusion.
    $entry = if ($Diag) { 'diag_main.cpp' } else { 'main.cpp' }
    $sources = $sources | Where-Object {
        $_.Name -notin @('main.cpp', 'diag_main.cpp', 'smoke_main.cpp') -or $_.Name -eq $entry
    }
}

Write-Host "hongguo: compiling $($sources.Count) units"
$failed = @()
foreach ($f in $sources) {
    $isC = $f.Extension -eq '.c'
    $flags = if ($isC) { $CFLAGS } else { $CXXFLAGS }
    $exe = if ($isC) { $GCC } else { $CXX }
    # THE OBJECT NAME KEEPS THE EXTENSION, AND THAT IS A FIX RATHER THAN A STYLE CHOICE.
    #
    # MEASURED (round 28): `app/src/hg_index.c` and `app/hongguo/hg_index.cpp` both staged as `hg_index.*` in one
    # directory, and `$f.BaseName + '.o'` gave them the SAME object path -- so the C++ compile overwrote the C
    # one, the C symbols vanished, and the failure appeared at LINK time as `undefined reference to
    # hg_index_append`. The compile step reported no error at all, which is exactly the kind of silent
    # clobber this project tries to make impossible.
    #
    # Keeping the extension makes the two names distinct (`hg_index.c.o` / `hg_index.cpp.o`) and costs nothing.
    $o = Join-Path "$stage\obj" ($f.Name + '.o')
    Write-Host "  CC $($f.Name)"
    $rc = Invoke-Tool -Exe $exe -ToolArgs ($flags + @('-c', $f.FullName, '-o', $o))
    if ($rc -ne 0 -or -not (Test-Path $o)) { $failed += $f.Name }
}
if ($failed.Count -gt 0) { throw "app compile failed: $($failed -join ', ')" }

$objs = Get-ChildItem "$stage\obj" -Filter *.o -File | ForEach-Object { $_.FullName }

# --- link -------------------------------------------------------------------
$elf = "$stage\hongguo.elf"
$LDFLAGS = @("-specs=$DEVKITPRO/libnx/switch.specs", '-g') + $ARCH +
           @("-L$DEVKITPRO/libnx/lib", "-L$DEVKITPRO/portlibs/switch/lib",
             '-Wl,--gc-sections', "-L$scratch")

# Order matters: borealis first (it pulls in fmt/tweeny/yoga/nanovg), then mpv
# and its private chain, then curl, then the platform libs. tweeny and yoga are
# header-only / compiled INTO libborealis.a by build-borealis.ps1, so there is
# no separate -ltweeny or -lyoga to find.
$libs = @('-lborealis')

if ($WithMpv) {
    # mpv.pc Libs.private, in the order that file gives, as build-switch-test.ps1
    # uses. libplacebo and libuam are C++, so -lstdc++ is required even though
    # the app itself is C++ here anyway.
    $libs += @('-lmpv', '-ldeko3d', '-luam', '-lavfilter', '-lswscale', '-lavformat',
               '-lmbedtls', '-lmbedx509', '-lmbedcrypto', '-lavcodec', '-ldav1d',
               '-lswresample', '-lavutil', '-lass', '-lharfbuzz', '-lfribidi',
               '-lfreetype', '-lbz2', '-lpng16', '-lnx', '-lplacebo', '-llua5.1',
               '-lm', '-lz')
} else {
    $libs += @('-ldeko3d')
}

$libs += @('-lcurl', '-lnx', '-lz', '-lm', '-lstdc++')

Write-Host 'LD  hongguo.elf'
$rc = Invoke-Tool -Exe $CXX -ToolArgs ($LDFLAGS + $objs + $libs + @('-o', $elf))
if ($rc -ne 0) { throw "link failed" }
Write-Host "  elf: $((Get-Item $elf).Length) bytes"

# --- romfs ------------------------------------------------------------------
#
# borealis loads its deko3d shaders by FILESYSTEM PATH:
#
#   dk_renderer.cpp:428  m_vertex_shader.load(m_code_mem_pool, "romfs:/shaders/fill_vsh.dksh");
#
# That is not libromfs (which embeds resources into the binary) -- it is a real
# path, so the NRO must carry a romfs partition containing shaders/. Without one
# the load fails, nanovg never gets a pipeline, and the app draws NOTHING while
# reporting no error at all: a black screen with a clean log.
#
# The three .dksh files are prebuilt in upstream/wiliwili/resources/shaders, so
# CMake's gen_dksh() step (which runs uam over nanovg's GLSL) does NOT need to be
# reproduced -- they only have to be copied in.
$romfs = "$stage\romfs"
if (Test-Path $romfs) { Remove-Item -Recurse -Force $romfs }
New-Item -ItemType Directory -Force -Path "$romfs\shaders" | Out-Null

$shaderSrc = Join-Path $repo 'upstream\wiliwili\resources\shaders'
$shaders = @(Get-ChildItem -Path $shaderSrc -Filter *.dksh -File -ErrorAction SilentlyContinue)
if ($shaders.Count -eq 0) { throw "no .dksh shaders found in $shaderSrc -- the UI cannot draw without them" }
foreach ($s in $shaders) { Copy-Item -Force $s.FullName "$romfs\shaders\" }
Write-Host "romfs: $($shaders.Count) shader(s) staged -> $romfs"

# --- package ----------------------------------------------------------------
$nacp = "$stage\hongguo.nacp"
$rc = Invoke-Tool -Exe $NACPTOOL -ToolArgs @('--create', 'hongguo', 'dongrez1990-cloud', '1.0.0', $nacp)
if ($rc -ne 0) { throw "nacptool failed" }

$nro = "$stage\hongguo.nro"
# --romfsdir, NOT --romfs. elf2nro distinguishes them:
#   --romfs=<image>        embeds a PREBUILT romfs image
#   --romfsdir=<directory> BUILDS a romfs from a directory and embeds it
# Passing a directory to --romfs fails with "Failed to open input romfs!".
$rc = Invoke-Tool -Exe $ELF2NRO -ToolArgs @($elf, $nro, "--nacp=$nacp", "--romfsdir=$romfs")
if ($rc -ne 0) { throw "elf2nro failed" }

$destDir = Split-Path $OutNro -Parent
if ($destDir -and -not (Test-Path $destDir)) { New-Item -ItemType Directory -Force -Path $destDir | Out-Null }
Copy-Item -Force $nro $OutNro
Copy-Item -Force $elf ([System.IO.Path]::ChangeExtension($OutNro, '.elf'))

$b = [System.IO.File]::ReadAllBytes($OutNro)
$magic = [System.Text.Encoding]::ASCII.GetString($b[16..19])
Write-Host ''
Write-Host "OK  -> $OutNro  ($([math]::Round((Get-Item $OutNro).Length/1MB,2)) MiB)"
Write-Host "    NRO0 magic at offset 16: '$magic'"
if ($magic -ne 'NRO0') { throw "not a valid NRO" }
