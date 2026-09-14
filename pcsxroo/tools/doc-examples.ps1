# Runs the behaviours pcsxroo/docs/cli.md and agent-guide.md promise, exactly as they are
# written there, against a live emulator.
#
# The point is to catch documentation drift: every check here corresponds to a specific
# claim in the docs, so a claim that stops being true fails here rather than wasting an
# agent's time later.
#
#   .\doc-examples.ps1
$ErrorActionPreference = 'Continue'
$R = "C:\Users\RaaViVi\Documents\github\pcsxroo"
$cli = "$R\bin\pcsxroo.exe"
$pass = 0; $fail = 0

function Check($label, $ok, $detail) {
    if ($ok) { Write-Host "PASS  $label"; $script:pass++ }
    else { Write-Host "FAIL  $label -- $detail"; $script:fail++ }
}
function J([string[]] $a, [int] $t = 8000) {
    $raw = (& $cli --json --timeout $t @a 2>&1 | Out-String)
    $script:code = $LASTEXITCODE
    try { return $raw | ConvertFrom-Json } catch { return $null }
}

Get-Process pcsxroo-qt -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400

# agent-guide "shape of a session"
& $cli launch --ready-timeout 40000 2>&1 | Out-Null
Check 'launch' ($LASTEXITCODE -eq 0) "exit $LASTEXITCODE"

$b = J @('boot','--bios') 60000
Check 'boot --bios' ($b.result.booted -eq $true) $b.error.code
Start-Sleep -Seconds 6

$p = J @('input','press','Start')
Check 'input press Start' ($p.ok -eq $true) $p.error.code

$pa = J @('pause')
Check 'pause returns a stop event' ($null -ne $pa.result.seq) $pa.error.code

$rg = J @('reg','get','a0')
Check 'reg get a0 while paused' ($rg.ok -eq $true) $rg.error.code

# guide: registers need a paused VM
$null = J @('resume')
Start-Sleep -Milliseconds 800
$np = J @('reg','get','a0')
Check 'reg get while running -> not_paused' ($np.error.code -eq 'not_paused') "got $($np.error.code)"

# guide: the --since workflow
$seq = (J @('status')).result.last_stop.seq
$pcNow = (J @('pause')).result.pc
$bp = J @('bp','add', ("0x{0:x}" -f $pcNow), '--cond', 'a0 == a0', '--desc', 'documented example')
Check 'bp add with a documented condition' ($bp.ok -eq $true) $bp.error.code

$seq = (J @('status')).result.last_stop.seq
$null = J @('run')
$hit = J @('wait','--since',"$seq") 20000
Check 'wait --since catches the hit' ($hit.result.reason -eq 'breakpoint') "reason $($hit.result.reason)"

# reference: $ prefix is rejected in expressions
$badExpr = J @('eval','$a0 == 2')
Check 'expressions reject the $ prefix' ($badExpr.ok -eq $false) 'accepted it'
$goodExpr = J @('eval','a0 == 2')
Check 'expressions accept a bare register' ($goodExpr.ok -eq $true) $goodExpr.error.code

# guide: memory search recipe
$s1 = J @('mem','search','--range','0x100000:0x140000','--unknown') 30000
Check 'mem search --unknown starts a session' ($null -ne $s1.result.session) $s1.error.code
$s2 = J @('mem','search','--session', "$($s1.result.session)", '--not-changed') 30000
Check 'mem search --session narrows it' ($s2.result.count -le $s1.result.count) "$($s1.result.count) -> $($s2.result.count)"

# reference: delta comparison refused on a first pass
$bad = J @('mem','search','--range','0x100000:0x110000','--increased') 20000
Check 'delta comparison refused on a first pass' ($bad.ok -eq $false) 'was allowed'

# reference: mem write reports verified
$w = J @('mem','write','0x100000','deadbeef')
Check 'mem write reports verified' ($w.result.verified -eq $true) $w.result.after

# reference: asm leaves memory alone on a bad instruction
$before = (J @('mem','read','0x100000','4')).result.data
$badAsm = J @('asm','0x100000','not_an_instruction')
$after = (J @('mem','read','0x100000','4')).result.data
Check 'a bad asm line leaves memory untouched' (($badAsm.ok -eq $false) -and ($before -eq $after)) "$before -> $after"

# reference: screenshot needs a running VM. The breakpoint is cleared first, or it keeps
# re-firing in this tight BIOS loop and the VM never stays running long enough to present.
$null = J @('bp','clear')
$null = J @('resume')
Start-Sleep -Seconds 2
$shot = Join-Path ([System.IO.Path]::GetTempPath()) 'pcsxroo-doc.png'
Remove-Item $shot -ErrorAction SilentlyContinue
$null = J @('screenshot', $shot)
$deadline = (Get-Date).AddSeconds(10)
while (-not (Test-Path $shot) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 250 }
Check 'screenshot while running lands on disk' (Test-Path $shot) 'no file'

# reference: bp.remove is idempotent
$again = J @('bp','remove','0x100000')
Check 'bp remove on a clear address is not an error' ($again.result.removed -eq $false) $again.error.code

$null = J @('shutdown')
Start-Sleep -Seconds 2
Get-Process pcsxroo-qt -ErrorAction SilentlyContinue | Stop-Process -Force

Write-Host ""
Write-Host "$pass passed, $fail failed"
exit $(if ($fail -gt 0) { 1 } else { 0 })
