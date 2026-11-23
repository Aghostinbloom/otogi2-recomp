<#
.SYNOPSIS
    Configure and build otogi2 with clang-cl and Ninja.
.PARAMETER Config
    CMake build type (default RelWithDebInfo).
.PARAMETER Compiler
    clang-cl (default) or cl.
#>
param(
    [string]$Config = "RelWithDebInfo",
    [ValidateSet("clang-cl", "cl")][string]$Compiler = "clang-cl"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

$vsRoots = @("${env:ProgramFiles}\Microsoft Visual Studio", "${env:ProgramFiles(x86)}\Microsoft Visual Studio")
$vcvars = Get-ChildItem $vsRoots -Filter vcvarsall.bat -Recurse -Depth 6 -ErrorAction SilentlyContinue |
    Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
if (-not $vcvars) { throw "vcvarsall.bat not found: install Visual Studio 2022 Build Tools with the C++ workload." }

# Import the x64 developer environment into this session.
cmd /c "`"$vcvars`" x64 >nul && set" | ForEach-Object {
    if ($_ -match "^([^=]+)=(.*)$") { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
}

if ($Compiler -eq "clang-cl" -and -not (Get-Command clang-cl -ErrorAction SilentlyContinue)) {
    $llvm = "${env:ProgramFiles}\LLVM\bin"
    if (-not (Test-Path "$llvm\clang-cl.exe")) { throw "clang-cl not found: install LLVM or pass -Compiler cl." }
    $env:PATH = "$llvm;$env:PATH"
}

# Native tools write progress to stderr, which Windows PowerShell would turn into
# terminating errors under "Stop".
$ErrorActionPreference = "Continue"
$build = Join-Path $root "build"
cmake -S $root -B $build -G Ninja "-DCMAKE_BUILD_TYPE=$Config" `
    "-DCMAKE_C_COMPILER=$Compiler" "-DCMAKE_CXX_COMPILER=$Compiler"
if ($LASTEXITCODE) { exit $LASTEXITCODE }
cmake --build $build
exit $LASTEXITCODE
