# Parse a PFS0 (Partition FS) archive header and list its entries.
#
# An NSP is a PFS0 container: a 16-byte header, then a fixed-size entry table,
# then the string table, then the file payloads. Note the order: hacBrewPack's
# pfs0.c writes the header, then the entry table, then the (0x20-aligned)
# string table -- which is the reverse of the more commonly documented
# header/string-table/entry-table layout. The string table size in the header
# is the PADDED size, so the entry table is at a fixed 16 and the payloads
# begin at 16 + num_files*24 + padded_string_table_size.
#
# Verification must read the real structure rather than trusting that
# hacbrewpack wrote a file.
#
# ASCII-only file: PowerShell 5.1 parses BOM-less .ps1 with the system ANSI
# codepage (GBK/936), which corrupts non-ASCII source text.
#
# Usage: powershell -File verify-nsp.ps1 -Nsp <file.nsp>

param(
    [Parameter(Mandatory = $true)][string]$Nsp
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $Nsp)) { throw "NSP not found: $Nsp" }
$Nsp = (Resolve-Path $Nsp).Path
$b = [System.IO.File]::ReadAllBytes($Nsp)

if ($b.Length -lt 16) { throw "file too small to be a PFS0 archive" }

$magic = [System.Text.Encoding]::ASCII.GetString($b[0..3])
# Cast to [int] immediately: ToUInt32 returns [uint32], and later arithmetic
# such as ($entryCount * 24) is done in unsigned space, which silently produces
# wrong array offsets instead of erroring.
$entryCount = [int][BitConverter]::ToUInt32($b, 4)
$stringTableSize = [int][BitConverter]::ToUInt32($b, 8)
# bytes 12..15 are reserved and must be zero

# hacBrewPack layout: header (16) | entry table | string table | payload
$entryTableOffset = 16
$stringTableOffset = $entryTableOffset + ($entryCount * 24)
$dataOffset = $stringTableOffset + $stringTableSize

Write-Host "file            : $Nsp"
Write-Host "size            : $($b.Length) bytes"
Write-Host "magic           : '$magic'"
Write-Host "entry count     : $entryCount"
Write-Host "string table sz : $stringTableSize"
Write-Host "data offset     : $dataOffset"
Write-Host ""

if ($magic -ne 'PFS0') { throw "bad magic '$magic' (expected PFS0) - not an NSP" }
if ($entryCount -eq 0) { throw "PFS0 archive has zero entries" }
if ($dataOffset -gt $b.Length) { throw "data offset $dataOffset is past end of file" }

Write-Host "entries:"
Write-Host ("  {0,-46} {1,12} {2,12} {3,10}" -f 'name', 'offset', 'size', 'in-bounds')
# Extract the NUL-terminated name for entry $i from the string table. The table
# is at $stringTableOffset and names are ASCII, so build the string byte by byte
# rather than slicing an array: PowerShell 5.1 mis-handles the mixed uint32/index
# arithmetic in a range expression and silently returns the wrong bytes.
function Get-Pfs0Name {
    # Extract the NUL-terminated name for entry $Index from the string table.
    # $nameOff is the low 32 bits of the entry's u64 string_table_offset.
    param([int]$Index, [byte[]]$Buf, [int]$EntryTableOffset, [int]$StringTableOffset, [int]$StringTableSize)
    $p = $EntryTableOffset + ($Index * 24)
    $nameOff = [BitConverter]::ToUInt32($Buf, [int]($p + 16))
    $si = [int]$StringTableOffset + [int]$nameOff
    $limit = [int]$StringTableOffset + [int]$StringTableSize
    $chars = New-Object System.Collections.Generic.List[byte]
    for ($k = $si; $k -lt $limit -and $Buf[$k] -ne 0; $k++) { $chars.Add($Buf[$k]) }
    if ($chars.Count -eq 0) { return '' }
    return [System.Text.Encoding]::ASCII.GetString($chars.ToArray())
}

$total = 0
$bad = @()
for ($i = 0; $i -lt $entryCount; $i++) {
    $p = $entryTableOffset + ($i * 24)
    # pfs0_file_entry_t is 24 bytes, three 64-bit fields. The order was
    # confirmed field-by-field against real hacbrewpack output; note that
    # "offset" comes FIRST, which is easy to get backwards:
    #   +0x00 uint64 offset               relative to the payload region
    #   +0x08 uint64 size                 payload byte length
    #   +0x10 uint64 string_table_offset  offset of the NUL-terminated name
    # Only the low 32 bits matter at these sizes; read them as two 32-bit halves
    # because PowerShell 5.1 picks the wrong BitConverter overload for an
    # untyped offset argument.
    $offLo   = [BitConverter]::ToUInt32($b, [int]$p)
    $offHi   = [BitConverter]::ToUInt32($b, [int]($p + 4))
    $offset  = ([uint64]$offHi * 4294967296) + [uint64]$offLo
    $size    = [BitConverter]::ToUInt32($b, [int]($p + 8))
    $nameOff = [BitConverter]::ToUInt32($b, [int]($p + 16))

    # name is a NUL-terminated string inside the string table
    $name = Get-Pfs0Name -Index $i -Buf $b -EntryTableOffset $entryTableOffset `
                         -StringTableOffset $stringTableOffset -StringTableSize $stringTableSize

    $absStart = $dataOffset + $offset
    $absEnd = $absStart + $size
    $inBounds = ($absStart -ge $dataOffset) -and ($absEnd -le $b.Length)
    if (-not $inBounds) { $bad += $name }

    Write-Host ("  {0,-46} {1,12} {2,12} {3,10}" -f $name, $offset, $size, $inBounds)
    $total += $size
}

Write-Host ""
Write-Host "sum of entry sizes : $total bytes"
Write-Host "payload region     : $($b.Length - $dataOffset) bytes"

# An NSP must carry at least one .nca, and homebrew NSPs carry a metadata
# (.cnmt.nca) plus program and control NCAs.
$names = @()
for ($i = 0; $i -lt $entryCount; $i++) {
    $names += Get-Pfs0Name -Index $i -Buf $b -EntryTableOffset $entryTableOffset `
                           -StringTableOffset $stringTableOffset -StringTableSize $stringTableSize
}

$ncaCount = @($names | Where-Object { $_ -like '*.nca' }).Count
$hasCnmt = @($names | Where-Object { $_ -like '*.cnmt.nca' }).Count -gt 0

Write-Host ""
if ($bad.Count -gt 0) { throw "entries out of bounds: $($bad -join ', ')" }
if ($ncaCount -lt 2) { throw "expected at least 2 .nca entries, found $ncaCount" }
if (-not $hasCnmt) { throw "no .cnmt.nca metadata entry found" }

Write-Host "OK  valid PFS0/NSP: $entryCount entries, $ncaCount .nca (cnmt present: $hasCnmt), magic PFS0"
