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
    Write-Host "ERROR: $cli not found. Run tools\pcsxroo\build.cmd first."
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
    $launchArgs = @('launch', '--ready-timeout', '60000')
    if ($Bios) { $launchArgs += '--bios' }
    if ($Game) { $launchArgs += $Game }

    Invoke-Cli $launchArgs
    Check 'launch reports ready' ($script:lastExit -eq 0) "exit $($script:lastExit)"
    if ($script:lastExit -ne 0) { throw 'cannot continue without a running emulator' }

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
        Check 'bp list shows the breakpoint' (($list.result.breakpoints | Where-Object { $_.addr -eq $target }).Count -ge 1) 'not listed'

        $null = Send @('run')
        $hit = Send @('--timeout', '30000', 'wait')
        Check 'the breakpoint is hit' ($hit.result.reason -in @('breakpoint', 'step')) $hit.result.reason

        # The regression guard for the post-break bookkeeping moved into DebuggerControl:
        # resuming must not immediately re-report the breakpoint we are sitting on.
        $seq = $hit.result.seq
        $null = Send @('run')
        $again = Send @('--timeout', '2000', 'wait', '--since', "$seq")
        Check 'resuming does not instantly re-trigger the same breakpoint' `
            ($script:lastExit -eq 4 -or $again.result.pc -ne $hit.result.pc) "re-hit at $($again.result.pc_hex)"

        $null = Send @('pause')
        $removed = Send @('bp', 'remove', ("0x{0:x}" -f $target))
        Check 'bp remove succeeds' ($removed.ok -eq $true) $removed.error.code

        $twice = Send @('bp', 'remove', ("0x{0:x}" -f $target))
        Check 'removing twice is idempotent' ($twice.result.removed -eq $false) 'not idempotent'

        # --- 7. stepping ---
        $before = (Send @('status')).result.ee.pc
        $stepped = Send @('step', 'into')
        Check 'step into advances the pc' ($stepped.result.pc -ne $before) "$before -> $($stepped.result.pc)"

        # --- 8. registers ---
        $regs = Send @('reg', 'dump', '--category', 'GPR')
        Check 'reg dump returns 32 GPRs' ($regs.result.registers.Count -eq 32) $regs.result.registers.Count

        # --- 9. memory write verification ---
        $scratch = '0x00100000'
        $written = Send @('mem', 'write', $scratch, 'deadbeef')
        Check 'mem write reports verified' ($written.result.verified -eq $true) "after $($written.result.after)"

        # --- 10. disassembly ---
        $dis = Send @('dis', ("0x{0:x}" -f $stepped.result.pc), '4')
        Check 'dis returns instructions' ($dis.result.instructions.Count -eq 4) $dis.result.instructions.Count

        # --- 11. screenshot ---
        $shot = Join-Path ([System.IO.Path]::GetTempPath()) 'pcsxroo-smoke.png'
        Remove-Item $shot -ErrorAction SilentlyContinue
        $null = Send @('screenshot', $shot)
        $deadline = (Get-Date).AddSeconds(10)
        while (-not (Test-Path $shot) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 250 }
        Check 'screenshot lands on disk' (Test-Path $shot) $shot
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
