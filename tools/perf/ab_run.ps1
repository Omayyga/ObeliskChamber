# SPDX-License-Identifier: GPL-3.0-or-later
# A/B performance runs over a scripted route (docs/PERF_ROUTES.md): the game is started unattended several times per variant, the two
# variants alternating (A B, B A, A B, ...) so drift over the session (temperature, background work) falls on both; each run writes its own
# log, and tools/perf/perf_report.py compares the two sets at the end.
#
#   pwsh -File tools\perf\ab_run.ps1 -B "NXRT_NVN_REAPER=false" -Names on,off
#   pwsh -File tools\perf\ab_run.ps1 -A "NXRT_UPSCALE_DLSS_PRESET_AUTO=false" -B "" -Runs 4
#   pwsh -File tools\perf\ab_run.ps1 -Exe C:\ab\master\ObeliskChamber.exe -ExeB game-build\fefw\build\release\ObeliskChamber.exe -Names master,pr
#
# -ExeB runs the B side with another build (two builds, same settings): copy the whole release folder of the old build somewhere first.
#
# A variant is a list of environment variables, `NAME=value` separated by ';' (empty: the ini as it is). Every [section] key of the
# runtime has one: NXRT_<SECTION>_<KEY> in capitals (runtime/docs/CONFIG.md, the "Variable" column); the environment wins over the ini.
# `{run}` in a value becomes a text new to every run (variant, run number, time), e.g. NXRT_NVN_SPIRV_SALT={run} for a cold shader
# compile in each run, as on a player's first start, without deleting any cache.
param(
    [string]$A = "",
    [string]$B = "",
    [string[]]$Names = @("A", "B"),
    [int]$Runs = 3,
    [string]$Exe = "",
    [string]$ExeB = "",
    [string]$Ini = "C:\ObeliskChamber-run\ObeliskChamber.ini",
    [string]$Route = "",
    [string]$Out = "",
    [int]$TimeoutS = 600
)
$ErrorActionPreference = "Stop"
# `pwsh -File` (as from cmd) hands "-Names on,off" over as one string, not two
if ($Names.Count -eq 1 -and $Names[0].Contains(",")) { $Names = $Names[0].Split(",") }
if ($Names.Count -ne 2 -or -not $Names[0] -or -not $Names[1]) { throw "-Names needs two names, e.g. -Names on,off" }
$root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
if (-not $Exe) { $Exe = Join-Path $root "game-build\fefw\build\release\ObeliskChamber.exe" }
if (-not $Route) { $Route = Join-Path $PSScriptRoot "routes\fefw_map.tt" }
if (-not $Out) { $Out = Join-Path $root ("work\perf\" + (Get-Date -Format "yyyyMMdd-HHmmss")) }
if (-not $ExeB) { $ExeB = $Exe }
foreach ($p in @($Exe, $ExeB, $Ini, $Route)) { if (-not (Test-Path $p)) { throw "not found: $p" } }
$Route = (Resolve-Path $Route).Path
$Exe = (Resolve-Path $Exe).Path
$ExeB = (Resolve-Path $ExeB).Path
$exes = @($Exe, $ExeB)
$Ini = (Resolve-Path $Ini).Path
New-Item -ItemType Directory -Force $Out | Out-Null
$runDir = Split-Path -Parent $Ini

function Parse-Vars([string]$spec) {
    $h = [ordered]@{}
    foreach ($part in ($spec -split ';')) {
        $p = $part.Trim()
        if (-not $p) { continue }
        $i = $p.IndexOf('=')
        if ($i -lt 1) { throw "not NAME=value: '$p'" }
        $h[$p.Substring(0, $i).Trim()] = $p.Substring($i + 1).Trim()
    }
    return $h
}
$vars = @((Parse-Vars $A), (Parse-Vars $B))
$allNames = @($vars[0].Keys) + @($vars[1].Keys) | Select-Object -Unique

Write-Host "exe:   $Exe"
if ($ExeB -ne $Exe) { Write-Host "exe B: $ExeB" }
Write-Host "ini:   $Ini"
Write-Host "route: $Route"
Write-Host "out:   $Out"
Write-Host ("{0}: {1}" -f $Names[0], $(if ($A) { $A } else { "(the ini as it is)" }))
Write-Host ("{0}: {1}" -f $Names[1], $(if ($B) { $B } else { "(the ini as it is)" }))
Write-Host "$Runs runs each, alternating; do not touch the game window while it runs."

$logs = @(@(), @())
for ($r = 1; $r -le $Runs; $r++) {
    $order = if ($r % 2 -eq 1) { @(0, 1) } else { @(1, 0) }
    foreach ($v in $order) {
        $name = $Names[$v]
        $log = Join-Path $Out ("{0}_{1}.log" -f $name, $r)
        $shots = Join-Path $Out ("shots_{0}_{1}" -f $name, $r)
        $saved = @{}
        foreach ($n in $allNames + @("NXRT_TEST_SCRIPT", "NXRT_TEST_OUT", "NXRT_TEST_SHOT_DIR", "NXRT_LOG", "NXRT_INPUT_JC2_MOUSE")) { $saved[$n] = [Environment]::GetEnvironmentVariable($n) }
        try {
            foreach ($n in $allNames) { [Environment]::SetEnvironmentVariable($n, $null) }
            $tag = "{0}-{1}-{2}" -f $name, $r, (Get-Date -Format "HHmmssfff")
            foreach ($kv in $vars[$v].GetEnumerator()) { [Environment]::SetEnvironmentVariable($kv.Key, $kv.Value.Replace("{run}", $tag)) }
            [Environment]::SetEnvironmentVariable("NXRT_TEST_SCRIPT", $Route)
            [Environment]::SetEnvironmentVariable("NXRT_TEST_OUT", (Join-Path $Out ("tt_{0}_{1}" -f $name, $r)))
            [Environment]::SetEnvironmentVariable("NXRT_TEST_SHOT_DIR", $shots)
            [Environment]::SetEnvironmentVariable("NXRT_LOG", $log)     # an ini's [log] path wins over --log; the environment wins over both
            # the PC mouse over the game window would wake the Joy-Con 2 mouse and move the menus' cursor under the route's presses
            if (-not $vars[$v].ContainsKey("NXRT_INPUT_JC2_MOUSE")) { [Environment]::SetEnvironmentVariable("NXRT_INPUT_JC2_MOUSE", "off") }
            Write-Host ("run {0}/{1}: {2} ..." -f $r, $Runs, $name) -NoNewline
            $t0 = Get-Date
            $p = Start-Process -FilePath $exes[$v] -ArgumentList @("--ini", "`"$Ini`"", "--log", "`"$log`"") -WorkingDirectory $runDir -PassThru
            if (-not $p.WaitForExit($TimeoutS * 1000)) {
                Stop-Process -Id $p.Id -Force
                Write-Host " stopped after $TimeoutS s (the route did not end: check its screenshots in $shots)"
                continue
            }
            Write-Host (" exit {0} after {1:N0} s" -f $p.ExitCode, ((Get-Date) - $t0).TotalSeconds)
            if ($p.ExitCode -ne 0) { Write-Warning "the route did not pass: look at the 'tt' lines in $log and the screenshots in $shots" }
            if (Test-Path $log) { $logs[$v] += $log } else { Write-Warning "no log at $log" }
        } finally {
            foreach ($kv in $saved.GetEnumerator()) { [Environment]::SetEnvironmentVariable($kv.Key, $kv.Value) }
        }
    }
}

$py = Join-Path $root "tools\.venv\Scripts\python.exe"
if (-not (Test-Path $py)) { $py = "python" }
$report = Join-Path $Out "report.md"
if ($logs[0].Count -and $logs[1].Count) {
    & $py -I (Join-Path $PSScriptRoot "perf_report.py") --a @($logs[0]) --b @($logs[1]) --names $Names[0] $Names[1] | Tee-Object -FilePath $report
    Write-Host "report: $report"
} else {
    Write-Warning "not enough finished runs to compare"
}
