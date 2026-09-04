# Seed a PCSXROO portable data directory from an existing PCSX2 installation.
#
# PCSXROO runs in portable mode: a portable.ini beside the executable makes the build
# directory its data directory, so it never reads or writes the settings of the PCSX2
# installation you actually use. That isolation costs a one-time setup - without a BIOS the
# emulator opens its first-run wizard and blocks before the debug server ever starts.
#
# This copies BIOS images, memory cards and settings across. It is strictly read-only with
# respect to -From: nothing is ever written back to the source installation.
#
#   .\seed-portable.ps1
#   .\seed-portable.ps1 -From "D:\emu\PCSX2" -To "build\pcsx2-qt"
#   .\seed-portable.ps1 -Force        # overwrite files already present at the destination

[CmdletBinding()]
param(
    [string] $From,
    [string] $To,
    [switch] $Force
)

$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

if (-not $To) {
    $To = Join-Path $repo 'build\pcsx2-qt'
}

if (-not $From) {
    # Resolved at runtime rather than hardcoded, so this works on any machine.
    $candidates = @(
        (Join-Path $env:APPDATA 'EmuDeck\Emulators\PCSX2-Qt'),
        (Join-Path $env:USERPROFILE 'Documents\PCSX2'),
        (Join-Path $env:APPDATA 'PCSX2')
    )
    $From = $candidates | Where-Object { Test-Path (Join-Path $_ 'bios') } | Select-Object -First 1
}

if (-not (Test-Path $To)) {
    Write-Host "ERROR: destination '$To' does not exist. Build PCSXROO first."
    exit 1
}

# portable.ini is what switches PCSX2 to keeping its data beside the executable. Create it
# even when there is nothing to seed, since that is what keeps PCSXROO out of the real
# installation's settings.
$portable = Join-Path $To 'portable.ini'
if (-not (Test-Path $portable)) {
    New-Item -ItemType File -Path $portable | Out-Null
    Write-Host "created $portable"
} else {
    Write-Host "portable.ini already present"
}

if (-not $From -or -not (Test-Path $From)) {
    Write-Host "No PCSX2 installation found to seed from; set -From to seed BIOS and settings."
    Write-Host "PCSXROO will start with an empty data directory and open its setup wizard."
    exit 0
}

Write-Host "seeding from $From"
Write-Host "           to $To"

$copied = 0
$skipped = 0

foreach ($dir in @('bios', 'memcards', 'inis')) {
    $sourceDir = Join-Path $From $dir
    if (-not (Test-Path $sourceDir)) {
        Write-Host "  $dir : not present in source, skipped"
        continue
    }

    $destDir = Join-Path $To $dir
    if (-not (Test-Path $destDir)) {
        New-Item -ItemType Directory -Path $destDir | Out-Null
    }

    foreach ($file in Get-ChildItem $sourceDir -File) {
        $dest = Join-Path $destDir $file.Name
        if ((Test-Path $dest) -and -not $Force) {
            $skipped++
            continue
        }

        Copy-Item $file.FullName $dest -Force
        $copied++
    }

    Write-Host "  $dir : done"
}

Write-Host "copied $copied file(s), skipped $skipped already present (use -Force to overwrite)"
