param(
    [ValidateSet("Coffee2", "MilkTea")]
    [string]$Product = "Coffee2",
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",
    [switch]$Fresh
)

$ErrorActionPreference = "Stop"
$ShellRoot = Split-Path -Parent $PSScriptRoot
$CMake = Join-Path $ShellRoot ".tools\python\cmake\data\bin\cmake.exe"
$Ninja = Join-Path $ShellRoot ".tools\python\bin\ninja.exe"
$ArmCompiler = Join-Path $ShellRoot ".tools\arm-gnu-toolchain\bin\arm-none-eabi-gcc.exe"
$Preset = "$Product-$Configuration"

foreach ($RequiredTool in @($CMake, $Ninja, $ArmCompiler)) {
    if (-not (Test-Path -LiteralPath $RequiredTool)) {
        throw "Missing project-local tool: $RequiredTool. Run setup_toolchain.ps1 first."
    }
}

# Switch to the GCC-ARM shell root so `cmake --preset` resolves CMakePresets.json
# from GCC-ARM/ regardless of the caller's current working directory.
Set-Location $ShellRoot

$ConfigureArguments = @("--preset", $Preset)
if ($Fresh) {
    $ConfigureArguments = @("--fresh") + $ConfigureArguments
}

& $CMake @ConfigureArguments
if ($LASTEXITCODE -ne 0) {
    throw "CMake configuration failed for $Preset."
}

& $CMake --build --preset $Preset --parallel
if ($LASTEXITCODE -ne 0) {
    throw "Firmware build failed for $Preset."
}
