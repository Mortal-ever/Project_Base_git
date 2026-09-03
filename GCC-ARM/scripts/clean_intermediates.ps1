# 一键清理 GCC 中间文件（由 一键清理中间文件.bat 调用）
# 只清理 GCC-ARM 侧构建产物；MDK/Keil 侧的文件完全不动。
# 删除项全部为可再生成的构建产物，且已被 .gitignore 排除。

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)   # scripts/ -> GCC-ARM/ -> 工程根

$targets = @(
    "GCC-ARM\build",
    "GCC-ARM\.tools\downloads"
)

Write-Host "============================================================"
Write-Host "  一键清理 GCC 中间文件"
Write-Host "  工程根目录: $Root"
Write-Host "  本脚本只清理 GCC-ARM 侧（MDK/Keil 不受影响）"
Write-Host "============================================================"
Write-Host ""
Write-Host "  以下目录将被删除（均为可再生成的构建产物）:"
Write-Host "    GCC-ARM\build               CMake/Ninja 构建产物"
Write-Host "    GCC-ARM\.tools\downloads    工具链下载缓存（可选）"
Write-Host ""
Write-Host "============================================================"
Write-Host "  正在计算并删除..."

$total = 0
foreach ($t in $targets) {
    $full = Join-Path $Root $t
    if (Test-Path -LiteralPath $full) {
        $size = (Get-ChildItem -LiteralPath $full -Recurse -File -ErrorAction SilentlyContinue |
            Measure-Object -Property Length -Sum).Sum
        $total += $size
        Remove-Item -LiteralPath $full -Recurse -Force
        Write-Host ("  [已删除] {0}  ({1:N2} MB)" -f $t, ($size / 1MB))
    } else {
        Write-Host ("  [跳过]   {0}  (不存在)" -f $t)
    }
}

Write-Host ""
Write-Host ("  本次共释放：{0:N2} MB" -f ($total / 1MB))
Write-Host "============================================================"
Write-Host "  保留（不要删除，删除后需重新下载/配置）："
Write-Host "    GCC-ARM\.tools\              工具链本体"
Write-Host "      arm-gnu-toolchain\    GNU Arm GCC + GDB（约 1.1 GB）"
Write-Host "      python\               CMake + Ninja（约 92 MB）"
Write-Host "      openocd\              调试服务器（约 8 MB）"
Write-Host "    GCC-ARM\CMakeLists.txt, CMakePresets.json, cmake\, linker\"
Write-Host "             Platform\, scripts\, startup_stm32f407xx.s"
Write-Host "    全部源码与 资料文档\"
Write-Host "============================================================"
Write-Host ""
Write-Host "  如何恢复 GCC 构建（无需重新下载工具链）："
Write-Host "    powershell -File .\GCC-ARM\scripts\build.ps1 -Product Coffee2 -Configuration Debug"
Write-Host ""
Write-Host "  提示：GCC-ARM\.tools\downloads 是下载缓存，仅 setup_toolchain.ps1 复用。"
Write-Host "        若换机需要重新安装工具链，可保留它避免重新下载压缩包。"
Write-Host ""