# Seed a PCSXROO portable data directory from an existing PCSX2 installation.
#
# PCSXROO runs in portable mode: a portable.ini beside the executable makes the install
# directory its data directory. That keeps it out of the settings of the PCSX2 you actually
# use, at the cost of a one-time copy - and without a BIOS it stops at its first-run wizard
# before the debug server ever starts.
#
# The aim is that everything you already configured keeps working: BIOS, controller
# bindings and hotkeys, per-game settings, cheats, patches, texture packs, memory cards,
# game list paths, graphics and audio settings. Only four things are changed, each because
# it breaks an unattended session or copies something private - see "sanitise" below.
#
#   .\seed-portable.ps1
#   .\seed-portable.ps1 -From "D:\emu\PCSX2" -To "bin"
#   .\seed-portable.ps1 -Force            # overwrite files already at the destination
#   .\seed-portable.ps1 -IncludeStates    # also copy save states (can be several GB)

[CmdletBinding()]
param(
    [string] $From,
    [string] $To,
    [switch] $Force,
    [switch] $IncludeStates
)

$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $To) { $To = Join-Path $repo 'bin' }

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
    Write-Host "ERROR: destination '$To' does not exist. Run pcsxroo\tools\build.cmd first."
    exit 1
}

$portable = Join-Path $To 'portable.ini'
if (-not (Test-Path $portable)) {
    New-Item -ItemType File -Path $portable | Out-Null
    Write-Host "created $portable"
}

if (-not $From -or -not (Test-Path $From)) {
    Write-Host "No PCSX2 installation found to seed from; pass -From to copy BIOS and settings."
    exit 0
}

Write-Host "seeding from $From"
Write-Host "           to $To"
Write-Host ''

# Everything worth carrying over. cache/ and logs/ are rebuilt, snaps/ and videos/ are
# output, and sstates/ is opt-in because save states run to gigabytes.
$directories = @(
    @{ name = 'bios';          why = 'BIOS images' },
    @{ name = 'memcards';      why = 'memory cards' },
    @{ name = 'inis';          why = 'settings, controller bindings and hotkeys' },
    @{ name = 'gamesettings';  why = 'per-game settings' },
    @{ name = 'inputprofiles'; why = 'input profiles' },
    @{ name = 'cheats';        why = 'cheats and pnach patches' },
    @{ name = 'patches';       why = 'patch files' },
    @{ name = 'textures';      why = 'texture replacement packs' },
    @{ name = 'covers';        why = 'game list covers' }
)

if ($IncludeStates) {
    $directories += @{ name = 'sstates'; why = 'save states' }
}

$copied = 0
$skipped = 0

foreach ($entry in $directories) {
    $sourceDir = Join-Path $From $entry.name
    if (-not (Test-Path $sourceDir)) { continue }

    $files = Get-ChildItem $sourceDir -File -Recurse -ErrorAction SilentlyContinue
    if (-not $files) {
        Write-Host ("  {0,-14} empty, skipped" -f $entry.name)
        continue
    }

    $dirCopied = 0
    foreach ($file in $files) {
        $relative = $file.FullName.Substring($sourceDir.Length).TrimStart('\')
        $dest = Join-Path (Join-Path $To $entry.name) $relative
        $destDir = Split-Path -Parent $dest

        if (-not (Test-Path $destDir)) { New-Item -ItemType Directory -Path $destDir -Force | Out-Null }

        if ((Test-Path $dest) -and -not $Force) { $skipped++; continue }

        Copy-Item $file.FullName $dest -Force
        $copied++
        $dirCopied++
    }

    Write-Host ("  {0,-14} {1,5} file(s)  {2}" -f $entry.name, $dirCopied, $entry.why)
}

Write-Host ''
Write-Host "copied $copied file(s), skipped $skipped already present (use -Force to overwrite)"

# --- sanitise ---------------------------------------------------------------------------
#
# Four changes, and only four. Everything else you configured is left exactly as it was.

$ini = Join-Path $To 'inis\PCSX2.ini'
if (-not (Test-Path $ini)) { exit 0 }

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

    # 1. The source install's RetroAchievements login travels with the file. A credential
    #    should not be copied to a second location just because it was convenient.
    if ($section -eq 'Achievements' -and $line -match '^(Username|Token|LoginToken) = ') {
        $notes += 'removed the copied RetroAchievements credentials'
        continue
    }

    # 2. Hardcore mode disables the debug server outright - it is the same rule that already
    #    disables PINE - so an agent session could never attach. Achievements themselves stay
    #    on if you had them on.
    if ($section -eq 'Achievements' -and $line -match '^ChallengeMode = true') {
        $notes += 'turned off achievements hardcore mode (it disables the debug server)'
        $out += 'ChallengeMode = false'
        continue
    }

    # 3. Directory overrides point back at the installation we just copied from, so PCSXROO
    #    would read the BIOS and write memory cards over there instead of using its own.
    if ($section -eq 'Folders' -and $line -match '^\w+ = ') {
        $notes += 'cleared directory overrides so PCSXROO uses its own copied folders'
        continue
    }

    # 4. Starting fullscreen makes an unattended session take over the display.
    if ($section -eq 'UI' -and $line -match '^StartFullscreen = true') {
        $notes += 'turned off start-fullscreen'
        $out += 'StartFullscreen = false'
        continue
    }

    if ($section -eq 'UI' -and $line -match '^Language = ') { $sawLanguage = $true }

    $out += $line
}

# PCSXROO downgrades PCSX2's "Translation Error" dialog to a warning, but pinning a language
# that definitely ships avoids the noise for locales with no .qm file at all.
if (-not $sawLanguage) {
    $final = @()
    foreach ($line in $out) {
        $final += $line
        if ($line -eq '[UI]') { $final += 'Language = en-US' }
    }

    $out = $final
    $notes += 'pinned Language to en-US'
}

# Written without a BOM: PCSX2's ini parser expects plain text.
[System.IO.File]::WriteAllLines($ini, $out)

if ($notes.Count -gt 0) {
    Write-Host ''
    Write-Host 'changed in the copied settings:'
    foreach ($note in ($notes | Select-Object -Unique)) { Write-Host "  - $note" }
    Write-Host ''
    Write-Host 'everything else - controls, hotkeys, graphics, audio, game paths, per-game'
    Write-Host 'settings, cheats and texture packs - was copied unchanged.'
}

# patches.zip is not in the source tree; CI fetches it. Without it the emulator logs a
# failure to open it and then dies on the way to opening the GS.
$patches = Join-Path $To 'resources\patches.zip'
if (-not (Test-Path $patches)) {
    Write-Host ''
    Write-Host 'fetching resources\patches.zip (built-in game patches)...'
    try {
        $resourcesDir = Split-Path -Parent $patches
        if (-not (Test-Path $resourcesDir)) { New-Item -ItemType Directory -Path $resourcesDir -Force | Out-Null }

        Invoke-WebRequest -Uri 'https://github.com/PCSX2/pcsx2_patches/releases/latest/download/patches.zip' `
            -OutFile $patches -UseBasicParsing
        Write-Host "  got $([math]::Round((Get-Item $patches).Length / 1MB, 1)) MB"
    } catch {
        Write-Host "  could not download it: $_"
        Write-Host '  Fetch it manually from https://github.com/PCSX2/pcsx2_patches/releases/latest'
    }
}
