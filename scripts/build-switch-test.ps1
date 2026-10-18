# Build the on-device verification NRO for the hongguo crypto/media stack.
#
# Works around two environment limits, both already established in this project:
#   1. aarch64-none-elf-gcc mangles non-ASCII arguments and the workspace lives
#      under a non-ASCII path, so all compiler work happens in an ASCII scratch
#      directory under $env:TEMP and only the artifacts are copied back.
#   2. msys2 make cannot run here (sandbox blocks the named pipes it needs), so
#      compile/link/elf2nro are driven directly rather than via a Makefile.
#
# ASCII-only file: PowerShell 5.1 parses BOM-less .ps1 with the system ANSI
# codepage (GBK/936), which corrupts non-ASCII source text.
#
# Usage: powershell -File build-switch-test.ps1 -AppSrc <dir> -MainC <file> -OutNro <file>

param(
    [Parameter(Mandatory = $true)][string]$AppSrc,
    [Parameter(Mandatory = $true)][string]$MainC,
    [Parameter(Mandatory = $true)][string]$OutNro,
    # Links libcurl (and its libnx/zlib deps). Needed by anything that actually
    # talks to the network. CURL_STATICLIB is mandatory, not optional: without it
    # the curl headers decorate every symbol with dllimport and the link fails.
    [switch]$WithCurl,
    # Links libmpv and its whole private dependency chain. That chain comes from
    # mpv.pc's Libs.private and is long because this mpv is built with libplacebo,
    # libass, dav1d, deko3d and harfbuzz.
    [switch]$WithMpv
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

$DEVKITPRO = 'C:/devkitPro'
$CC        = 'C:\devkitPro\devkitA64\bin\aarch64-none-elf-gcc.exe'
$ELF2NRO   = 'C:\devkitPro\tools\bin\elf2nro.exe'
$NACPTOOL  = 'C:\devkitPro\tools\bin\nacptool.exe'

if (-not (Test-Path $AppSrc)) { throw "AppSrc not found: $AppSrc" }
if (-not (Test-Path $MainC))  { throw "MainC not found: $MainC" }
$AppSrc = (Resolve-Path $AppSrc).Path
$MainC  = (Resolve-Path $MainC).Path

$scratch = Join-Path $env:TEMP 'hgswitch'
if (Test-Path $scratch) { Remove-Item -Recurse -Force $scratch }
New-Item -ItemType Directory -Force -Path $scratch | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $scratch 'obj') | Out-Null
Write-Host "scratch: $scratch"

# Flat copy: the sources use quoted includes among themselves, and the generated
# vector headers live in tests/ upstream, so flattening keeps -I simple.
Copy-Item (Join-Path $AppSrc '*.c') $scratch -Force
Copy-Item (Join-Path $AppSrc '*.h') $scratch -Force
Copy-Item (Join-Path $AppSrc 'tests\*.h') $scratch -Force
Copy-Item $MainC (Join-Path $scratch 'switch_main.c') -Force

$ARCH = @('-march=armv8-a+crc+crypto', '-mtune=cortex-a57', '-mtp=soft', '-fPIE')
$CFLAGS = @('-g', '-O2', '-Wall', '-ffunction-sections', '-fdata-sections',
            '-fno-strict-aliasing', '-std=c99') + $ARCH +
          @('-D__SWITCH__', '-D_GNU_SOURCE',
            "-I$DEVKITPRO/libnx/include", "-I$DEVKITPRO/portlibs/switch/include", "-I$scratch")
if ($WithCurl) { $CFLAGS += '-DCURL_STATICLIB' }

$sources = @(Get-ChildItem -Path $scratch -Filter *.c -File)
if ($sources.Count -eq 0) { throw "no .c files staged in $scratch" }

$objs = @()
$failed = 0
foreach ($f in $sources) {
    $o = Join-Path $scratch ('obj\' + $f.BaseName + '.o')
    Write-Host "CC  $($f.Name)"
    $rc = Invoke-Tool -Exe $CC -ToolArgs ($CFLAGS + @('-c', $f.FullName, '-o', $o))
    if ($rc -ne 0) { Write-Host "  FAILED $($f.Name)"; $failed = 1; continue }
    $objs += $o
}
if ($failed) { throw "compile failed" }

$elf = Join-Path $scratch 'hgswitch.elf'
$LDFLAGS = @("-specs=$DEVKITPRO/libnx/switch.specs", '-g') + $ARCH +
           @("-L$DEVKITPRO/libnx/lib", "-L$DEVKITPRO/portlibs/switch/lib",
             '-Wl,--gc-sections')
Write-Host 'LD  hgswitch.elf'
$ldLibs = @('-lnx', '-lm')
if ($WithCurl) { $ldLibs += @('-lcurl', '-lnx', '-lz') }
if ($WithMpv) {
    # mpv.pc Libs.private, in the order the pc file gives it.
    $ldLibs += @('-lmpv', '-ldeko3d', '-luam', '-lavfilter', '-lswscale', '-lavformat',
                 '-lmbedtls', '-lmbedx509', '-lmbedcrypto', '-lavcodec', '-ldav1d',
                 '-lswresample', '-lavutil', '-lass', '-lharfbuzz', '-lfribidi',
                 '-lfreetype', '-lbz2', '-lpng16', '-lnx', '-lplacebo', '-llua5.1',
                 '-lm', '-lz')
    # libplacebo and libuam are C++, so the link needs the C++ runtime even
    # though everything in this project is C: without it the link reports
    # undefined std::to_chars / std::from_chars and __gxx_personality_v0.
    $ldLibs += @('-lstdc++')
}
$rc = Invoke-Tool -Exe $CC -ToolArgs ($LDFLAGS + $objs + $ldLibs + @('-o', $elf))
if ($rc -ne 0) { throw "link failed" }
Write-Host "  elf: $((Get-Item $elf).Length) bytes"

$nacp = Join-Path $scratch 'hgswitch.nacp'
$rc = Invoke-Tool -Exe $NACPTOOL -ToolArgs @('--create', 'hongguo-test', 'dongrez1990-cloud', '1.0.0', $nacp)
if ($rc -ne 0) { throw "nacptool failed" }

$nro = Join-Path $scratch 'hgswitch.nro'
$rc = Invoke-Tool -Exe $ELF2NRO -ToolArgs @($elf, $nro, "--nacp=$nacp")
if ($rc -ne 0) { throw "elf2nro failed" }

$dest = $OutNro
$destDir = Split-Path $dest -Parent
if ($destDir -and -not (Test-Path $destDir)) { New-Item -ItemType Directory -Force -Path $destDir | Out-Null }
Copy-Item -Force $nro $dest

# Also keep the ELF next to the NRO. NSP packaging needs it: the NRO this script
# produces is a *bare* homebrew NRO (NRO0 header, then segments -- no PFS0
# container and no NSO0 block), so `exefs/main` cannot be derived from it and
# has to come from the ELF. The ELF only exists in the temp scratch dir, which
# is cleaned on exit, so without this copy `build-nsp.ps1` breaks as soon as the
# scratch is gone.
$elfDest = [System.IO.Path]::ChangeExtension($dest, '.elf')
Copy-Item -Force $elf $elfDest

$b = [System.IO.File]::ReadAllBytes($dest)
$magic16 = [System.Text.Encoding]::ASCII.GetString($b[16..19])
Write-Host ''
Write-Host "OK  -> $dest  ($([math]::Round((Get-Item $dest).Length/1KB,1)) KiB)"
Write-Host "    NRO0 magic at offset 16: '$magic16'"
if ($magic16 -ne 'NRO0') { throw "not a valid NRO" }
