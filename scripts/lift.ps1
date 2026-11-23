<#
.SYNOPSIS
    Lift default.xbe to C with xboxrecomp, into src\recomp\gen.
.PARAMETER Xbe
    Path to the game's default.xbe (default input\default.xbe).
.PARAMETER RecompOnly
    Skip the analysis steps and only re-run the lifter, e.g. after changing the
    overrides. Needs a previous full run.
#>
param(
    [string]$Xbe = "input\default.xbe",
    [switch]$RecompOnly
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$python = Join-Path $root ".venv\Scripts\python.exe"
if (-not (Test-Path $python)) { throw "No .venv: python -m venv .venv; .venv\Scripts\pip install capstone" }

$xbePath = (Resolve-Path (Join-Path $root $Xbe)).Path
$analysisJson = Join-Path (Split-Path $xbePath) "default_analysis.json"
$out = Join-Path $root "analysis\otogi2"
$disasm = Join-Path $out "disasm"
$funcId = Join-Path $out "func_id"
$abi = Join-Path $out "abi"
$gen = Join-Path $root "src\recomp\gen"
$overrides = Join-Path $root "src\games\otogi2\overrides.cpp"

function Step([string]$name, [string[]]$arguments) {
    Write-Host "== $name"
    $ErrorActionPreference = "Continue"
    & $python -m "tools.$name" @arguments
    if ($LASTEXITCODE) { throw "tools.$name failed ($LASTEXITCODE)" }
}

Push-Location (Join-Path $root "xboxrecomp")
try {
    if (-not $RecompOnly) {
        Step xbe_parser @($xbePath, "--json", $analysisJson, "--quiet")
        Step disasm @($xbePath, "-o", $disasm, "--analysis-json", $analysisJson)
        Step func_id @($xbePath, "--functions", "$disasm\functions.json", "--strings", "$disasm\strings.json",
            "--xrefs", "$disasm\xrefs.json", "-o", $funcId)
        Step abi_analysis @($xbePath, "--disasm-dir", $disasm, "--func-id-dir", $funcId, "--output-dir", $abi)
    }
    if (Test-Path $gen) { Remove-Item -Recurse -Force $gen }
    Step recomp @($xbePath, "--all", "--split", "250", "--game-name", "Otogi 2", "--gen-dir", $gen,
        "--disasm-dir", $disasm, "--func-id-dir", $funcId, "--abi-dir", $abi, "--exclude-manual", $overrides)
}
finally {
    Pop-Location
}
