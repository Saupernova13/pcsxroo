# Seed a PCSXROO portable data directory from an existing PCSX2 installation.
#
# PCSXROO runs in portable mode: a portable.ini beside the executable makes the install
# directory its data directory, so it never reads or writes the settings of the PCSX2
# installation you actually use. That isolation costs a one-time setup - without a BIOS the
# emulator opens its first-run wizard and blocks before the debug server ever starts.
#
# This copies BIOS images, memory cards and settings across, then sanitises the copied
# settings. It is strictly read-only with respect to -From: nothing is ever written back to
# the source installation.
#
#   .\seed-portable.ps1
#   .\seed-portable.ps1 -From "D:\emu\PCSX2" -To "bin"
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
    $To = Join-Path $repo 'bin'
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
    Write-Host "ERROR: destination '$To' does not exist. Build PCSXROO first (tools\pcsxroo\build.cmd)."
    exit 1
}

# portable.ini is what switches PCSX2 to keeping its data beside the executable. Created
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
    Write-Host "No PCSX2 installation found to seed from; pass -From to seed BIOS and settings."
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

# --- sanitise the copied settings -------------------------------------------------------
#
# A settings file copied from a working installation carries things that are wrong, unsafe,
# or actively fatal for an unattended PCSXROO. Each rule below is here because it stopped
# the emulator from reaching the debug server, or because it copies something private.

$ini = Join-Path $To 'inis\PCSX2.ini'
if (-not (Test-Path $ini)) {
    Write-Host "no PCSX2.ini to sanitise"
    exit 0
}

$lines = Get-Content $ini
$out = @()
$section = ''
$notes = @()
$sawLanguage = $false

foreach ($line in $lines) {
    if ($line -match '^\[(.+)\]$') {
        $section = $Matches[1]
        $out += $line

        continue
    }

    # The source install's RetroAchievements login travels with the file. Never copy a
    # credential into a second location just because it was convenient.
    if ($section -eq 'Achievements' -and $line -match '^(Username|Token|LoginToken) = ') {
        $notes += 'removed RetroAchievements credentials'
        continue
    }

    # Achievements log in over the network during CPU thread startup, before the debug
    # server exists, and hardcore mode would refuse the debug server outright.
    if ($section -eq 'Achievements' -and $line -match '^Enabled = true') {
        $notes += 'disabled achievements'
        $out += 'Enabled = false'
        continue
    }

    # A recursive scan of the source install's ROM directory runs at startup and is the
    # slowest thing PCSXROO would ever do for no benefit: it boots by explicit path.
    if ($section -eq 'GameList') {
        if ($line.Trim() -ne '') { $notes += 'cleared the game list search paths' }
        continue
    }

    # Directory overrides point back at the installation we copied from, so PCSXROO would
    # read its BIOS from there - or, when the relative path does not resolve, from nowhere.
    if ($section -eq 'Folders' -and $line -match '^\w+ = ') {
        $notes += 'cleared directory overrides so PCSXROO uses its own folders'
        continue
    }

    # Starting fullscreen makes an unattended session fight for the display.
    if ($section -eq 'UI' -and $line -match '^StartFullscreen = true') {
        $notes += 'disabled start-fullscreen'
        $out += 'StartFullscreen = false'
        continue
    }

    if ($section -eq 'UI' -and $line -match '^Language = ') {
        $sawLanguage = $true
    }

    $out += $line
}

# A Devel build shows a modal "Translation Error" box for any system locale with no
# shipped .qm file, and that box blocks startup. PCSXROO downgrades it to a warning, but
# pinning a language that definitely exists avoids the noise entirely.
if (-not $sawLanguage) {
    $final = @()
    foreach ($line in $out) {
        $final += $line
        if ($line -eq '[UI]') { $final += 'Language = en-US' }
    }

    $out = $final
    $notes += 'pinned Language to en-US'
}

# Written without a BOM: PCSX2's ini parser expects a plain text file.
[System.IO.File]::WriteAllLines($ini, $out)

if ($notes.Count -gt 0) {
    Write-Host 'sanitised the copied settings:'
    foreach ($note in ($notes | Select-Object -Unique)) {
        Write-Host "  - $note"
    }
}
