# End-to-end smoke test for the PCSXROO debugger CLI.
#
#   .\smoke-test.ps1                 # server-only checks, no game needed
#   .\smoke-test.ps1 -Bios           # also boot the PS2 BIOS and exercise the debugger
#   .\smoke-test.ps1 -Game "G:\roms\ps2\game.iso"
#
# Uses a non-default port so it cannot collide with a debugging session already in progress.
# Always shuts the emulator down, even when a step fails.

[CmdletBinding()]
param(
    [int] $Port = 28199,
    [string] $BinDir,
    [string] $Game,
    [switch] $Bios
)

$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $BinDir) { $BinDir = Join-Path $repo 'bin' }

$cli = Join-Path $BinDir 'pcsxroo.exe'
if (-not (Test-Path $cli)) {
    Write-Host "ERROR: $cli not found. Run pcsxroo\tools\build.cmd first."
    exit 1
}

$script:passed = 0
$script:failed = 0

function Check($name, $condition, $detail) {
    if ($condition) {
        Write-Host ("PASS  {0}" -f $name)
        $script:passed++
    } else {
        Write-Host ("FAIL  {0}{1}" -f $name, $(if ($detail) { " -- $detail" } else { "" }))
        $script:failed++
    }
}

# Returns the parsed JSON reply, or $null. Exit code lands in $script:lastExit.
#
# ErrorActionPreference is relaxed for the call: with it set to Stop, anything a native
# command writes to stderr becomes a terminating error, and pcsxroo writes its error text
# there by design. Every non-zero exit here is expected output, not a script failure.
function Send([string[]] $arguments) {
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $raw = & $cli --port $Port --json @arguments 2>&1 | Out-String
        $script:lastExit = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previous
    }

    try { return $raw | ConvertFrom-Json } catch { return $null }
}

# Runs the CLI for its exit code and console output rather than a JSON reply.
function Invoke-Cli([string[]] $arguments) {
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $cli --port $Port @arguments 2>&1 | Out-Host
        $script:lastExit = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previous
    }
}

$emulator = $null

try {
    # --- 1. the CLI reports a missing emulator rather than hanging ---
    $null = Send @('status')
    Check 'status with no emulator exits 3' ($script:lastExit -eq 3) "got $($script:lastExit)"

    # --- 2. launch ---
    Invoke-Cli @('launch', '--ready-timeout', '60000')
    Check 'launch reports ready' ($script:lastExit -eq 0) "exit $($script:lastExit)"
    if ($script:lastExit -ne 0) { throw 'cannot continue without a running emulator' }

    # Booting is a separate step from starting the emulator, and fails for its own reasons.
    if ($Bios -or $Game) {
        $bootArgs = @('boot')
        if ($Game) { $bootArgs += $Game } else { $bootArgs += '--bios' }

        Invoke-Cli (@('--timeout', '120000') + $bootArgs)
        Check 'boot succeeds' ($script:lastExit -eq 0) "exit $($script:lastExit)"
        Start-Sleep -Seconds $(if ($Game) { 25 } else { 6 })
    }

    $emulator = Get-Process pcsxroo-qt -ErrorAction SilentlyContinue | Select-Object -Last 1

    # --- 3. version ---
    $version = Send @('version')
    Check 'version identifies PCSXROO' ($version.result.emulator -eq 'PCSXROO') $version.result.emulator
    Check 'version reports protocol 1' ($version.result.protocol_version -eq 1) $version.result.protocol_version

    # --- 4. protocol hygiene ---
    $unknown = Send @('status')
    Check 'status succeeds' ($script:lastExit -eq 0) "exit $($script:lastExit)"

    $waited = Send @('--timeout', '1500', 'wait')
    Check 'wait times out with exit 4' ($script:lastExit -eq 4) "exit $($script:lastExit)"
    Check 'wait reports a timeout error' ($waited.error.code -eq 'timeout') $waited.error.code

    if (-not ($Bios -or $Game)) {
        # Without a VM these must fail cleanly rather than crash or hang.
        $novm = Send @('bp', 'list')
        Check 'bp list without a VM reports no_vm' ($novm.error.code -eq 'no_vm') $novm.error.code
        Check 'no_vm exits 1' ($script:lastExit -eq 1) "exit $($script:lastExit)"

        Write-Host ''
        Write-Host 'Server checks only; pass -Bios or -Game to exercise the debugger itself.'
    } else {
        # --- 5. a real debugging session ---
        $status = Send @('status')
        Check 'a VM is running' ($status.result.vm_state -in @('running', 'paused')) $status.result.vm_state

        # Pad input, which is what lets an agent get a game into the state it wants.
        $buttons = Send @('input', 'list')
        Check 'input list names the pad' ($buttons.result.buttons.Count -gt 0) $buttons.result.controller

        $pressed = Send @('input', 'press', 'Start')
        Check 'input press accepted' ($pressed.ok -eq $true) $pressed.error.code

        $held = Send @('input', 'set', 'Cross', '--left-stick', '1.0,0.0')
        Check 'input set holds two binds' ($held.result.binds -eq 2) $held.result.binds

        $released = Send @('input', 'release')
        Check 'input release accepted' ($released.ok -eq $true) $released.error.code

        $paused = Send @('pause')
        Check 'pause reports a stop' ($null -ne $paused.result.seq) "exit $($script:lastExit)"

        $status = Send @('status')
        Check 'status shows paused' ($status.result.paused -eq $true) $status.result.vm_state

        $pc = $status.result.ee.pc
        Check 'ee pc is readable' ($null -ne $pc) 'no pc'

        # --- 6. breakpoint round trip ---
        $target = $pc + 4
        $added = Send @('bp', 'add', ("0x{0:x}" -f $target))
        Check 'bp add succeeds' ($added.ok -eq $true) $added.error.code

        $list = Send @('bp', 'list')
        $listed = @($list.result.breakpoints | Where-Object { [uint32] $_.addr -eq [uint32] $target })
        Check 'bp list shows the breakpoint' ($listed.Count -ge 1) ("listed: " + (($list.result.breakpoints | ForEach-Object { $_.addr_hex }) -join ','))

        $null = Send @('run')
        $hit = Send @('--timeout', '30000', 'wait')
        Check 'the breakpoint is hit' ($hit.result.reason -in @('breakpoint', 'step')) $hit.result.reason

        # The regression guard for the post-break bookkeeping moved into DebuggerControl:
        # resuming must make progress rather than re-reporting the stop we are sitting on
        # without executing anything. A tight loop legitimately comes back to the same
        # breakpoint, so the check is that a new stop is reported at all, with a new
        # sequence number - not that the address differs.
        $seq = $hit.result.seq
        $null = Send @('run')
        $again = Send @('--timeout', '3000', 'wait', '--since', "$seq")
        Check 'resuming makes progress rather than re-reporting the same stop' `
            ($script:lastExit -eq 4 -or $again.result.seq -gt $seq) "seq $seq -> $($again.result.seq)"

        $null = Send @('pause')
        $removed = Send @('bp', 'remove', ("0x{0:x}" -f $target))
        Check 'bp remove succeeds' ($removed.ok -eq $true) $removed.error.code

        $twice = Send @('bp', 'remove', ("0x{0:x}" -f $target))
        Check 'removing twice is idempotent' ($twice.result.removed -eq $false) 'not idempotent'

        # A stop that cuts a frame advance short has to cancel the frames it did not run.
        # Otherwise the leftover count keeps ticking once the VM runs again, and a plain
        # run pauses by itself a moment later with a stop nobody asked for.
        $advancePc = (Send @('status')).result.ee.pc
        $advanceTarget = $advancePc + 4
        $null = Send @('bp', 'add', ("0x{0:x}" -f $advanceTarget))
        $advanced = Send @('frame-advance', '60')
        Check 'a breakpoint ends a frame advance early' `
            ($advanced.result.stop.reason -in @('breakpoint', 'step')) $advanced.result.stop.reason
        $null = Send @('bp', 'remove', ("0x{0:x}" -f $advanceTarget))

        $advanceSeq = $advanced.result.stop.seq
        $null = Send @('run')
        $spurious = Send @('--timeout', '3000', 'wait', '--since', "$advanceSeq")
        Check 'running after an interrupted frame advance does not pause by itself' `
            ($script:lastExit -eq 4) "stopped: $($spurious.result.reason) seq $($spurious.result.seq)"
        $null = Send @('pause')

        # --- 7. stepping ---
        $before = (Send @('status')).result.ee.pc
        $stepped = Send @('step', 'into')
        Check 'step into advances the pc' ($stepped.result.pc -ne $before) "$before -> $($stepped.result.pc)"

        $resumed = Send @('resume')
        Check 'resume unpauses' ($resumed.ok -eq $true) $resumed.error.code
        $null = Send @('pause')

        # --- 8. registers ---
        # The EE GPR category carries pc, hi and lo alongside the 32 general registers.
        $regs = Send @('reg', 'dump', '--category', 'GPR')
        Check 'reg dump returns the GPRs' ($regs.result.registers.Count -ge 32) $regs.result.registers.Count

        # --- 9. memory write verification ---
        $scratch = '0x00100000'
        $written = Send @('mem', 'write', $scratch, 'deadbeef')
        Check 'mem write reports verified' ($written.result.verified -eq $true) "after $($written.result.after)"

        # --- 10. disassembly ---
        $dis = Send @('dis', ("0x{0:x}" -f $stepped.result.pc), '4')
        Check 'dis returns instructions' ($dis.result.instructions.Count -eq 4) $dis.result.instructions.Count

        # --- 11. screenshot ---
        # The GS only presents a frame while the VM runs, so a snapshot requested while
        # paused would sit in the queue and the file would never appear.
        $null = Send @('resume')
        Start-Sleep -Seconds 2

        $shot = Join-Path ([System.IO.Path]::GetTempPath()) 'pcsxroo-smoke.png'
        Remove-Item $shot -ErrorAction SilentlyContinue
        $null = Send @('screenshot', $shot)
        $deadline = (Get-Date).AddSeconds(10)
        while (-not (Test-Path $shot) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 250 }
        Check 'screenshot lands on disk' (Test-Path $shot) $shot

        # --- 12. shutdown while paused ---
        # A paused VM leaves the emulator thread blocked in its event loop. Shutdown has to
        # wake that loop, or vm_state sits at "stopping" forever and every later boot is
        # refused with "already running".
        $null = Send @('pause')
        $down = Send @('shutdown')
        Check 'shutdown is accepted while paused' ($down.result.stopping -eq $true) $down.error.code

        $state = ''
        $deadline = (Get-Date).AddSeconds(15)
        while ((Get-Date) -lt $deadline) {
            $state = (Send @('status')).result.vm_state
            if ($state -eq 'shutdown') { break }
            Start-Sleep -Milliseconds 250
        }
        Check 'a paused VM reaches shutdown' ($state -eq 'shutdown') "still $state after 15s"

        if ($state -eq 'shutdown') {
            Invoke-Cli (@('--timeout', '120000') + $bootArgs)
            Check 'the emulator boots again after a paused shutdown' ($script:lastExit -eq 0) "exit $($script:lastExit)"
        }
    }
} catch {
    Write-Host "FAIL  $_"
    $script:failed++
} finally {
    if ($emulator) {
        $ErrorActionPreference = 'Continue'
        $null = & $cli --port $Port shutdown 2>&1
        Start-Sleep -Seconds 2
        Get-Process -Id $emulator.Id -ErrorAction SilentlyContinue | Stop-Process -Force
    } else {
        Get-Process pcsxroo-qt -ErrorAction SilentlyContinue | Stop-Process -Force
    }
}

Write-Host ''
Write-Host ("{0} passed, {1} failed" -f $script:passed, $script:failed)
exit $(if ($script:failed -gt 0) { 1 } else { 0 })
