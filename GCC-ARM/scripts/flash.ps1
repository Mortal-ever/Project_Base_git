param(
    [ValidateSet("Coffee2", "MilkTea")]
    [string]$Product = "Coffee2",
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",
    [ValidateSet("OpenOCD", "STLink")]
    [string]$Backend = "OpenOCD",
    [switch]$Program
)

$ErrorActionPreference = "Stop"
$ShellRoot = Split-Path -Parent $PSScriptRoot
$TargetName = "${Product}Target"
$ElfFile = Join-Path $ShellRoot "build\$Product-$Configuration\$TargetName.elf"
$HexFile = Join-Path $ShellRoot "build\$Product-$Configuration\$TargetName.hex"
$OpenOcd = Join-Path $ShellRoot ".tools\openocd\bin\openocd.exe"
$OpenOcdScripts = Join-Path $ShellRoot ".tools\openocd\openocd\scripts"
$StLink = "C:\Program Files (x86)\STMicroelectronics\STM32 ST-LINK Utility\ST-LINK Utility\ST-LINK_CLI.exe"

if ($Backend -eq "OpenOCD") {
    if (-not (Test-Path -LiteralPath $ElfFile)) {
        throw "Firmware not found: $ElfFile. Build $Product-$Configuration first."
    }
    foreach ($RequiredPath in @($OpenOcd, $OpenOcdScripts)) {
        if (-not (Test-Path -LiteralPath $RequiredPath)) {
            throw "Missing project-local OpenOCD component: $RequiredPath"
        }
    }
    if (-not $Program) {
        Write-Output "Ready to program with project-local OpenOCD: $ElfFile"
        Write-Output "Re-run with -Program only after confirming the connected target board."
        exit 0
    }

    $OpenOcdElf = $ElfFile.Replace("\", "/")
    & $OpenOcd `
        -s $OpenOcdScripts `
        -f interface/stlink.cfg `
        -f target/stm32f4x.cfg `
        -c "adapter speed 4000" `
        -c "program {$OpenOcdElf} verify reset exit"
    if ($LASTEXITCODE -ne 0) {
        throw "OpenOCD programming failed."
    }
    exit 0
}

if (-not (Test-Path -LiteralPath $HexFile)) {
    throw "Firmware not found: $HexFile. Build $Product-$Configuration first."
}
if (-not (Test-Path -LiteralPath $StLink)) {
    throw "ST-LINK Utility CLI not found: $StLink"
}
if (-not $Program) {
    Write-Output "Ready to program with ST-LINK Utility CLI: $HexFile"
    Write-Output "Re-run with -Program only after confirming the connected target board."
    exit 0
}

& $StLink -c SWD -P $HexFile -V -Run
if ($LASTEXITCODE -ne 0) {
    throw "ST-LINK programming failed."
}
