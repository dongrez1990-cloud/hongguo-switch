# Package a hongguo-switch NRO into an installable Nintendo Switch NSP.
#
# Pipeline (each step explained where the reason is not obvious):
#
#   NRO --(ELF)--> elf2nso --> exefs/main   (supply -Elf; see note below)
#   npdm.json --> exefs/main.npdm  (npdmtool)
#   control.nacp --> control/control.nacp
#   hacbrewpack --> <titleid>.nsp
#
# hacBrewPack expects this working-directory layout (from its template/README):
#
#   <work>/exefs/main          NSO, the program itself
#   <work>/exefs/main.npdm     NPDM, the process descriptor (META magic)
#   <work>/control/control.nacp  NACP metadata, renamed from <target>.nacp
#   <work>/romfs/...           optional; skipped here with --noromfs
#   <work>/logo/...            optional; skipped here with --nologo
#
# It writes <work>/hacbrewpack_nsp/<titleid>.nsp.
#
# ASCII-only file: PowerShell 5.1 parses BOM-less .ps1 with the system ANSI
# codepage (GBK/936), which corrupts non-ASCII source text. The workspace path
# IS non-ASCII, so it is received as a parameter and only ever passed through
# as a value -- never written into this file's text.
#
# The build happens in an ASCII scratch directory under the session temp dir
# because hacbrewpack and several devkitPro tools mishandle non-ASCII argv.
#
# Usage:
#   powershell -File build-nsp.ps1 -Nro <file.nro> -OutNsp <file.nsp> -Elf <file.elf>
#
# -Elf is what makes this reliable: hacBrewPack wants an NSO for exefs/main, and
# the NRO produced by scripts\build-switch-test.ps1 does NOT contain one (it is a
# bare NRO, not a PFS0 container), so the ELF is the only local source of an NSO.
# If the ELF from the build's scratch directory is gone, rebuild it with
# scripts\build-switch-test.ps1 (which leaves hgswitch.elf in $env:TEMP\hgswitch)
# or re-link the same objects.

param(
    [Parameter(Mandatory = $true)][string]$Nro,
    [Parameter(Mandatory = $true)][string]$OutNsp,
    # Skip the desktop icon (see the icon step below). The NSP still installs; it gets the console's generic one.
    [switch]$NoIcon,
    # ELF the NRO was linked from (e.g. hgswitch.elf in $env:TEMP\hgswitch).
    # Strongly recommended: hacBrewPack needs an NSO for exefs/main, and the
    # build script's NRO does not embed one, so without this the run can only
    # fail. Passed through elf2nso to produce exefs/main.
    [string]$Elf = '',
    # Optional pre-made .nacp (16 KiB). If omitted, nacptool generates one.
    [string]$Nacp = '',
    # Directory to embed as the title's RomFS. NOT optional in practice: this app
    # needs romfs:/shaders/*.dksh or it draws nothing at all (see the hacbrewpack
    # section below). Pass the staged romfs directory that a build produced, e.g.
    # the one build-hongguo.ps1 creates under its own scratch dir, or a directory
    # containing shaders/ copied from upstream/wiliwili/resources/shaders.
    [string]$RomfsDir = '',
    # Homebrew title ids must be lower-case hex in 0x0100000000000000 -
    # 0x0fffffffffffffff; the range below is the usual homebrew band.
    [string]$TitleId = '0101000000000000',
    [string]$TitleName = 'hongguo-test',
    [string]$TitleAuthor = 'dongrez1990-cloud',
    [string]$TitleVersion = '1.0.0'
)

$ErrorActionPreference = 'Stop'

$scriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectRoot = Split-Path -Parent $scriptDir
$HBP         = Join-Path $projectRoot 'third_party\hacbrewpack'
$HBP_EXE     = Join-Path $HBP 'hacbrewpack.exe'
$NPDM_JSON   = Join-Path $HBP 'npdm.json'

$ELF2NSO  = 'C:\devkitPro\tools\bin\elf2nso.exe'
$NPDMTOOL = 'C:\devkitPro\tools\bin\npdmtool.exe'
$NACPTOOL = 'C:\devkitPro\tools\bin\nacptool.exe'
$VERIFY   = Join-Path $scriptDir 'verify-nsp.ps1'

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

foreach ($f in @($HBP_EXE, $NPDM_JSON, $ELF2NSO, $NPDMTOOL, $NACPTOOL, $VERIFY)) {
    if (-not (Test-Path $f)) { throw "required file missing: $f" }
}
if (-not (Test-Path $Nro)) { throw "NRO not found: $Nro" }
$Nro = (Resolve-Path $Nro).Path

$TitleId = $TitleId.ToLower()
if ($TitleId -notmatch '^[0-9a-f]{16}$') {
    throw "TitleId must be 16 lowercase hex digits, got '$TitleId'"
}

# ASCII scratch. Prefer a private dir under $env:TEMP; the workspace itself is
# unusable for this because its path is non-ASCII.
$scratch = Join-Path $env:TEMP 'hg-nsp'
if (Test-Path $scratch) { Remove-Item -Recurse -Force $scratch }
New-Item -ItemType Directory -Force -Path $scratch | Out-Null
New-Item -ItemType Directory -Force -Path "$scratch\exefs" | Out-Null
New-Item -ItemType Directory -Force -Path "$scratch\control" | Out-Null
Write-Host "scratch: $scratch"

# ---------------------------------------------------------------------- icon
# THE DESKTOP ICON, AND WHY IT GOES *HERE*. hacBrewPack packages whatever is in control/ as the control romfs,
# and the console reads the icon from a file in it named icon_<Language>.dat -- "dat files are just renamed jpg
# files", 256x256, with NO exif data or the icon shows as a placeholder (hacbrewpack/README.md:40-48, the
# vendored copy of the tool this script runs). There is no command-line flag for it; the file IS the flag.
#
# The artwork is generated by scripts/make-icon.ps1 and is entirely our own drawing -- the handover's 3.5
# requires that, so no part of the upstream service's branding is redistributed here.
if (-not $NoIcon) {
    $iconSrc = Join-Path $projectRoot 'assets\icon-256.jpg'
    if (Test-Path $iconSrc) {
        $iconDst = "$scratch\control\icon_AmericanEnglish.dat"
        Copy-Item -LiteralPath $iconSrc -Destination $iconDst -Force
        $iconBytes = (Get-Item $iconDst).Length
        Write-Host "icon: control\icon_AmericanEnglish.dat  ($iconBytes bytes)"
        if ($iconBytes -lt 2000) {
            throw "the icon is suspiciously small ($iconBytes bytes); regenerate it with scripts\make-icon.ps1"
        }
    } else {
        Write-Host 'icon: assets\icon-256.jpg is missing -- the NSP will install with the console general icon'
    }
} else {
    Write-Host 'icon: -NoIcon -- packing without a desktop icon'
}

# ---------------------------------------------------------------- exefs/main
# hacBrewPack needs exefs/main to be an NSO ("NSO0"). Two sources are possible.
$mainOut = "$scratch\exefs\main"
$haveElf = ($Elf -ne '') -and (Test-Path $Elf)

if ($haveElf) {
    $Elf = (Resolve-Path $Elf).Path
    Write-Host "exefs/main: elf2nso from $Elf"
    $rc = Invoke-Tool -Exe $ELF2NSO -ToolArgs @($Elf, $mainOut)
    if ($rc -ne 0) { throw "elf2nso failed (exit $rc)" }
} else {
    # Fallback: recover the NSO from inside the NRO.
    #
    # CAUTION, verified empirically against out\hongguo-net-test.nro: this NRO
    # contains NO embedded NSO. elf2nro.exe produces a bare NRO whose header
    # (magic NRO0 at 0x10) is followed directly by the text/ro/data segments at
    # 0x20 -- there is no PFS0 asset container and no NSO0 block anywhere in the
    # file. So this branch cannot succeed for NROs built by
    # scripts\build-switch-test.ps1, and is retained only so the failure is
    # explicit rather than silent. Pass -Elf instead.
    Write-Host "exefs/main: no ELF supplied; probing the NRO for an embedded NSO"
    $nroBytes = [System.IO.File]::ReadAllBytes($Nro)
    $magic16 = [System.Text.Encoding]::ASCII.GetString($nroBytes[16..19])
    if ($magic16 -ne 'NRO0') { throw "not an NRO (magic at offset 16 is '$magic16')" }

    # Only a PFS0-style NRO (assets present) can carry an NSO, and even then the
    # NSO is appended after the PFS0. Search for the NSO0 magic instead of
    # assuming an offset.
    $nsoOff = -1
    for ($i = 0; $i -lt $nroBytes.Length - 4; $i++) {
        if ($nroBytes[$i] -eq 0x4E -and $nroBytes[$i+1] -eq 0x53 -and
            $nroBytes[$i+2] -eq 0x4F -and $nroBytes[$i+3] -eq 0x30) { $nsoOff = $i; break }
    }
    if ($nsoOff -lt 0) {
        throw ("this NRO contains no embedded NSO (no NSO0 magic found); " +
               "it is a bare NRO with segments at 0x20, not a PFS0 container. " +
               "Re-run with -Elf <hgswitch.elf> to supply the ELF that the NRO " +
               "was built from, so elf2nso can produce exefs/main.")
    }
    $nsoLen = $nroBytes.Length - $nsoOff
    $out = New-Object byte[] $nsoLen
    [Array]::Copy($nroBytes, $nsoOff, $out, 0, $nsoLen)
    [System.IO.File]::WriteAllBytes($mainOut, $out)
    Write-Host "  extracted $nsoLen-byte NSO from NRO offset 0x$($nsoOff.ToString('x'))"
}
$mb = [System.IO.File]::ReadAllBytes($mainOut)
if ([System.Text.Encoding]::ASCII.GetString($mb[0..3]) -ne 'NSO0') {
    throw "exefs/main is not an NSO"
}

# ------------------------------------------------------------- exefs/main.npdm
# The NPDM carries the process descriptor, including the title id. It is built
# from npdm.json by devkitPro's npdmtool. Note that npdmtool's expected schema
# is NEWER than hacBrewPack's shipped template/npdm.json: it requires
# program_id / program_id_range_min / program_id_range_max / version /
# system_resource_size and friends, and rejects debug_flags containing more than
# one of allow_debug/force_debug/force_debug_prod. Our npdm.json satisfies it.
Copy-Item $NPDM_JSON "$scratch\npdm.json" -Force
Write-Host "exefs/main.npdm: npdmtool from $NPDM_JSON"
$rc = Invoke-Tool -Exe $NPDMTOOL -ToolArgs @("$scratch\npdm.json", "$scratch\exefs\main.npdm")
if ($rc -ne 0) { throw "npdmtool failed (exit $rc)" }
$nb = [System.IO.File]::ReadAllBytes("$scratch\exefs\main.npdm")
if ([System.Text.Encoding]::ASCII.GetString($nb[0..3]) -ne 'META') {
    throw "main.npdm is not an NPDM (magic '$([System.Text.Encoding]::ASCII.GetString($nb[0..3]))')"
}

# ------------------------------------------------------- control/control.nacp
$nacpDst = "$scratch\control\control.nacp"
if ($Nacp -ne '' -and (Test-Path $Nacp)) {
    $Nacp = (Resolve-Path $Nacp).Path
    Write-Host "control/control.nacp: using supplied $Nacp"
    Copy-Item $Nacp $nacpDst -Force
} else {
    Write-Host "control/control.nacp: nacptool --create"
    $rc = Invoke-Tool -Exe $NACPTOOL -ToolArgs @('--create', $TitleName, $TitleAuthor, $TitleVersion, $nacpDst)
    if ($rc -ne 0) { throw "nacptool failed (exit $rc)" }
}
$cb = [System.IO.File]::ReadAllBytes($nacpDst)
if ($cb.Length -ne 16384) { throw "control.nacp must be 16384 bytes, got $($cb.Length)" }

# --------------------------------------------------------------- hacbrewpack
# --nologo always: we ship no Nintendo logo assets, and hacBrewPack would
# otherwise try to read logo/ and fail.
# --titleid overrides any title id baked into the NPDM/NACP so the three agree.
#
# ROMFS IS NOT OPTIONAL FOR THIS APP. The original version of this script passed
# --noromfs unconditionally, on the reasoning that the NSP needs no resources.
# That was wrong, and the console proved it: the installed title showed a grey
# screen and exited, while the same code run from its NRO worked.
#
# borealis loads its deko3d shaders by FILESYSTEM PATH --
#   dk_renderer.cpp  m_vertex_shader.load(..., "romfs:/shaders/fill_vsh.dksh")
# -- so without a romfs partition there are no shaders, and nothing can be drawn
# at all, silently. build-hongguo.ps1 already documents exactly this for the NRO
# (scripts\build-hongguo.ps1:200-220) and stages the three prebuilt .dksh files
# for it; the NSP build simply never did the same.
#
# So -RomfsDir is now the normal case: pass the staged romfs directory and this
# script copies it to <work>/romfs, where hacBrewPack looks for it.
if ($RomfsDir -eq '') {
    # SELF-HEAL RATHER THAN WARN. Leaving this to the caller is exactly how the
    # romfs-less NSP got built in the first place. The shaders are the same three
    # prebuilt .dksh files build-hongguo.ps1 copies, so they are staged here from
    # the same source; the run then cannot silently produce a title that draws
    # nothing.
    $shaderSrc = Join-Path $projectRoot 'upstream\wiliwili\resources\shaders'
    $shaders = @(Get-ChildItem -Path $shaderSrc -Filter *.dksh -File -ErrorAction SilentlyContinue)
    if ($shaders.Count -eq 0) {
        throw ("no .dksh shaders in $shaderSrc and no -RomfsDir given. Without romfs:/shaders " +
               "the installed title draws NOTHING. Pass -RomfsDir explicitly if the shaders " +
               "live somewhere else.")
    }
    New-Item -ItemType Directory -Force -Path "$scratch\romfs\shaders" | Out-Null
    foreach ($s in $shaders) { Copy-Item -Force $s.FullName "$scratch\romfs\shaders\" }
    Write-Host "romfs: staged $($shaders.Count) shader(s) from $shaderSrc"
} else {
    if (-not (Test-Path $RomfsDir)) { throw "-RomfsDir not found: $RomfsDir" }
    New-Item -ItemType Directory -Force -Path "$scratch\romfs" | Out-Null
    Copy-Item (Join-Path $RomfsDir '*') "$scratch\romfs" -Recurse -Force
    $staged = @(Get-ChildItem "$scratch\romfs" -Recurse -File)
    Write-Host "romfs: $($staged.Count) file(s) staged from $RomfsDir"
    if ($staged.Count -eq 0) { throw "-RomfsDir $RomfsDir is empty; the UI would draw nothing" }
}

Write-Host ''
Write-Host '=== hacbrewpack ==='
$hbpArgs = @('--nologo', '--titleid', $TitleId)
Push-Location $scratch
try {
    $rc = Invoke-Tool -Exe $HBP_EXE -ToolArgs $hbpArgs
} finally {
    Pop-Location
}
if ($rc -ne 0) { throw "hacbrewpack failed (exit $rc)" }

$produced = Join-Path $scratch "hacbrewpack_nsp\$TitleId.nsp"
if (-not (Test-Path $produced)) { throw "hacbrewpack did not produce $produced" }

# ------------------------------------------------------------------- collect
$destDir = Split-Path -Parent $OutNsp
if ($destDir -and -not (Test-Path $destDir)) { New-Item -ItemType Directory -Force -Path $destDir | Out-Null }
Copy-Item -Force $produced $OutNsp

# -------------------------------------------------------------------- verify
Write-Host ''
Write-Host '=== verify ==='
$rc = Invoke-Tool -Exe 'powershell' -ToolArgs @('-ExecutionPolicy', 'Bypass', '-File', $VERIFY, '-Nsp', $OutNsp)
if ($rc -ne 0) { throw "NSP structural verification failed (exit $rc)" }

Write-Host ''
Write-Host "OK  -> $OutNsp ($([math]::Round((Get-Item $OutNsp).Length/1KB,1)) KiB, title id $TitleId)"
