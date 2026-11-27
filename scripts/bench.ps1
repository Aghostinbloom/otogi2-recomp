<#
.SYNOPSIS
    Repeatable run into the first stage, with a report on frame times and audio.
.DESCRIPTION
    Starts a new game with a scripted pad (logos, title, New Game, settings,
    a save in a fresh folder, story text, the cutscene skipped), waits until
    map\m000 loads, plays the stage for -Seconds pressing A now and then, then
    stops the game and writes report.txt next to the log: the [PERF] lines of
    the stage, every [HITCH] and [FREEZE], the [AUDIO] totals, and
    scripts\audio_report.py on the recorded output.
.PARAMETER Game
    The extracted game folder (default: game, as in the README).
.PARAMETER Out
    The folder for the log, the recording, the save and the report
    (default: build\bench\<date-time>).
.PARAMETER Seconds
    How long to stay in the stage once it is loaded (default 60).
.PARAMETER Frames
    Also write a frame every 2 seconds to <Out>\frames, to see where a run went.
#>
param(
    [string]$Game = "",
    [string]$Out = "",
    [int]$Seconds = 60,
    [switch]$Frames
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not $Game) { $Game = Join-Path $root "game" }
if (-not $Out) { $Out = Join-Path $root ("build\bench\" + (Get-Date -Format "yyyyMMdd-HHmmss")) }
$exe = Join-Path $root "build\otogi2.exe"
if (-not (Test-Path $exe)) { throw "build\otogi2.exe not found: run scripts\build.ps1 first." }
if (-not (Test-Path (Join-Path $Game "default.xbe"))) { throw "no default.xbe in $Game (pass -Game)." }
New-Item -ItemType Directory -Force $Out, (Join-Path $Out "save") | Out-Null
$log = Join-Path $Out "stderr.txt"
$pad = Join-Path $Out "pad.txt"
$wav = Join-Path $Out "audio.wav"
Set-Content $pad -Value $null

Get-Process otogi2 -ErrorAction SilentlyContinue | Stop-Process -Force
$vars = @{
    RECOMP_SAVE_DIR   = (Join-Path $Out "save")
    RECOMP_PAD_LIVE   = $pad
    RECOMP_PERF       = "1"
    RECOMP_AUDIO_WAV  = $wav
}
if ($Frames) {
    New-Item -ItemType Directory -Force (Join-Path $Out "frames") | Out-Null
    $vars.RECOMP_D3D9_DUMP = Join-Path $Out "frames\f"
    $vars.RECOMP_D3D9_DUMP_EVERY = "120"
}
foreach ($k in $vars.Keys) { Set-Item "env:$k" $vars[$k] }
$p = Start-Process -FilePath $exe -ArgumentList "`"$Game`"" -RedirectStandardOutput (Join-Path $Out "stdout.txt") `
    -RedirectStandardError $log -PassThru -NoNewWindow
foreach ($k in $vars.Keys) { Remove-Item "env:$k" }

# Taps of 80 ms: the menus repeat a button held longer (150 ms, the pad
# script's default hold, moves the cursor twice).
function Press([string[]]$buttons, [int]$wait) {
    foreach ($b in $buttons) { Add-Content $pad "${b}:80"; Start-Sleep -Milliseconds 700 }
    Start-Sleep -Milliseconds $wait
}
function Seen([string]$pattern) { (Test-Path $log) -and (Select-String -Path $log -Pattern $pattern -Quiet) }

# Each screen is waited for in the log rather than timed: how fast the title
# boots depends on the machine, and a main menu left alone returns to the
# title after about 15 s.
function WaitFor([string]$pattern, [int]$seconds, [string]$press = "", [string]$what) {
    $until = (Get-Date).AddSeconds($seconds)
    while (-not (Seen $pattern)) {
        if ((Get-Date) -gt $until -or $p.HasExited) {
            if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
            throw "$what did not come within $seconds s; see $log"
        }
        if ($press) { Press @($press) 0 } else { Start-Sleep -Milliseconds 500 }
    }
}

WaitFor 'mainmenu_locz\.med' 60 "start" "the main menu"        # Start through logos and title
WaitFor 'm000prm\.dat' 20 "" "the main menu's background"
Press @() 1500
Press @("a") 3000                                    # New Game
Press @("down", "down", "down", "down", "a") 3500   # Game Settings, Next Page
Press @("down", "down", "down", "down", "a") 3500   # Sound Settings, Next Page
Press @("a") 3500                                    # Controller Config
Press @("a") 3500                                    # save slot 01
Press @("a") 0                                       # "Create a new Save Game?" YES
WaitFor 'SaveMeta\.xbx' 30 "" "the save"
Start-Sleep 8
Press @("a", "a", "a") 6000                          # story text
$stage = $false
for ($k = 0; $k -lt 12 -and -not $stage; $k++) {
    Press @("a") 5000                                # skips the m050 cutscene
    $stage = Seen 'map\\m000\\scene\.dat'
}
if (-not $stage) {
    Stop-Process -Id $p.Id -Force
    throw "map\m000 was not reached; see $log"
}
$mark = (Get-Content $log).Count
Write-Host "m000 loading; staying $Seconds s"
$end = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $end) { Press @("a") 2300 }
Stop-Process -Id $p.Id -Force
Start-Sleep 1

# The report covers the stage only: the lines logged after m000 began loading.
$lines = Get-Content $log | Select-Object -Skip $mark
$perf = $lines | Where-Object { $_ -like "[[]PERF]*" }
$report = New-Object System.Collections.Generic.List[string]
$report.Add("Stage m000, $Seconds s after it began loading")
if ($perf) {
    $fps = $perf | ForEach-Object { if ($_ -match '\[PERF\] ([\d.]+) fps, frame ([\d.]+) ms mean, ([\d.]+) worst, ([\d.]+) 99th') {
        [pscustomobject]@{ fps = [double]$Matches[1]; mean = [double]$Matches[2]; worst = [double]$Matches[3]; p99 = [double]$Matches[4] } } }
    $m = $fps | Measure-Object fps -Average -Minimum
    $w = $fps | Measure-Object worst -Maximum
    $q = $fps | Measure-Object p99 -Average
    $report.Add(("frames: {0:N1} fps on average, {1:N1} at the lowest second; worst frame {2:N0} ms; 99th percentile {3:N1} ms on average" -f $m.Average, $m.Minimum, $w.Maximum, $q.Average))
}
$hitches = @($lines | Where-Object { $_ -like "[[]HITCH]*" })
$freezes = @($lines | Where-Object { $_ -like "[[]FREEZE] no frame*" })
$report.Add("frames over 50 ms: $($hitches.Count); freezes (no frame for 200 ms): $($freezes.Count)")
$audio = $lines | Where-Object { $_ -match '^\[AUDIO\] (\d+) underruns; the device played ([\d.]+)%' } | ForEach-Object {
    [pscustomobject]@{ u = [int]($_ -replace '^\[AUDIO\] (\d+).*', '$1'); kept = [double]($_ -replace '.* played ([\d.]+)%.*', '$1') } }
if ($audio) {
    $u = ($audio | Measure-Object u -Sum).Sum
    $k = ($audio | Measure-Object kept -Average).Average
    $report.Add(("audio: {0} device underruns ({1:N1} a second); the device played {2:N1}% of real time" -f $u, ($u / [Math]::Max(1, $audio.Count)), $k))
}
$report.Add("")
$report.Add("Hitches (first 20):")
$hitches | Select-Object -First 20 | ForEach-Object { $report.Add("  $_") }
$report.Add("")
$report.Add("Audio recording:")
if (Test-Path $wav) {
    $python = Join-Path $root ".venv\Scripts\python.exe"
    if (-not (Test-Path $python)) { $python = "python" }
    $ErrorActionPreference = "Continue"
    & $python (Join-Path $PSScriptRoot "audio_report.py") $wav 2>&1 | ForEach-Object { $report.Add("  $_") }
}
$report | Set-Content (Join-Path $Out "report.txt")
$report
Write-Host "report: $(Join-Path $Out 'report.txt')"
