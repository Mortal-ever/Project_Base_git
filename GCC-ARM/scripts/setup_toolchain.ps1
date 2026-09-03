param()

$ErrorActionPreference = "Stop"
$ShellRoot = Split-Path -Parent $PSScriptRoot
$ToolsRoot = Join-Path $ShellRoot ".tools"
$PythonTools = Join-Path $ToolsRoot "python"
$Downloads = Join-Path $ToolsRoot "downloads"
$ArmRoot = Join-Path $ToolsRoot "arm-gnu-toolchain"
$ArmCompiler = Join-Path $ArmRoot "bin\arm-none-eabi-gcc.exe"
$OpenOcd = Join-Path $ToolsRoot "openocd\bin\openocd.exe"
$CMake = Join-Path $PythonTools "cmake\data\bin\cmake.exe"
$Ninja = Join-Path $PythonTools "bin\ninja.exe"
$ArchiveName = "arm-gnu-toolchain-15.2.rel1-mingw-w64-x86_64-arm-none-eabi.zip"
$Archive = Join-Path $Downloads $ArchiveName
$ExpectedSha256 = "7936CAC895611023FFB22A64B8E426098C7104CB689778C1894572CA840B9ECE"
$ArmUrl = "https://developer.arm.com/-/media/Files/downloads/gnu/15.2.rel1/binrel/$ArchiveName"

New-Item -ItemType Directory -Force -Path $ToolsRoot, $Downloads | Out-Null

if ((-not (Test-Path -LiteralPath $CMake)) -or
    (-not (Test-Path -LiteralPath $Ninja))) {
    $Python = Get-Command python -ErrorAction Stop
    & $Python.Source -m pip install --disable-pip-version-check `
        --target $PythonTools "cmake==4.3.4" "ninja==1.13.0"
    if ($LASTEXITCODE -ne 0) {
        throw "CMake/Ninja installation failed."
    }
}

if (-not (Test-Path -LiteralPath $ArmCompiler)) {
    if (-not (Test-Path -LiteralPath $Archive)) {
        Invoke-WebRequest -Uri $ArmUrl -OutFile $Archive
    }
    $ActualSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $Archive).Hash
    if ($ActualSha256 -ne $ExpectedSha256) {
        throw "GNU Arm archive checksum mismatch."
    }
    New-Item -ItemType Directory -Force -Path $ArmRoot | Out-Null
    Expand-Archive -LiteralPath $Archive -DestinationPath $ArmRoot -Force
}

if ((-not (Test-Path -LiteralPath $CMake)) -or
    (-not (Test-Path -LiteralPath $Ninja)) -or
    (-not (Test-Path -LiteralPath $ArmCompiler))) {
    throw "The project-local toolchain is incomplete."
}

& $CMake --version
& $Ninja --version
& $ArmCompiler --version

if (Test-Path -LiteralPath $OpenOcd) {
    Write-Output "Project-local OpenOCD: $OpenOcd"
} else {
    Write-Warning "OpenOCD is missing. Restore .tools\openocd from the project tool backup before flashing or debugging."
}
