<#
.SYNOPSIS
    构建 EvernightCommit（Windows x64 Git GUI，C++20 / CMake）。
.DESCRIPTION
    使用 CMakePresets 构建，默认 Visual Studio 2026 生成器 + Release，无需开发者命令提示符。
    会自动选择一个真正提供该生成器的 cmake.exe：PATH 上的 `cmake` 可能是 MSYS2 构建，
    它不含任何 Visual Studio 生成器，直接拿去配置会在第一步就报“未知生成器”。
    共用逻辑在 scripts\windows\cmake-selection.ps1（test.ps1 也用同一份）。
    产物：build\<preset>-x64\bin\<Config>\EvernightCommit.exe
    Linux 侧的对应入口是根目录的 configure.sh / build.sh / test.sh / run.sh / debug.sh。
.EXAMPLE
    .\build.ps1                 # vs2026 + Release
    .\build.ps1 -Preset vs2022  # 只有 VS 2022 的机器
    .\build.ps1 -Config Debug   # Debug 构建（含测试程序 gc_tests.exe）
    .\build.ps1 -Preset ninja   # 需先进入 Developer Command Prompt / vcvars64.bat
    .\build.ps1 -CMake 'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
#>
[CmdletBinding()]
param(
    [ValidateSet('vs2026', 'vs2022', 'ninja')]
    [string]$Preset = 'vs2026',
    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Release',
    [string]$CMake
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
. (Join-Path $root 'scripts\windows\cmake-selection.ps1')

$requiredGenerator = Get-GcRequiredGenerator -Preset $Preset
$tool = Resolve-GcCmake -Generator $requiredGenerator -Explicit $CMake
$cmakeExe = $tool.CMake
Write-Host "使用 CMake：$cmakeExe（$(((& $cmakeExe --version | Select-Object -First 1) -join '').Trim())）" -ForegroundColor DarkCyan

# ninja 预设的构建目录名就是 ninja-x64，vs 预设是 vs2026-x64 / vs2022-x64。
$configPreset = "${Preset}-x64"

Write-Host "配置：cmake --preset $configPreset" -ForegroundColor Cyan
& $cmakeExe --preset $configPreset -S $root
if ($LASTEXITCODE -ne 0) { throw "cmake 配置失败（退出码 $LASTEXITCODE）。" }

Write-Host "构建：cmake --build --preset ${Preset}-$($Config.ToLower())" -ForegroundColor Cyan
& $cmakeExe --build --preset "${Preset}-$($Config.ToLower())"
if ($LASTEXITCODE -ne 0) { throw "cmake 构建失败（退出码 $LASTEXITCODE）。" }

$exe = Join-Path $root "build\$configPreset\bin\$Config\EvernightCommit.exe"
if (-not (Test-Path -LiteralPath $exe)) { throw "构建完成但找不到产物：$exe" }
Write-Host "`n产物: $exe" -ForegroundColor Green
